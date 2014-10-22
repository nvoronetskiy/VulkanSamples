/*
 * XGL
 *
 * Copyright (C) 2014 LunarG, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 * Authors:
 *   LunarG
 */

#include "gpu.h"
#include "pipeline.h"
#include "compiler/shader/compiler_interface.h"
#include "compiler/pipeline/pipeline_compiler_interface.h"
#include "compiler/pipeline/brw_blorp_blit_eu.h"
#include "compiler/pipeline/brw_blorp.h"

static void initialize_brw_context(struct brw_context *brw,
                                   const struct intel_gpu *gpu)
{

    // create a stripped down context for compilation
    initialize_mesa_context_to_defaults(&brw->ctx);

    //
    // init the things pulled from DRI in brwCreateContext
    //
    struct brw_device_info *devInfo = rzalloc(brw, struct brw_device_info);
    switch (intel_gpu_gen(gpu)) {
    case INTEL_GEN(7.5):
        devInfo->gen = 7;
        devInfo->is_haswell = true;
        break;
    case INTEL_GEN(7):
        devInfo->gen = 7;
        break;
    case INTEL_GEN(6):
        devInfo->gen = 6;
        break;
    default:
        assert(!"unsupported GEN");
        break;
    }

    devInfo->gt = gpu->gt;
    devInfo->has_llc = true;
    devInfo->has_pln = true;
    devInfo->has_compr4 = true;
    devInfo->has_negative_rhw_bug = false;
    devInfo->needs_unlit_centroid_workaround = true;

    // hand code values until we have something to pull from
    // use brw_device_info_hsw_gt3
    brw->intelScreen = rzalloc(brw, struct intel_screen);
    brw->intelScreen->devinfo = devInfo;

    brw->gen = brw->intelScreen->devinfo->gen;
    brw->gt = brw->intelScreen->devinfo->gt;
    brw->is_g4x = brw->intelScreen->devinfo->is_g4x;
    brw->is_baytrail = brw->intelScreen->devinfo->is_baytrail;
    brw->is_haswell = brw->intelScreen->devinfo->is_haswell;
    brw->has_llc = brw->intelScreen->devinfo->has_llc;
    brw->has_pln = brw->intelScreen->devinfo->has_pln;
    brw->has_compr4 = brw->intelScreen->devinfo->has_compr4;
    brw->has_negative_rhw_bug = brw->intelScreen->devinfo->has_negative_rhw_bug;
    brw->needs_unlit_centroid_workaround =
       brw->intelScreen->devinfo->needs_unlit_centroid_workaround;

    brw->vs.base.stage = MESA_SHADER_VERTEX;
    brw->gs.base.stage = MESA_SHADER_GEOMETRY;
    brw->wm.base.stage = MESA_SHADER_FRAGMENT;

    //
    // init what remains of intel_screen
    //
    brw->intelScreen->deviceID = 0;
    brw->intelScreen->program_id = 0;

    brw_vec4_alloc_reg_set(brw->intelScreen);

    brw->shader_prog = brw_new_shader_program(&brw->ctx, 0);
}

class intel_meta_compiler : public brw_blorp_eu_emitter
{
public:
    intel_meta_compiler(struct brw_context *brw,
                        enum intel_dev_meta_shader id);
    void *compile(brw_blorp_prog_data *prog_data, uint32_t *code_size);

private:
    void alloc_regs();
    void alloc_pcb_regs(int grf);

    void emit_compute_frag_coord();
    void emit_copy_mem();
    void emit_clear_color();
    void emit_clear_depth();
    void *codegen(uint32_t *code_size);

    struct brw_context *brw;
    enum intel_dev_meta_shader id;

    const struct brw_reg poison;
    const struct brw_reg r0;
    const struct brw_reg r1;
    const int base_grf;
    const int base_mrf;

    struct brw_reg clear_vals[4];

    struct brw_reg src_offset_x;
    struct brw_reg src_offset_y;
    struct brw_reg src_layer;
    struct brw_reg src_lod;

    struct brw_reg dst_mem_offset;
    struct brw_reg dst_extent_width;

    struct brw_reg frag_x;
    struct brw_reg frag_y;

    struct brw_reg texels[4];

    struct brw_reg tmp1;
    struct brw_reg tmp2;
};

intel_meta_compiler::intel_meta_compiler(struct brw_context *brw,
                                         enum intel_dev_meta_shader id)
    : brw_blorp_eu_emitter(brw), brw(brw), id(id),
      poison(brw_imm_ud(0x12345678)),
      r0(retype(brw_vec8_grf(0, 0), BRW_REGISTER_TYPE_UW)),
      r1(retype(brw_vec8_grf(1, 0), BRW_REGISTER_TYPE_UW)),
      base_grf(2), /* skipping r0 and r1 */
      base_mrf(2)
{
    int i;

    for (i = 0; i < ARRAY_SIZE(clear_vals); i++)
        clear_vals[i] = poison;

    src_offset_x = poison;
    src_offset_y = poison;
    src_layer = poison;
    src_lod = poison;

    dst_mem_offset = poison;
    dst_extent_width = poison;
}

void intel_meta_compiler::alloc_pcb_regs(int grf)
{
    /* clears are special */
    switch (id) {
    case INTEL_DEV_META_FS_CLEAR_COLOR:
    case INTEL_DEV_META_FS_CLEAR_DEPTH:
        clear_vals[0] = retype(brw_vec1_grf(grf, 0), BRW_REGISTER_TYPE_UD);
        clear_vals[1] = retype(brw_vec1_grf(grf, 1), BRW_REGISTER_TYPE_UD);
        clear_vals[2] = retype(brw_vec1_grf(grf, 2), BRW_REGISTER_TYPE_UD);
        clear_vals[3] = retype(brw_vec1_grf(grf, 3), BRW_REGISTER_TYPE_UD);
        return;
    default:
        break;
    }

    src_offset_x = retype(brw_vec1_grf(grf, 0), BRW_REGISTER_TYPE_UD);
    src_offset_y = retype(brw_vec1_grf(grf, 1), BRW_REGISTER_TYPE_UD);

    switch (id) {
    case INTEL_DEV_META_FS_COPY_MEM:
    case INTEL_DEV_META_FS_COPY_1D:
    case INTEL_DEV_META_FS_COPY_1D_ARRAY:
    case INTEL_DEV_META_FS_COPY_2D:
    case INTEL_DEV_META_FS_COPY_2D_ARRAY:
    case INTEL_DEV_META_FS_COPY_2D_MS:
        src_layer = retype(brw_vec1_grf(grf, 2), BRW_REGISTER_TYPE_UD);
        src_lod = retype(brw_vec1_grf(grf, 3), BRW_REGISTER_TYPE_UD);
        break;
    case INTEL_DEV_META_FS_COPY_1D_TO_MEM:
    case INTEL_DEV_META_FS_COPY_1D_ARRAY_TO_MEM:
    case INTEL_DEV_META_FS_COPY_2D_TO_MEM:
    case INTEL_DEV_META_FS_COPY_2D_ARRAY_TO_MEM:
    case INTEL_DEV_META_FS_COPY_2D_MS_TO_MEM:
        src_layer = retype(brw_vec1_grf(grf, 2), BRW_REGISTER_TYPE_UD);
        src_lod = retype(brw_vec1_grf(grf, 3), BRW_REGISTER_TYPE_UD);
        dst_mem_offset = retype(brw_vec1_grf(grf, 4), BRW_REGISTER_TYPE_UD);
        dst_extent_width = retype(brw_vec1_grf(grf, 5), BRW_REGISTER_TYPE_UD);
        break;
    case INTEL_DEV_META_FS_COPY_MEM_TO_IMG:
        dst_extent_width = retype(brw_vec1_grf(grf, 2), BRW_REGISTER_TYPE_UD);
        break;
    case INTEL_DEV_META_FS_RESOLVE_2X:
    case INTEL_DEV_META_FS_RESOLVE_4X:
    case INTEL_DEV_META_FS_RESOLVE_8X:
    case INTEL_DEV_META_FS_RESOLVE_16X:
        break;
    default:
        break;
    }
}

void intel_meta_compiler::alloc_regs(void)
{
    int grf = base_grf;
    int i;

    alloc_pcb_regs(grf);
    grf++;

    frag_x = retype(brw_vec8_grf(grf, 0), BRW_REGISTER_TYPE_UD);
    grf += 2;

    frag_y = retype(brw_vec8_grf(grf, 0), BRW_REGISTER_TYPE_UD);
    grf += 2;

    for (i = 0; i < ARRAY_SIZE(texels); i++) {
        texels[i] = retype(vec16(brw_vec8_grf(grf, 0)), BRW_REGISTER_TYPE_UD);
        grf += 8;
    }

    tmp1 = retype(brw_vec8_grf(grf, 0), BRW_REGISTER_TYPE_UD);
    grf += 2;

    tmp2 = retype(brw_vec8_grf(grf, 0), BRW_REGISTER_TYPE_UD);
    grf += 2;
}

void intel_meta_compiler::emit_compute_frag_coord()
{
    emit_add(vec16(retype(frag_x, BRW_REGISTER_TYPE_UW)),
            stride(suboffset(r1, 4), 2, 4, 0), brw_imm_v(0x10101010));
    emit_add(vec16(retype(frag_y, BRW_REGISTER_TYPE_UW)),
            stride(suboffset(r1, 5), 2, 4, 0), brw_imm_v(0x11001100));
}

void intel_meta_compiler::emit_copy_mem()
{
    struct brw_reg mrf = retype(vec16(brw_message_reg(base_mrf)),
                                BRW_REGISTER_TYPE_UD);
    int mrf_offset = 0;
    int i;

    emit_compute_frag_coord();

    emit_add(offset(mrf, mrf_offset),
            retype(frag_x, BRW_REGISTER_TYPE_UW),
            retype(src_offset_x, BRW_REGISTER_TYPE_UW));
    mrf_offset += 2;

    emit_texture_lookup(texels[0], SHADER_OPCODE_TXF, base_mrf, mrf_offset);

    mrf_offset = 0;
    for (i = 0; i < 4; i++) {
        emit_mov(offset(mrf, mrf_offset), offset(texels[0], i * 2));
        mrf_offset += 2;
    }
    emit_render_target_write(mrf, base_mrf, mrf_offset, false);
}

void intel_meta_compiler::emit_clear_color()
{
    struct brw_reg mrf = retype(vec16(brw_message_reg(base_mrf)),
                                BRW_REGISTER_TYPE_UD);
    int mrf_offset = 0;
    int i;

    for (i = 0; i < 4; i++) {
        emit_mov(offset(mrf, mrf_offset), clear_vals[i]);
        mrf_offset += 2;
    }

    emit_render_target_write(mrf, base_mrf, mrf_offset, false);
}

void intel_meta_compiler::emit_clear_depth()
{
    struct brw_reg mrf = retype(vec16(brw_message_reg(base_mrf)),
                                BRW_REGISTER_TYPE_UD);
    /* skip colors */
    int mrf_offset = 4 * 2;

    /* oDepth */
    emit_mov(offset(mrf, mrf_offset), clear_vals[0]);
    mrf_offset += 2;

    emit_render_target_write(mrf, base_mrf, mrf_offset, false);
}

void *intel_meta_compiler::codegen(uint32_t *code_size)
{
    const unsigned *prog;
    unsigned prog_size;
    void *code;

    prog = get_program(&prog_size, stderr);

    code = icd_alloc(prog_size, 0, XGL_SYSTEM_ALLOC_INTERNAL);
    if (!code)
        return NULL;

    memcpy(code, prog, prog_size);
    if (code_size)
        *code_size = prog_size;

    return code;
}

void *intel_meta_compiler::compile(brw_blorp_prog_data *prog_data,
                                   uint32_t *code_size)
{
    memset(prog_data, 0, sizeof(*prog_data));
    prog_data->first_curbe_grf = base_grf;

    alloc_regs();

    switch (id) {
    case INTEL_DEV_META_FS_COPY_MEM:
        emit_copy_mem();
        break;
    case INTEL_DEV_META_FS_COPY_1D:
    case INTEL_DEV_META_FS_COPY_1D_ARRAY:
    case INTEL_DEV_META_FS_COPY_2D:
    case INTEL_DEV_META_FS_COPY_2D_ARRAY:
    case INTEL_DEV_META_FS_COPY_2D_MS:
        emit_clear_color();
        break;
    case INTEL_DEV_META_FS_COPY_1D_TO_MEM:
    case INTEL_DEV_META_FS_COPY_1D_ARRAY_TO_MEM:
    case INTEL_DEV_META_FS_COPY_2D_TO_MEM:
    case INTEL_DEV_META_FS_COPY_2D_ARRAY_TO_MEM:
    case INTEL_DEV_META_FS_COPY_2D_MS_TO_MEM:
        emit_clear_color();
        break;
    case INTEL_DEV_META_FS_COPY_MEM_TO_IMG:
        emit_clear_color();
        break;
    case INTEL_DEV_META_FS_CLEAR_COLOR:
        emit_clear_color();
        break;
    case INTEL_DEV_META_FS_CLEAR_DEPTH:
        emit_clear_depth();
        break;
    case INTEL_DEV_META_FS_RESOLVE_2X:
    case INTEL_DEV_META_FS_RESOLVE_4X:
    case INTEL_DEV_META_FS_RESOLVE_8X:
    case INTEL_DEV_META_FS_RESOLVE_16X:
        emit_clear_color();
        break;
    default:
        emit_clear_color();
        break;
    }

    return codegen(code_size);
}

extern "C" {

XGL_RESULT intel_pipeline_shader_compile_meta(struct intel_pipeline_shader *sh,
                                              const struct intel_gpu *gpu,
                                              enum intel_dev_meta_shader id)
{
    struct brw_context *brw = rzalloc(NULL, struct brw_context);

    initialize_brw_context(brw, gpu);

    intel_meta_compiler c(brw, id);
    brw_blorp_prog_data prog_data;

    sh->pCode = c.compile(&prog_data, &sh->codeSize);

    sh->out_count = 1;
    sh->surface_count = BRW_BLORP_NUM_BINDING_TABLE_ENTRIES;
    sh->urb_grf_start = prog_data.first_curbe_grf;

    switch (id) {
    case INTEL_DEV_META_FS_CLEAR_DEPTH:
        sh->uses = INTEL_SHADER_USE_COMPUTED_DEPTH;
        break;
    default:
        sh->uses = 0;
        break;
    }

    ralloc_free(brw);
    return (sh->pCode) ? XGL_SUCCESS : XGL_ERROR_UNKNOWN;
}

} // extern "C"
