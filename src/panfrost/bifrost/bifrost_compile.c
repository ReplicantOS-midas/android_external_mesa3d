/*
 * Copyright (C) 2020 Collabora Ltd.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 * Authors (Collabora):
 *      Alyssa Rosenzweig <alyssa.rosenzweig@collabora.com>
 */

#include "main/mtypes.h"
#include "compiler/glsl/glsl_to_nir.h"
#include "compiler/nir_types.h"
#include "compiler/nir/nir_builder.h"
#include "util/u_debug.h"

#include "disassemble.h"
#include "bifrost_compile.h"
#include "bifrost_nir.h"
#include "compiler.h"
#include "bi_quirks.h"
#include "bi_builder.h"

static const struct debug_named_value bifrost_debug_options[] = {
        {"msgs",      BIFROST_DBG_MSGS,		"Print debug messages"},
        {"shaders",   BIFROST_DBG_SHADERS,	"Dump shaders in NIR and MIR"},
        {"shaderdb",  BIFROST_DBG_SHADERDB,	"Print statistics"},
        {"verbose",   BIFROST_DBG_VERBOSE,	"Disassemble verbosely"},
        {"internal",  BIFROST_DBG_INTERNAL,	"Dump even internal shaders"},
        {"nosched",   BIFROST_DBG_NOSCHED, 	"Force trivial scheduling"},
        DEBUG_NAMED_VALUE_END
};

DEBUG_GET_ONCE_FLAGS_OPTION(bifrost_debug, "BIFROST_MESA_DEBUG", bifrost_debug_options, 0)

/* How many bytes are prefetched by the Bifrost shader core. From the final
 * clause of the shader, this range must be valid instructions or zero. */
#define BIFROST_SHADER_PREFETCH 128

int bifrost_debug = 0;

#define DBG(fmt, ...) \
		do { if (bifrost_debug & BIFROST_DBG_MSGS) \
			fprintf(stderr, "%s:%d: "fmt, \
				__FUNCTION__, __LINE__, ##__VA_ARGS__); } while (0)

static bi_block *emit_cf_list(bi_context *ctx, struct exec_list *list);

static void
bi_emit_jump(bi_builder *b, nir_jump_instr *instr)
{
        bi_instr *branch = bi_jump(b, bi_zero());

        switch (instr->type) {
        case nir_jump_break:
                branch->branch_target = b->shader->break_block;
                break;
        case nir_jump_continue:
                branch->branch_target = b->shader->continue_block;
                break;
        default:
                unreachable("Unhandled jump type");
        }

        pan_block_add_successor(&b->shader->current_block->base, &branch->branch_target->base);
        b->shader->current_block->base.unconditional_jumps = true;
}

static bi_index
bi_varying_src0_for_barycentric(bi_builder *b, nir_intrinsic_instr *intr)
{
        switch (intr->intrinsic) {
        case nir_intrinsic_load_barycentric_centroid:
        case nir_intrinsic_load_barycentric_sample:
                return bi_register(61);

        /* Need to put the sample ID in the top 16-bits */
        case nir_intrinsic_load_barycentric_at_sample:
                return bi_mkvec_v2i16(b, bi_half(bi_dontcare(), false),
                                bi_half(bi_src_index(&intr->src[0]), false));

        /* Interpret as 8:8 signed fixed point positions in pixels along X and
         * Y axes respectively, relative to top-left of pixel. In NIR, (0, 0)
         * is the center of the pixel so we first fixup and then convert. For
         * fp16 input:
         *
         * f2i16(((x, y) + (0.5, 0.5)) * 2**8) =
         * f2i16((256 * (x, y)) + (128, 128)) =
         * V2F16_TO_V2S16(FMA.v2f16((x, y), #256, #128))
         *
         * For fp32 input, that lacks enough precision for MSAA 16x, but the
         * idea is the same. FIXME: still doesn't pass
         */
        case nir_intrinsic_load_barycentric_at_offset: {
                bi_index offset = bi_src_index(&intr->src[0]);
                bi_index f16 = bi_null();
                unsigned sz = nir_src_bit_size(intr->src[0]);

                if (sz == 16) {
                        f16 = bi_fma_v2f16(b, offset, bi_imm_f16(256.0),
                                        bi_imm_f16(128.0), BI_ROUND_NONE);
                } else {
                        assert(sz == 32);
                        bi_index f[2];
                        for (unsigned i = 0; i < 2; ++i) {
                                f[i] = bi_fadd_rscale_f32(b,
                                                bi_word(offset, i),
                                                bi_imm_f32(0.5), bi_imm_u32(8),
                                                BI_ROUND_NONE, BI_SPECIAL_NONE);
                        }

                        f16 = bi_v2f32_to_v2f16(b, f[0], f[1], BI_ROUND_NONE);
                }

                return bi_v2f16_to_v2s16(b, f16, BI_ROUND_RTZ);
        }

        case nir_intrinsic_load_barycentric_pixel:
        default:
                return bi_dontcare();
        }
}

static enum bi_sample
bi_interp_for_intrinsic(nir_intrinsic_op op)
{
        switch (op) {
        case nir_intrinsic_load_barycentric_centroid:
                return BI_SAMPLE_CENTROID;
        case nir_intrinsic_load_barycentric_sample:
        case nir_intrinsic_load_barycentric_at_sample:
                return BI_SAMPLE_SAMPLE;
        case nir_intrinsic_load_barycentric_at_offset:
                return BI_SAMPLE_EXPLICIT;
        case nir_intrinsic_load_barycentric_pixel:
        default:
                return BI_SAMPLE_CENTER;
        }
}

/* auto, 64-bit omitted */
static enum bi_register_format
bi_reg_fmt_for_nir(nir_alu_type T)
{
        switch (T) {
        case nir_type_float16: return BI_REGISTER_FORMAT_F16;
        case nir_type_float32: return BI_REGISTER_FORMAT_F32;
        case nir_type_int16:   return BI_REGISTER_FORMAT_S16;
        case nir_type_uint16:  return BI_REGISTER_FORMAT_U16;
        case nir_type_int32:   return BI_REGISTER_FORMAT_S32;
        case nir_type_uint32:  return BI_REGISTER_FORMAT_U32;
        default: unreachable("Invalid type for register format");
        }
}

/* Checks if the _IMM variant of an intrinsic can be used, returning in imm the
 * immediate to be used (which applies even if _IMM can't be used) */

static bool
bi_is_intr_immediate(nir_intrinsic_instr *instr, unsigned *immediate, unsigned max)
{
        nir_src *offset = nir_get_io_offset_src(instr);

        if (!nir_src_is_const(*offset))
                return false;

        *immediate = nir_intrinsic_base(instr) + nir_src_as_uint(*offset);
        return (*immediate) < max;
}

static void
bi_emit_load_attr(bi_builder *b, nir_intrinsic_instr *instr)
{
        nir_alu_type T = nir_intrinsic_dest_type(instr);
        enum bi_register_format regfmt = bi_reg_fmt_for_nir(T);
        nir_src *offset = nir_get_io_offset_src(instr);
        unsigned imm_index = 0;
        unsigned base = nir_intrinsic_base(instr);
        bool constant = nir_src_is_const(*offset);
        bool immediate = bi_is_intr_immediate(instr, &imm_index, 16);

        if (immediate) {
                bi_ld_attr_imm_to(b, bi_dest_index(&instr->dest),
                                bi_register(61), /* TODO RA */
                                bi_register(62), /* TODO RA */
                                regfmt, instr->num_components - 1, imm_index);
        } else {
                bi_index idx = bi_src_index(&instr->src[0]);

                if (constant)
                        idx = bi_imm_u32(imm_index);
                else if (base != 0)
                        idx = bi_iadd_u32(b, idx, bi_imm_u32(base), false);

                bi_ld_attr_to(b, bi_dest_index(&instr->dest),
                                bi_register(61), /* TODO RA */
                                bi_register(62), /* TODO RA */
                                idx, regfmt, instr->num_components - 1);
        }
}

static void
bi_emit_load_vary(bi_builder *b, nir_intrinsic_instr *instr)
{
        enum bi_sample sample = BI_SAMPLE_CENTER;
        enum bi_update update = BI_UPDATE_STORE;
        enum bi_register_format regfmt = BI_REGISTER_FORMAT_AUTO;
        enum bi_vecsize vecsize = instr->num_components - 1;
        bool smooth = instr->intrinsic == nir_intrinsic_load_interpolated_input;
        bi_index src0 = bi_null();

        if (smooth) {
                nir_intrinsic_instr *parent = nir_src_as_intrinsic(instr->src[0]);
                assert(parent);

                sample = bi_interp_for_intrinsic(parent->intrinsic);
                src0 = bi_varying_src0_for_barycentric(b, parent);
        } else {
                regfmt = bi_reg_fmt_for_nir(nir_intrinsic_dest_type(instr));
        }

        nir_src *offset = nir_get_io_offset_src(instr);
        unsigned imm_index = 0;
        bool immediate = bi_is_intr_immediate(instr, &imm_index, 20);

        if (immediate && smooth) {
                bi_ld_var_imm_to(b, bi_dest_index(&instr->dest),
                                src0, regfmt, sample, update, vecsize,
                                imm_index);
        } else if (immediate && !smooth) {
                bi_ld_var_flat_imm_to(b, bi_dest_index(&instr->dest),
                                BI_FUNCTION_NONE, regfmt, vecsize, imm_index);
        } else {
                bi_index idx = bi_src_index(offset);
                unsigned base = nir_intrinsic_base(instr);

                if (base != 0)
                        idx = bi_iadd_u32(b, idx, bi_imm_u32(base), false);

                if (smooth) {
                        bi_ld_var_to(b, bi_dest_index(&instr->dest),
                                        src0, idx, regfmt, sample, update,
                                        vecsize);
                } else {
                        bi_ld_var_flat_to(b, bi_dest_index(&instr->dest),
                                        idx, BI_FUNCTION_NONE, regfmt,
                                        vecsize);
                }
        }
}

static void
bi_make_vec_to(bi_builder *b, bi_index final_dst,
                bi_index *src,
                unsigned *channel,
                unsigned count,
                unsigned bitsize)
{
        /* If we reads our own output, we need a temporary move to allow for
         * swapping. TODO: Could do a bit better for pairwise swaps of 16-bit
         * vectors */
        bool reads_self = false;

        for (unsigned i = 0; i < count; ++i)
                reads_self |= bi_is_equiv(final_dst, src[i]);

        /* SSA can't read itself */
        assert(!reads_self || final_dst.reg);

        bi_index dst = reads_self ? bi_temp(b->shader) : final_dst;

        if (bitsize == 32) {
                for (unsigned i = 0; i < count; ++i) {
                        bi_mov_i32_to(b, bi_word(dst, i),
                                        bi_word(src[i], channel ? channel[i] : 0));
                }
        } else if (bitsize == 16) {
                for (unsigned i = 0; i < count; i += 2) {
                        unsigned chan = channel ? channel[i] : 0;

                        bi_index w0 = bi_half(bi_word(src[i], chan >> 1), chan & 1);
                        bi_index w1 = bi_imm_u16(0);

                        /* Don't read out of bound for vec3 */
                        if ((i + 1) < count) {
                                unsigned nextc = channel ? channel[i + 1] : 0;
                                w1 = bi_half(bi_word(src[i + 1], nextc >> 1), nextc & 1);
                        }

                        bi_mkvec_v2i16_to(b, bi_word(dst, i >> 1), w0, w1);
                }
        } else {
                unreachable("8-bit mkvec not yet supported");
        }

        /* Emit an explicit copy if needed */
        if (!bi_is_equiv(dst, final_dst)) {
                unsigned shift = (bitsize == 8) ? 2 : (bitsize == 16) ? 1 : 0;
                unsigned vec = (1 << shift);

                for (unsigned i = 0; i < count; i += vec) {
                        bi_mov_i32_to(b, bi_word(final_dst, i >> shift),
                                        bi_word(dst, i >> shift));
                }
        }
}

static bi_instr *
bi_load_sysval_to(bi_builder *b, bi_index dest, int sysval,
                unsigned nr_components, unsigned offset)
{
        unsigned sysval_ubo =
                MAX2(b->shader->inputs->sysval_ubo, b->shader->nir->info.num_ubos);
        unsigned uniform =
                pan_lookup_sysval(b->shader->sysval_to_id,
                                  &b->shader->info->sysvals,
                                  sysval);
        unsigned idx = (uniform * 16) + offset;

        return bi_load_to(b, nr_components * 32, dest,
                        bi_imm_u32(idx),
                        bi_imm_u32(sysval_ubo), BI_SEG_UBO);
}

static void
bi_load_sysval_nir(bi_builder *b, nir_intrinsic_instr *intr,
                unsigned nr_components, unsigned offset)
{
        bi_load_sysval_to(b, bi_dest_index(&intr->dest),
                        panfrost_sysval_for_instr(&intr->instr, NULL),
                        nr_components, offset);
}

static bi_index
bi_load_sysval(bi_builder *b, int sysval,
                unsigned nr_components, unsigned offset)
{
        bi_index tmp = bi_temp(b->shader);
        bi_load_sysval_to(b, tmp, sysval, nr_components, offset);
        return tmp;
}

static void
bi_emit_load_blend_input(bi_builder *b, nir_intrinsic_instr *instr)
{
        ASSERTED nir_io_semantics sem = nir_intrinsic_io_semantics(instr);

        /* Source color is passed through r0-r3, or r4-r7 for the second
         * source when dual-source blending.  TODO: Precolour instead */
        bi_index srcs[] = {
                bi_register(0), bi_register(1), bi_register(2), bi_register(3)
        };
        bi_index srcs2[] = {
                bi_register(4), bi_register(5), bi_register(6), bi_register(7)
        };

        bool second_source = (sem.location == VARYING_SLOT_VAR0);

        bi_make_vec_to(b, bi_dest_index(&instr->dest),
                       second_source ? srcs2 : srcs,
                       NULL, 4, 32);
}

static void
bi_emit_blend_op(bi_builder *b, bi_index rgba, nir_alu_type T, unsigned rt)
{
        if (b->shader->inputs->is_blend) {
                uint64_t blend_desc = b->shader->inputs->blend.bifrost_blend_desc;

                /* Blend descriptor comes from the compile inputs */
                /* Put the result in r0 */
                bi_blend_to(b, bi_register(0), rgba,
                                bi_register(60) /* TODO RA */,
                                bi_imm_u32(blend_desc & 0xffffffff),
                                bi_imm_u32(blend_desc >> 32));
        } else {
                /* Blend descriptor comes from the FAU RAM. By convention, the
                 * return address is stored in r48 and will be used by the
                 * blend shader to jump back to the fragment shader after */
                bi_blend_to(b, bi_register(48), rgba,
                                bi_register(60) /* TODO RA */,
                                bi_fau(BIR_FAU_BLEND_0 + rt, false),
                                bi_fau(BIR_FAU_BLEND_0 + rt, true));
        }

        assert(rt < 8);
        b->shader->info->bifrost.blend[rt].type = T;
}

/* Blend shaders do not need to run ATEST since they are dependent on a
 * fragment shader that runs it. Blit shaders may not need to run ATEST, since
 * ATEST is not needed if early-z is forced, alpha-to-coverage is disabled, and
 * there are no writes to the coverage mask. The latter two are satisfied for
 * all blit shaders, so we just care about early-z, which blit shaders force
 * iff they do not write depth or stencil */

static bool
bi_skip_atest(bi_context *ctx, bool emit_zs)
{
        return (ctx->inputs->is_blit && !emit_zs) || ctx->inputs->is_blend;
}

static void
bi_emit_fragment_out(bi_builder *b, nir_intrinsic_instr *instr)
{
        bool combined = instr->intrinsic ==
                nir_intrinsic_store_combined_output_pan;

        unsigned writeout = combined ? nir_intrinsic_component(instr) :
                PAN_WRITEOUT_C;

        bool emit_blend = writeout & (PAN_WRITEOUT_C);
        bool emit_zs = writeout & (PAN_WRITEOUT_Z | PAN_WRITEOUT_S);

        const nir_variable *var =
                nir_find_variable_with_driver_location(b->shader->nir,
                                nir_var_shader_out, nir_intrinsic_base(instr));
        assert(var);

        unsigned loc = var->data.location;
        bi_index src0 = bi_src_index(&instr->src[0]);

        /* By ISA convention, the coverage mask is stored in R60. The store
         * itself will be handled by a subsequent ATEST instruction */
        if (loc == FRAG_RESULT_SAMPLE_MASK) {
                bi_index orig = bi_register(60);
                bi_index msaa = bi_load_sysval(b, PAN_SYSVAL_MULTISAMPLED, 1, 0);
                bi_index new = bi_lshift_and_i32(b, orig, src0, bi_imm_u8(0));
                bi_mux_i32_to(b, orig, orig, new, msaa, BI_MUX_INT_ZERO);
                return;
        }


        /* Dual-source blending is implemented by putting the color in
         * registers r4-r7. */
        if (var->data.index) {
                unsigned count = nir_src_num_components(instr->src[0]);

                for (unsigned i = 0; i < count; ++i)
                        bi_mov_i32_to(b, bi_register(4 + i),
                                      bi_word(src0, i));
                return;
        }

        /* Emit ATEST if we have to, note ATEST requires a floating-point alpha
         * value, but render target #0 might not be floating point. However the
         * alpha value is only used for alpha-to-coverage, a stage which is
         * skipped for pure integer framebuffers, so the issue is moot. */

        if (!b->shader->emitted_atest && !bi_skip_atest(b->shader, emit_zs)) {
                nir_alu_type T = nir_intrinsic_src_type(instr);

                bi_index rgba = bi_src_index(&instr->src[0]);
                bi_index alpha =
                        (T == nir_type_float16) ? bi_half(bi_word(rgba, 1), true) :
                        (T == nir_type_float32) ? bi_word(rgba, 3) :
                        bi_dontcare();

                /* Don't read out-of-bounds */
                if (nir_src_num_components(instr->src[0]) < 4)
                        alpha = bi_imm_f32(1.0);

                bi_instr *atest = bi_atest_to(b, bi_register(60),
                                bi_register(60), alpha);
                b->shader->emitted_atest = true;

                /* Pseudo-source to encode in the tuple */
                atest->src[2] = bi_fau(BIR_FAU_ATEST_PARAM, false);
        }

        if (emit_zs) {
                bi_index z = { 0 }, s = { 0 };

                if (writeout & PAN_WRITEOUT_Z)
                        z = bi_src_index(&instr->src[2]);

                if (writeout & PAN_WRITEOUT_S)
                        s = bi_src_index(&instr->src[3]);

                bi_zs_emit_to(b, bi_register(60), z, s,
                                bi_register(60) /* TODO RA */,
                                writeout & PAN_WRITEOUT_S,
                                writeout & PAN_WRITEOUT_Z);
        }

        if (emit_blend) {
                assert(loc == FRAG_RESULT_COLOR || loc >= FRAG_RESULT_DATA0);

                unsigned rt = loc == FRAG_RESULT_COLOR ? 0 :
                        (loc - FRAG_RESULT_DATA0);
                bi_index color = bi_src_index(&instr->src[0]);

                /* Explicit copy since BLEND inputs are precoloured to R0-R3,
                 * TODO: maybe schedule around this or implement in RA as a
                 * spill */
                bool has_mrt = false;

                nir_foreach_shader_out_variable(var, b->shader->nir)
                        has_mrt |= (var->data.location > FRAG_RESULT_DATA0);

                if (has_mrt) {
                        bi_index srcs[4] = { color, color, color, color };
                        unsigned channels[4] = { 0, 1, 2, 3 };
                        color = bi_temp(b->shader);
                        bi_make_vec_to(b, color, srcs, channels,
                                       nir_src_num_components(instr->src[0]),
                                       nir_alu_type_get_type_size(nir_intrinsic_src_type(instr)));
                }

                bi_emit_blend_op(b, color, nir_intrinsic_src_type(instr), rt);
        }

        if (b->shader->inputs->is_blend) {
                /* Jump back to the fragment shader, return address is stored
                 * in r48 (see above).
                 */
                bi_jump(b, bi_register(48));
        }
}

static void
bi_emit_store_vary(bi_builder *b, nir_intrinsic_instr *instr)
{
        nir_alu_type T = nir_intrinsic_src_type(instr);
        enum bi_register_format regfmt = bi_reg_fmt_for_nir(T);

        unsigned imm_index = 0;
        bool immediate = bi_is_intr_immediate(instr, &imm_index, 16);

        bi_index address;
        if (immediate) {
                address = bi_lea_attr_imm(b,
                                          bi_register(61), /* TODO RA */
                                          bi_register(62), /* TODO RA */
                                          regfmt, imm_index);
        } else {
                bi_index idx =
                        bi_iadd_u32(b,
                                    bi_src_index(nir_get_io_offset_src(instr)),
                                    bi_imm_u32(nir_intrinsic_base(instr)),
                                    false);
                address = bi_lea_attr(b,
                                      bi_register(61), /* TODO RA */
                                      bi_register(62), /* TODO RA */
                                      idx, regfmt);
        }

        /* Only look at the total components needed. In effect, we fill in all
         * the intermediate "holes" in the write mask, since we can't mask off
         * stores. Since nir_lower_io_to_temporaries ensures each varying is
         * written at most once, anything that's masked out is undefined, so it
         * doesn't matter what we write there. So we may as well do the
         * simplest thing possible. */
        unsigned nr = util_last_bit(nir_intrinsic_write_mask(instr));
        assert(nr > 0 && nr <= nir_intrinsic_src_components(instr, 0));

        bi_st_cvt(b, bi_src_index(&instr->src[0]), address,
                        bi_word(address, 1), bi_word(address, 2),
                        regfmt, nr - 1);
}

static void
bi_emit_load_ubo(bi_builder *b, nir_intrinsic_instr *instr)
{
        nir_src *offset = nir_get_io_offset_src(instr);

        bool offset_is_const = nir_src_is_const(*offset);
        bi_index dyn_offset = bi_src_index(offset);
        uint32_t const_offset = offset_is_const ? nir_src_as_uint(*offset) : 0;
        bool kernel_input = (instr->intrinsic == nir_intrinsic_load_kernel_input);

        bi_load_to(b, instr->num_components * nir_dest_bit_size(instr->dest),
                        bi_dest_index(&instr->dest), offset_is_const ?
                        bi_imm_u32(const_offset) : dyn_offset,
                        kernel_input ? bi_zero() : bi_src_index(&instr->src[0]),
                        BI_SEG_UBO);
}

static bi_index
bi_addr_high(nir_src *src)
{
	return (nir_src_bit_size(*src) == 64) ?
		bi_word(bi_src_index(src), 1) : bi_zero();
}

static void
bi_emit_load(bi_builder *b, nir_intrinsic_instr *instr, enum bi_seg seg)
{
        bi_load_to(b, instr->num_components * nir_dest_bit_size(instr->dest),
                   bi_dest_index(&instr->dest),
                   bi_src_index(&instr->src[0]), bi_addr_high(&instr->src[0]),
                   seg);
}

static void
bi_emit_store(bi_builder *b, nir_intrinsic_instr *instr, enum bi_seg seg)
{
        bi_store(b, instr->num_components * nir_src_bit_size(instr->src[0]),
                    bi_src_index(&instr->src[0]),
                    bi_src_index(&instr->src[1]), bi_addr_high(&instr->src[1]),
                    seg);
}

/* Exchanges the staging register with memory */

static void
bi_emit_axchg_to(bi_builder *b, bi_index dst, bi_index addr, nir_src *arg, enum bi_seg seg)
{
        assert(seg == BI_SEG_NONE || seg == BI_SEG_WLS);

        unsigned sz = nir_src_bit_size(*arg);
        assert(sz == 32 || sz == 64);

        bi_index data = bi_src_index(arg);

        bi_index data_words[] = {
                bi_word(data, 0),
                bi_word(data, 1),
        };

        bi_index inout = bi_temp_reg(b->shader);
        bi_make_vec_to(b, inout, data_words, NULL, sz / 32, 32);

        bi_axchg_to(b, sz, inout, inout,
                        bi_word(addr, 0),
                        (seg == BI_SEG_NONE) ? bi_word(addr, 1) : bi_zero(),
                        seg);

        bi_index inout_words[] = {
                bi_word(inout, 0),
                bi_word(inout, 1),
        };

        bi_make_vec_to(b, dst, inout_words, NULL, sz / 32, 32);
}

/* Exchanges the second staging register with memory if comparison with first
 * staging register passes */

static void
bi_emit_acmpxchg_to(bi_builder *b, bi_index dst, bi_index addr, nir_src *arg_1, nir_src *arg_2, enum bi_seg seg)
{
        assert(seg == BI_SEG_NONE || seg == BI_SEG_WLS);

        /* hardware is swapped from NIR */
        bi_index src0 = bi_src_index(arg_2);
        bi_index src1 = bi_src_index(arg_1);

        unsigned sz = nir_src_bit_size(*arg_1);
        assert(sz == 32 || sz == 64);

        bi_index data_words[] = {
                bi_word(src0, 0),
                sz == 32 ? bi_word(src1, 0) : bi_word(src0, 1),

                /* 64-bit */
                bi_word(src1, 0),
                bi_word(src1, 1),
        };

        bi_index inout = bi_temp_reg(b->shader);
        bi_make_vec_to(b, inout, data_words, NULL, 2 * (sz / 32), 32);

        bi_acmpxchg_to(b, sz, inout, inout,
                        bi_word(addr, 0),
                        (seg == BI_SEG_NONE) ? bi_word(addr, 1) : bi_zero(),
                        seg);

        bi_index inout_words[] = {
                bi_word(inout, 0),
                bi_word(inout, 1),
        };

        bi_make_vec_to(b, dst, inout_words, NULL, sz / 32, 32);
}

/* Extracts an atomic opcode */

static enum bi_atom_opc
bi_atom_opc_for_nir(nir_intrinsic_op op)
{
        switch (op) {
        case nir_intrinsic_global_atomic_add:
        case nir_intrinsic_shared_atomic_add:
        case nir_intrinsic_image_atomic_add:
                return BI_ATOM_OPC_AADD;

        case nir_intrinsic_global_atomic_imin:
        case nir_intrinsic_shared_atomic_imin:
        case nir_intrinsic_image_atomic_imin:
                return BI_ATOM_OPC_ASMIN;

        case nir_intrinsic_global_atomic_umin:
        case nir_intrinsic_shared_atomic_umin:
        case nir_intrinsic_image_atomic_umin:
                return BI_ATOM_OPC_AUMIN;

        case nir_intrinsic_global_atomic_imax:
        case nir_intrinsic_shared_atomic_imax:
        case nir_intrinsic_image_atomic_imax:
                return BI_ATOM_OPC_ASMAX;

        case nir_intrinsic_global_atomic_umax:
        case nir_intrinsic_shared_atomic_umax:
        case nir_intrinsic_image_atomic_umax:
                return BI_ATOM_OPC_AUMAX;

        case nir_intrinsic_global_atomic_and:
        case nir_intrinsic_shared_atomic_and:
        case nir_intrinsic_image_atomic_and:
                return BI_ATOM_OPC_AAND;

        case nir_intrinsic_global_atomic_or:
        case nir_intrinsic_shared_atomic_or:
        case nir_intrinsic_image_atomic_or:
                return BI_ATOM_OPC_AOR;

        case nir_intrinsic_global_atomic_xor:
        case nir_intrinsic_shared_atomic_xor:
        case nir_intrinsic_image_atomic_xor:
                return BI_ATOM_OPC_AXOR;

        default:
                unreachable("Unexpected computational atomic");
        }
}

/* Optimized unary atomics are available with an implied #1 argument */

static bool
bi_promote_atom_c1(enum bi_atom_opc op, bi_index arg, enum bi_atom_opc *out)
{
        /* Check we have a compatible constant */
        if (arg.type != BI_INDEX_CONSTANT)
                return false;

        if (!(arg.value == 1 || (arg.value == -1 && op == BI_ATOM_OPC_AADD)))
                return false;

        /* Check for a compatible operation */
        switch (op) {
        case BI_ATOM_OPC_AADD:
                *out = (arg.value == 1) ? BI_ATOM_OPC_AINC : BI_ATOM_OPC_ADEC;
                return true;
        case BI_ATOM_OPC_ASMAX:
                *out = BI_ATOM_OPC_ASMAX1;
                return true;
        case BI_ATOM_OPC_AUMAX:
                *out = BI_ATOM_OPC_AUMAX1;
                return true;
        case BI_ATOM_OPC_AOR:
                *out = BI_ATOM_OPC_AOR1;
                return true;
        default:
                return false;
        }
}

/* Coordinates are 16-bit integers in Bifrost but 32-bit in NIR */

static bi_index
bi_emit_image_coord(bi_builder *b, bi_index coord)
{
        return bi_mkvec_v2i16(b,
                        bi_half(bi_word(coord, 0), false),
                        bi_half(bi_word(coord, 1), false));
}

static void
bi_emit_image_load(bi_builder *b, nir_intrinsic_instr *instr)
{
        enum glsl_sampler_dim dim = nir_intrinsic_image_dim(instr);
        ASSERTED unsigned nr_dim = glsl_get_sampler_dim_coordinate_components(dim);

        bi_index coords = bi_src_index(&instr->src[1]);
        /* TODO: MSAA */
        assert(nr_dim != GLSL_SAMPLER_DIM_MS && "MSAA'd images not supported");

        bi_ld_attr_tex_to(b, bi_dest_index(&instr->dest),
                          bi_emit_image_coord(b, coords),
                          bi_emit_image_coord(b, bi_word(coords, 2)),
                          bi_src_index(&instr->src[0]),
                          bi_reg_fmt_for_nir(nir_intrinsic_dest_type(instr)),
                          instr->num_components - 1);
}

static bi_index
bi_emit_lea_image(bi_builder *b, nir_intrinsic_instr *instr)
{
        enum glsl_sampler_dim dim = nir_intrinsic_image_dim(instr);
        ASSERTED unsigned nr_dim = glsl_get_sampler_dim_coordinate_components(dim);

        /* TODO: MSAA */
        assert(nr_dim != GLSL_SAMPLER_DIM_MS && "MSAA'd images not supported");

        enum bi_register_format type = (instr->intrinsic == nir_intrinsic_image_store) ?
                bi_reg_fmt_for_nir(nir_intrinsic_src_type(instr)) :
                BI_REGISTER_FORMAT_AUTO;

        bi_index coords = bi_src_index(&instr->src[1]);
        bi_index xy = bi_emit_image_coord(b, coords);
        bi_index zw = bi_emit_image_coord(b, bi_word(coords, 2));

        bi_instr *I = bi_lea_attr_tex_to(b, bi_temp(b->shader), xy, zw,
                        bi_src_index(&instr->src[0]), type);

        /* LEA_ATTR_TEX defaults to the secondary attribute table, but our ABI
         * has all images in the primary attribute table */
        I->table = BI_TABLE_ATTRIBUTE_1;

        return I->dest[0];
}

static void
bi_emit_image_store(bi_builder *b, nir_intrinsic_instr *instr)
{
        bi_index addr = bi_emit_lea_image(b, instr);

        bi_st_cvt(b, bi_src_index(&instr->src[3]),
                     addr, bi_word(addr, 1), bi_word(addr, 2),
                     bi_reg_fmt_for_nir(nir_intrinsic_src_type(instr)),
                     instr->num_components - 1);
}

static void
bi_emit_atomic_i32_to(bi_builder *b, bi_index dst,
                bi_index addr, bi_index arg, nir_intrinsic_op intrinsic)
{
        /* ATOM_C.i32 takes a vector with {arg, coalesced}, ATOM_C1.i32 doesn't
         * take any vector but can still output in RETURN mode */
        bi_index sr = bi_temp_reg(b->shader);

        enum bi_atom_opc opc = bi_atom_opc_for_nir(intrinsic);
        enum bi_atom_opc post_opc = opc;

        bi_instr *I;

        /* Generate either ATOM_C or ATOM_C1 as required */
        if (bi_promote_atom_c1(opc, arg, &opc)) {
                I = bi_patom_c1_i32_to(b, sr, bi_word(addr, 0),
                                bi_word(addr, 1), opc);
        } else {
                bi_mov_i32_to(b, sr, arg);
                I = bi_patom_c_i32_to(b, sr, sr, bi_word(addr, 0),
                                bi_word(addr, 1), opc);

        }

        I->sr_count = 2;

        /* Post-process it */
        bi_atom_post_i32_to(b, dst, bi_word(sr, 0), bi_word(sr, 1), post_opc);
}

/* gl_FragCoord.xy = u16_to_f32(R59.xy) + 0.5
 * gl_FragCoord.z = ld_vary(fragz)
 * gl_FragCoord.w = ld_vary(fragw)
 */

static void
bi_emit_load_frag_coord(bi_builder *b, nir_intrinsic_instr *instr)
{
        bi_index src[4] = {};

        for (unsigned i = 0; i < 2; ++i) {
                src[i] = bi_fadd_f32(b,
                                bi_u16_to_f32(b, bi_half(bi_register(59), i)),
                                bi_imm_f32(0.5f), BI_ROUND_NONE);
        }

        for (unsigned i = 0; i < 2; ++i) {
                src[2 + i] = bi_ld_var_special(b, bi_zero(),
                                BI_REGISTER_FORMAT_F32, BI_SAMPLE_CENTER,
                                BI_UPDATE_CLOBBER,
                                (i == 0) ? BI_VARYING_NAME_FRAG_Z :
                                        BI_VARYING_NAME_FRAG_W,
                                BI_VECSIZE_NONE);
        }

        bi_make_vec_to(b, bi_dest_index(&instr->dest), src, NULL, 4, 32);
}

static void
bi_emit_ld_tile(bi_builder *b, nir_intrinsic_instr *instr)
{
        unsigned rt = b->shader->inputs->blend.rt;

        /* Get the render target */
        if (!b->shader->inputs->is_blend) {
                const nir_variable *var =
                        nir_find_variable_with_driver_location(b->shader->nir,
                                        nir_var_shader_out, nir_intrinsic_base(instr));
                unsigned loc = var->data.location;
                assert(loc == FRAG_RESULT_COLOR || loc >= FRAG_RESULT_DATA0);
                rt = loc == FRAG_RESULT_COLOR ? 0 :
                        (loc - FRAG_RESULT_DATA0);
        }

        /* We want to load the current pixel.
         * FIXME: The sample to load is currently hardcoded to 0. This should
         * be addressed for multi-sample FBs.
         */
        struct bifrost_pixel_indices pix = {
                .y = BIFROST_CURRENT_PIXEL,
                .rt = rt
        };

        bi_index desc = b->shader->inputs->is_blend ?
                bi_imm_u32(b->shader->inputs->blend.bifrost_blend_desc >> 32) :
                bi_load_sysval(b, PAN_SYSVAL(RT_CONVERSION, rt), 1, 0);

        uint32_t indices = 0;
        memcpy(&indices, &pix, sizeof(indices));

        bi_ld_tile_to(b, bi_dest_index(&instr->dest), bi_imm_u32(indices),
                        bi_register(60), desc, (instr->num_components - 1));
}

static void
bi_emit_intrinsic(bi_builder *b, nir_intrinsic_instr *instr)
{
        bi_index dst = nir_intrinsic_infos[instr->intrinsic].has_dest ?
                bi_dest_index(&instr->dest) : bi_null();
        gl_shader_stage stage = b->shader->stage;

        switch (instr->intrinsic) {
        case nir_intrinsic_load_barycentric_pixel:
        case nir_intrinsic_load_barycentric_centroid:
        case nir_intrinsic_load_barycentric_sample:
        case nir_intrinsic_load_barycentric_at_sample:
        case nir_intrinsic_load_barycentric_at_offset:
                /* handled later via load_vary */
                break;
        case nir_intrinsic_load_interpolated_input:
        case nir_intrinsic_load_input:
                if (b->shader->inputs->is_blend)
                        bi_emit_load_blend_input(b, instr);
                else if (stage == MESA_SHADER_FRAGMENT)
                        bi_emit_load_vary(b, instr);
                else if (stage == MESA_SHADER_VERTEX)
                        bi_emit_load_attr(b, instr);
                else
                        unreachable("Unsupported shader stage");
                break;

        case nir_intrinsic_store_output:
                if (stage == MESA_SHADER_FRAGMENT)
                        bi_emit_fragment_out(b, instr);
                else if (stage == MESA_SHADER_VERTEX)
                        bi_emit_store_vary(b, instr);
                else
                        unreachable("Unsupported shader stage");
                break;

        case nir_intrinsic_store_combined_output_pan:
                assert(stage == MESA_SHADER_FRAGMENT);
                bi_emit_fragment_out(b, instr);
                break;

        case nir_intrinsic_load_ubo:
        case nir_intrinsic_load_kernel_input:
                bi_emit_load_ubo(b, instr);
                break;

        case nir_intrinsic_load_global:
        case nir_intrinsic_load_global_constant:
                bi_emit_load(b, instr, BI_SEG_NONE);
                break;

        case nir_intrinsic_store_global:
                bi_emit_store(b, instr, BI_SEG_NONE);
                break;

        case nir_intrinsic_load_scratch:
                bi_emit_load(b, instr, BI_SEG_TL);
                break;

        case nir_intrinsic_store_scratch:
                bi_emit_store(b, instr, BI_SEG_TL);
                break;

        case nir_intrinsic_load_shared:
                bi_emit_load(b, instr, BI_SEG_WLS);
                break;

        case nir_intrinsic_store_shared:
                bi_emit_store(b, instr, BI_SEG_WLS);
                break;

        /* Blob doesn't seem to do anything for memory barriers, note +BARRIER
         * is illegal in fragment shaders */
        case nir_intrinsic_memory_barrier:
        case nir_intrinsic_memory_barrier_buffer:
        case nir_intrinsic_memory_barrier_image:
        case nir_intrinsic_memory_barrier_shared:
        case nir_intrinsic_group_memory_barrier:
                break;

        case nir_intrinsic_control_barrier:
                assert(b->shader->stage != MESA_SHADER_FRAGMENT);
                bi_barrier(b);
                break;

        case nir_intrinsic_shared_atomic_add:
        case nir_intrinsic_shared_atomic_imin:
        case nir_intrinsic_shared_atomic_umin:
        case nir_intrinsic_shared_atomic_imax:
        case nir_intrinsic_shared_atomic_umax:
        case nir_intrinsic_shared_atomic_and:
        case nir_intrinsic_shared_atomic_or:
        case nir_intrinsic_shared_atomic_xor: {
                assert(nir_src_bit_size(instr->src[1]) == 32);

                bi_index addr = bi_seg_add_i64(b, bi_src_index(&instr->src[0]),
                                bi_zero(), false, BI_SEG_WLS);

                bi_emit_atomic_i32_to(b, dst, addr, bi_src_index(&instr->src[1]),
                                instr->intrinsic);
                break;
        }

        case nir_intrinsic_image_atomic_add:
        case nir_intrinsic_image_atomic_imin:
        case nir_intrinsic_image_atomic_umin:
        case nir_intrinsic_image_atomic_imax:
        case nir_intrinsic_image_atomic_umax:
        case nir_intrinsic_image_atomic_and:
        case nir_intrinsic_image_atomic_or:
        case nir_intrinsic_image_atomic_xor:
                assert(nir_src_bit_size(instr->src[3]) == 32);

                bi_emit_atomic_i32_to(b, dst,
                                bi_emit_lea_image(b, instr),
                                bi_src_index(&instr->src[3]),
                                instr->intrinsic);
                break;

        case nir_intrinsic_global_atomic_add:
        case nir_intrinsic_global_atomic_imin:
        case nir_intrinsic_global_atomic_umin:
        case nir_intrinsic_global_atomic_imax:
        case nir_intrinsic_global_atomic_umax:
        case nir_intrinsic_global_atomic_and:
        case nir_intrinsic_global_atomic_or:
        case nir_intrinsic_global_atomic_xor:
                assert(nir_src_bit_size(instr->src[1]) == 32);

                bi_emit_atomic_i32_to(b, dst,
                                bi_src_index(&instr->src[0]),
                                bi_src_index(&instr->src[1]),
                                instr->intrinsic);
                break;

        case nir_intrinsic_image_load:
                bi_emit_image_load(b, instr);
                break;

        case nir_intrinsic_image_store:
                bi_emit_image_store(b, instr);
                break;

        case nir_intrinsic_global_atomic_exchange:
                bi_emit_axchg_to(b, dst, bi_src_index(&instr->src[0]),
                                &instr->src[1], BI_SEG_NONE);
                break;

        case nir_intrinsic_image_atomic_exchange:
                bi_emit_axchg_to(b, dst, bi_emit_lea_image(b, instr),
                                &instr->src[3], BI_SEG_NONE);
                break;

        case nir_intrinsic_shared_atomic_exchange:
                bi_emit_axchg_to(b, dst, bi_src_index(&instr->src[0]),
                                &instr->src[1], BI_SEG_WLS);
                break;

        case nir_intrinsic_global_atomic_comp_swap:
                bi_emit_acmpxchg_to(b, dst, bi_src_index(&instr->src[0]),
                                &instr->src[1], &instr->src[2], BI_SEG_NONE);
                break;

        case nir_intrinsic_image_atomic_comp_swap:
                bi_emit_acmpxchg_to(b, dst, bi_emit_lea_image(b, instr),
                                &instr->src[3], &instr->src[4], BI_SEG_NONE);
                break;

        case nir_intrinsic_shared_atomic_comp_swap:
                bi_emit_acmpxchg_to(b, dst, bi_src_index(&instr->src[0]),
                                &instr->src[1], &instr->src[2], BI_SEG_WLS);
                break;

        case nir_intrinsic_load_frag_coord:
                bi_emit_load_frag_coord(b, instr);
                break;

        case nir_intrinsic_load_output:
                bi_emit_ld_tile(b, instr);
                break;

        case nir_intrinsic_discard_if: {
                bi_index src = bi_src_index(&instr->src[0]);

                unsigned sz = nir_src_bit_size(instr->src[0]);
                assert(sz == 16 || sz == 32);

                if (sz == 16)
                        src = bi_half(src, false);

                bi_discard_f32(b, src, bi_zero(), BI_CMPF_NE);
                break;
        }

        case nir_intrinsic_discard:
                bi_discard_f32(b, bi_zero(), bi_zero(), BI_CMPF_EQ);
                break;

        case nir_intrinsic_load_ssbo_address:
                bi_load_sysval_nir(b, instr, 2, 0);
                break;

        case nir_intrinsic_load_work_dim:
                bi_load_sysval_nir(b, instr, 1, 0);
                break;

        case nir_intrinsic_get_ssbo_size:
                bi_load_sysval_nir(b, instr, 1, 8);
                break;

        case nir_intrinsic_load_viewport_scale:
        case nir_intrinsic_load_viewport_offset:
        case nir_intrinsic_load_num_work_groups:
        case nir_intrinsic_load_sampler_lod_parameters_pan:
        case nir_intrinsic_load_local_group_size:
                bi_load_sysval_nir(b, instr, 3, 0);
                break;

        case nir_intrinsic_image_size:
                bi_load_sysval_nir(b, instr,
                                nir_dest_num_components(instr->dest), 0);
                break;

        case nir_intrinsic_load_blend_const_color_r_float:
                bi_mov_i32_to(b, dst,
                                bi_imm_f32(b->shader->inputs->blend.constants[0]));
                break;

        case nir_intrinsic_load_blend_const_color_g_float:
                bi_mov_i32_to(b, dst,
                                bi_imm_f32(b->shader->inputs->blend.constants[1]));
                break;

        case nir_intrinsic_load_blend_const_color_b_float:
                bi_mov_i32_to(b, dst,
                                bi_imm_f32(b->shader->inputs->blend.constants[2]));
                break;

        case nir_intrinsic_load_blend_const_color_a_float:
                bi_mov_i32_to(b, dst,
                                bi_imm_f32(b->shader->inputs->blend.constants[3]));
                break;

	case nir_intrinsic_load_sample_positions_pan:
                bi_mov_i32_to(b, bi_word(dst, 0),
                                bi_fau(BIR_FAU_SAMPLE_POS_ARRAY, false));
                bi_mov_i32_to(b, bi_word(dst, 1),
                                bi_fau(BIR_FAU_SAMPLE_POS_ARRAY, true));
                break;

	case nir_intrinsic_load_sample_mask_in:
                /* r61[0:15] contains the coverage bitmap */
                bi_u16_to_u32_to(b, dst, bi_half(bi_register(61), false));
                break;

        case nir_intrinsic_load_sample_id: {
                /* r61[16:23] contains the sampleID, mask it out. Upper bits
                 * seem to read garbage (despite being architecturally defined
                 * as zero), so use a 5-bit mask instead of 8-bits */

                bi_rshift_and_i32_to(b, dst, bi_register(61), bi_imm_u32(0x1f),
                                bi_imm_u8(16));
                break;
        }

	case nir_intrinsic_load_front_face:
                /* r58 == 0 means primitive is front facing */
                bi_icmp_i32_to(b, dst, bi_register(58), bi_zero(), BI_CMPF_EQ,
                                BI_RESULT_TYPE_M1);
                break;

        case nir_intrinsic_load_point_coord:
                bi_ld_var_special_to(b, dst, bi_zero(), BI_REGISTER_FORMAT_F32,
                                BI_SAMPLE_CENTER, BI_UPDATE_CLOBBER,
                                BI_VARYING_NAME_POINT, BI_VECSIZE_V2);
                break;

        case nir_intrinsic_load_vertex_id:
                bi_mov_i32_to(b, dst, bi_register(61));
                break;

        case nir_intrinsic_load_instance_id:
                bi_mov_i32_to(b, dst, bi_register(62));
                break;

        case nir_intrinsic_load_local_invocation_id:
                for (unsigned i = 0; i < 3; ++i)
                        bi_u16_to_u32_to(b, bi_word(dst, i),
                                         bi_half(bi_register(55 + i / 2), i % 2));
                break;

        case nir_intrinsic_load_work_group_id:
                for (unsigned i = 0; i < 3; ++i)
                        bi_mov_i32_to(b, bi_word(dst, i), bi_register(57 + i));
                break;

        case nir_intrinsic_load_global_invocation_id:
        case nir_intrinsic_load_global_invocation_id_zero_base:
                for (unsigned i = 0; i < 3; ++i)
                        bi_mov_i32_to(b, bi_word(dst, i), bi_register(60 + i));
                break;

        case nir_intrinsic_shader_clock:
                bi_ld_gclk_u64_to(b, dst, BI_SOURCE_CYCLE_COUNTER);
                break;

        default:
                fprintf(stderr, "Unhandled intrinsic %s\n", nir_intrinsic_infos[instr->intrinsic].name);
                assert(0);
        }
}

static void
bi_emit_load_const(bi_builder *b, nir_load_const_instr *instr)
{
        /* Make sure we've been lowered */
        assert(instr->def.num_components <= (32 / instr->def.bit_size));

        /* Accumulate all the channels of the constant, as if we did an
         * implicit SEL over them */
        uint32_t acc = 0;

        for (unsigned i = 0; i < instr->def.num_components; ++i) {
                unsigned v = nir_const_value_as_uint(instr->value[i], instr->def.bit_size);
                acc |= (v << (i * instr->def.bit_size));
        }

        bi_mov_i32_to(b, bi_get_index(instr->def.index, false, 0), bi_imm_u32(acc));
}

static bi_index
bi_alu_src_index(nir_alu_src src, unsigned comps)
{
        /* we don't lower modifiers until the backend */
        assert(!(src.negate || src.abs));

        unsigned bitsize = nir_src_bit_size(src.src);

        /* the bi_index carries the 32-bit (word) offset separate from the
         * subword swizzle, first handle the offset */

        unsigned offset = 0;

        assert(bitsize == 8 || bitsize == 16 || bitsize == 32);
        unsigned subword_shift = (bitsize == 32) ? 0 : (bitsize == 16) ? 1 : 2;

        for (unsigned i = 0; i < comps; ++i) {
                unsigned new_offset = (src.swizzle[i] >> subword_shift);

                if (i > 0)
                        assert(offset == new_offset);

                offset = new_offset;
        }

        bi_index idx = bi_word(bi_src_index(&src.src), offset);

        /* Compose the subword swizzle with existing (identity) swizzle */
        assert(idx.swizzle == BI_SWIZZLE_H01);

        /* Bigger vectors should have been lowered */
        assert(comps <= (1 << subword_shift));

        if (bitsize == 16) {
                unsigned c0 = src.swizzle[0] & 1;
                unsigned c1 = (comps > 1) ? src.swizzle[1] & 1 : c0;
                idx.swizzle = BI_SWIZZLE_H00 + c1 + (c0 << 1);
        } else if (bitsize == 8) {
                /* 8-bit vectors not yet supported */
                assert(comps == 1 && "8-bit vectors not supported");
                assert(src.swizzle[0] == 0 && "8-bit vectors not supported");
                idx.swizzle = BI_SWIZZLE_B0000;
        }

        return idx;
}

static enum bi_round
bi_nir_round(nir_op op)
{
        switch (op) {
        case nir_op_fround_even: return BI_ROUND_NONE;
        case nir_op_ftrunc: return BI_ROUND_RTZ;
        case nir_op_fceil: return BI_ROUND_RTP;
        case nir_op_ffloor: return BI_ROUND_RTN;
        default: unreachable("invalid nir round op");
        }
}

static enum bi_cmpf
bi_cmpf_nir(nir_op op)
{
        switch (op) {
        case nir_op_flt32:
        case nir_op_ilt32:
        case nir_op_ult32:
                return BI_CMPF_LT;

        case nir_op_fge32:
        case nir_op_ige32:
        case nir_op_uge32:
                return BI_CMPF_GE;

        case nir_op_feq32:
        case nir_op_ieq32:
                return BI_CMPF_EQ;

        case nir_op_fneu32:
        case nir_op_ine32:
                return BI_CMPF_NE;

        default:
                unreachable("Invalid compare");
        }
}

/* Convenience for lowered transcendentals */

static bi_index
bi_fmul_f32(bi_builder *b, bi_index s0, bi_index s1)
{
        return bi_fma_f32(b, s0, s1, bi_imm_f32(-0.0f), BI_ROUND_NONE);
}

/* Approximate with FRCP_APPROX.f32 and apply a single iteration of
 * Newton-Raphson to improve precision */

static void
bi_lower_frcp_32(bi_builder *b, bi_index dst, bi_index s0)
{
        bi_index x1 = bi_frcp_approx_f32(b, s0);
        bi_index m  = bi_frexpm_f32(b, s0, false, false);
        bi_index e  = bi_frexpe_f32(b, bi_neg(s0), false, false);
        bi_index t1 = bi_fma_rscale_f32(b, m, bi_neg(x1), bi_imm_f32(1.0),
                        bi_zero(), BI_ROUND_NONE, BI_SPECIAL_N);
        bi_fma_rscale_f32_to(b, dst, t1, x1, x1, e,
                        BI_ROUND_NONE, BI_SPECIAL_NONE);
}

static void
bi_lower_frsq_32(bi_builder *b, bi_index dst, bi_index s0)
{
        bi_index x1 = bi_frsq_approx_f32(b, s0);
        bi_index m  = bi_frexpm_f32(b, s0, false, true);
        bi_index e  = bi_frexpe_f32(b, bi_neg(s0), false, true);
        bi_index t1 = bi_fmul_f32(b, x1, x1);
        bi_index t2 = bi_fma_rscale_f32(b, m, bi_neg(t1), bi_imm_f32(1.0),
                        bi_imm_u32(-1), BI_ROUND_NONE, BI_SPECIAL_N);
        bi_fma_rscale_f32_to(b, dst, t2, x1, x1, e,
                        BI_ROUND_NONE, BI_SPECIAL_N);
}

/* More complex transcendentals, see
 * https://gitlab.freedesktop.org/panfrost/mali-isa-docs/-/blob/master/Bifrost.adoc
 * for documentation */

static void
bi_lower_fexp2_32(bi_builder *b, bi_index dst, bi_index s0)
{
        bi_index t1 = bi_temp(b->shader);
        bi_instr *t1_instr = bi_fadd_f32_to(b, t1,
                        s0, bi_imm_u32(0x49400000), BI_ROUND_NONE);
        t1_instr->clamp = BI_CLAMP_CLAMP_0_INF;

        bi_index t2 = bi_fadd_f32(b, t1, bi_imm_u32(0xc9400000), BI_ROUND_NONE);

        bi_instr *a2 = bi_fadd_f32_to(b, bi_temp(b->shader),
                        s0, bi_neg(t2), BI_ROUND_NONE);
        a2->clamp = BI_CLAMP_CLAMP_M1_1;

        bi_index a1t = bi_fexp_table_u4(b, t1, BI_ADJ_NONE);
        bi_index t3 = bi_isub_u32(b, t1, bi_imm_u32(0x49400000), false);
        bi_index a1i = bi_arshift_i32(b, t3, bi_null(), bi_imm_u8(4));
        bi_index p1 = bi_fma_f32(b, a2->dest[0], bi_imm_u32(0x3d635635),
                        bi_imm_u32(0x3e75fffa), BI_ROUND_NONE);
        bi_index p2 = bi_fma_f32(b, p1, a2->dest[0],
                        bi_imm_u32(0x3f317218), BI_ROUND_NONE);
        bi_index p3 = bi_fmul_f32(b, a2->dest[0], p2);
        bi_instr *x = bi_fma_rscale_f32_to(b, bi_temp(b->shader),
                        p3, a1t, a1t, a1i, BI_ROUND_NONE, BI_SPECIAL_NONE);
        x->clamp = BI_CLAMP_CLAMP_0_INF;

        bi_instr *max = bi_fmax_f32_to(b, dst, x->dest[0], s0);
        max->sem = BI_SEM_NAN_PROPAGATE;
}

static void
bi_lower_flog2_32(bi_builder *b, bi_index dst, bi_index s0)
{
        /* s0 = a1 * 2^e, with a1 in [0.75, 1.5) */
        bi_index a1 = bi_frexpm_f32(b, s0, true, false);
        bi_index ei = bi_frexpe_f32(b, s0, true, false);
        bi_index ef = bi_s32_to_f32(b, ei, BI_ROUND_RTZ);

        /* xt estimates -log(r1), a coarse approximation of log(a1) */
        bi_index r1 = bi_flog_table_f32(b, s0, BI_MODE_RED, BI_PRECISION_NONE);
        bi_index xt = bi_flog_table_f32(b, s0, BI_MODE_BASE2, BI_PRECISION_NONE);

        /* log(s0) = log(a1 * 2^e) = e + log(a1) = e + log(a1 * r1) -
         * log(r1), so let x1 = e - log(r1) ~= e + xt and x2 = log(a1 * r1),
         * and then log(s0) = x1 + x2 */
        bi_index x1 = bi_fadd_f32(b, ef, xt, BI_ROUND_NONE);

        /* Since a1 * r1 is close to 1, x2 = log(a1 * r1) may be computed by
         * polynomial approximation around 1. The series is expressed around
         * 1, so set y = (a1 * r1) - 1.0 */
        bi_index y = bi_fma_f32(b, a1, r1, bi_imm_f32(-1.0), BI_ROUND_NONE);

        /* x2 = log_2(1 + y) = log_e(1 + y) * (1/log_e(2)), so approximate
         * log_e(1 + y) by the Taylor series (lower precision than the blob):
         * y - y^2/2 + O(y^3) = y(1 - y/2) + O(y^3) */
        bi_index loge = bi_fmul_f32(b, y,
                bi_fma_f32(b, y, bi_imm_f32(-0.5), bi_imm_f32(1.0), BI_ROUND_NONE));

        bi_index x2 = bi_fmul_f32(b, loge, bi_imm_f32(1.0 / logf(2.0)));

        /* log(s0) = x1 + x2 */
        bi_fadd_f32_to(b, dst, x1, x2, BI_ROUND_NONE);
}

/* Bifrost has extremely coarse tables for approximating sin/cos, accessible as
 * FSIN/COS_TABLE.u6, which multiplies the bottom 6-bits by pi/32 and
 * calculates the results. We use them to calculate sin/cos via a Taylor
 * approximation:
 *
 * f(x + e) = f(x) + e f'(x) + (e^2)/2 f''(x)
 * sin(x + e) = sin(x) + e cos(x) - (e^2)/2 sin(x)
 * cos(x + e) = cos(x) - e sin(x) - (e^2)/2 cos(x)
 */

#define TWO_OVER_PI  bi_imm_f32(2.0f / 3.14159f)
#define MPI_OVER_TWO bi_imm_f32(-3.14159f / 2.0)
#define SINCOS_BIAS  bi_imm_u32(0x49400000)

static void
bi_lower_fsincos_32(bi_builder *b, bi_index dst, bi_index s0, bool cos)
{
        /* bottom 6-bits of result times pi/32 approximately s0 mod 2pi */
        bi_index x_u6 = bi_fma_f32(b, s0, TWO_OVER_PI, SINCOS_BIAS, BI_ROUND_NONE);

        /* Approximate domain error (small) */
        bi_index e = bi_fma_f32(b, bi_fadd_f32(b, x_u6, bi_neg(SINCOS_BIAS),
                                BI_ROUND_NONE),
                        MPI_OVER_TWO, s0, BI_ROUND_NONE);

        /* Lookup sin(x), cos(x) */
        bi_index sinx = bi_fsin_table_u6(b, x_u6, false);
        bi_index cosx = bi_fcos_table_u6(b, x_u6, false);

        /* e^2 / 2 */
        bi_index e2_over_2 = bi_fma_rscale_f32(b, e, e, bi_neg(bi_zero()),
                        bi_imm_u32(-1), BI_ROUND_NONE, BI_SPECIAL_NONE);

        /* (-e^2)/2 f''(x) */
        bi_index quadratic = bi_fma_f32(b, bi_neg(e2_over_2),
                        cos ? cosx : sinx,
                        bi_neg(bi_zero()),  BI_ROUND_NONE);

        /* e f'(x) - (e^2/2) f''(x) */
        bi_instr *I = bi_fma_f32_to(b, bi_temp(b->shader), e,
                        cos ? bi_neg(sinx) : cosx,
                        quadratic, BI_ROUND_NONE);
        I->clamp = BI_CLAMP_CLAMP_M1_1;

        /* f(x) + e f'(x) - (e^2/2) f''(x) */
        bi_fadd_f32_to(b, dst, I->dest[0], cos ? cosx : sinx, BI_ROUND_NONE);
}

static void
bi_emit_alu(bi_builder *b, nir_alu_instr *instr)
{
        bi_index dst = bi_dest_index(&instr->dest.dest);
        unsigned sz = nir_dest_bit_size(instr->dest.dest);

        unsigned srcs = nir_op_infos[instr->op].num_inputs;
        unsigned comps = nir_dest_num_components(instr->dest.dest);

        if (!instr->dest.dest.is_ssa) {
                for (unsigned i = 0; i < comps; ++i)
                        assert(instr->dest.write_mask);
        }

        /* First, match against the various moves in NIR. These are
         * special-cased because they can operate on vectors even after
         * lowering ALU to scalar. For Bifrost, bi_alu_src_index assumes the
         * instruction is no "bigger" than SIMD-within-a-register. These moves
         * are the exceptions that need to handle swizzles specially. */

        switch (instr->op) {
        case nir_op_pack_32_2x16:
        case nir_op_vec2:
        case nir_op_vec3:
        case nir_op_vec4: {
                bi_index unoffset_srcs[4] = {
                        srcs > 0 ? bi_src_index(&instr->src[0].src) : bi_null(),
                        srcs > 1 ? bi_src_index(&instr->src[1].src) : bi_null(),
                        srcs > 2 ? bi_src_index(&instr->src[2].src) : bi_null(),
                        srcs > 3 ? bi_src_index(&instr->src[3].src) : bi_null(),
                };

                unsigned channels[4] = {
                        instr->src[0].swizzle[0],
                        instr->src[1].swizzle[0],
                        srcs > 2 ? instr->src[2].swizzle[0] : 0,
                        srcs > 3 ? instr->src[3].swizzle[0] : 0,
                };

                bi_make_vec_to(b, dst, unoffset_srcs, channels, srcs, sz);
                return;
        }

        case nir_op_vec8:
        case nir_op_vec16:
                unreachable("should've been lowered");

        case nir_op_unpack_32_2x16:
        case nir_op_unpack_64_2x32_split_x:
                bi_mov_i32_to(b, dst, bi_src_index(&instr->src[0].src));
                return;

        case nir_op_unpack_64_2x32_split_y:
                bi_mov_i32_to(b, dst, bi_word(bi_src_index(&instr->src[0].src), 1));
                return;

        case nir_op_pack_64_2x32_split:
                bi_mov_i32_to(b, bi_word(dst, 0), bi_src_index(&instr->src[0].src));
                bi_mov_i32_to(b, bi_word(dst, 1), bi_src_index(&instr->src[1].src));
                return;

        case nir_op_pack_64_2x32:
                bi_mov_i32_to(b, bi_word(dst, 0), bi_word(bi_src_index(&instr->src[0].src), 0));
                bi_mov_i32_to(b, bi_word(dst, 1), bi_word(bi_src_index(&instr->src[0].src), 1));
                return;

        case nir_op_mov: {
                bi_index idx = bi_src_index(&instr->src[0].src);
                bi_index unoffset_srcs[4] = { idx, idx, idx, idx };

                unsigned channels[4] = {
                        comps > 0 ? instr->src[0].swizzle[0] : 0,
                        comps > 1 ? instr->src[0].swizzle[1] : 0,
                        comps > 2 ? instr->src[0].swizzle[2] : 0,
                        comps > 3 ? instr->src[0].swizzle[3] : 0,
                };

                bi_make_vec_to(b, dst, unoffset_srcs, channels, comps, sz);
                return;
        }

        default:
                break;
        }

        bi_index s0 = srcs > 0 ? bi_alu_src_index(instr->src[0], comps) : bi_null();
        bi_index s1 = srcs > 1 ? bi_alu_src_index(instr->src[1], comps) : bi_null();
        bi_index s2 = srcs > 2 ? bi_alu_src_index(instr->src[2], comps) : bi_null();

        unsigned src_sz = srcs > 0 ? nir_src_bit_size(instr->src[0].src) : 0;

        switch (instr->op) {
        case nir_op_ffma:
                bi_fma_to(b, sz, dst, s0, s1, s2, BI_ROUND_NONE);
                break;

        case nir_op_fmul:
                bi_fma_to(b, sz, dst, s0, s1, bi_zero(), BI_ROUND_NONE);
                break;

        case nir_op_fsub:
                s1 = bi_neg(s1);
                /* fallthrough */
        case nir_op_fadd:
                bi_fadd_to(b, sz, dst, s0, s1, BI_ROUND_NONE);
                break;

        case nir_op_fsat: {
                bi_instr *I = (sz == 32) ?
                        bi_fadd_f32_to(b, dst, s0, bi_zero(), BI_ROUND_NONE) :
                        bi_fma_v2f16_to(b, dst, s0, bi_zero(), bi_zero(),
                                        BI_ROUND_NONE);

                I->clamp = BI_CLAMP_CLAMP_0_1;
                break;
        }

        case nir_op_fneg:
                bi_fadd_to(b, sz, dst, bi_neg(s0), bi_zero(), BI_ROUND_NONE);
                break;

        case nir_op_fabs:
                bi_fadd_to(b, sz, dst, bi_abs(s0), bi_zero(), BI_ROUND_NONE);
                break;

        case nir_op_fsin:
                bi_lower_fsincos_32(b, dst, s0, false);
                break;

        case nir_op_fcos:
                bi_lower_fsincos_32(b, dst, s0, true);
                break;

        case nir_op_fexp2: {
                assert(sz == 32); /* should've been lowered */

                if (b->shader->quirks & BIFROST_NO_FP32_TRANSCENDENTALS) {
                        bi_lower_fexp2_32(b, dst, s0);
                        break;
                }

                /* multiply by 1.0 * 2*24 */
                bi_index scale = bi_fma_rscale_f32(b, s0, bi_imm_f32(1.0f),
                                bi_zero(), bi_imm_u32(24), BI_ROUND_NONE,
                                BI_SPECIAL_NONE);

                bi_fexp_f32_to(b, dst, bi_f32_to_s32(b, scale, BI_ROUND_NONE), s0);
                break;
        }

        case nir_op_flog2: {
                assert(sz == 32); /* should've been lowered */

                if (b->shader->quirks & BIFROST_NO_FP32_TRANSCENDENTALS) {
                        bi_lower_flog2_32(b, dst, s0);
                        break;
                }

                bi_index frexp = bi_frexpe_f32(b, s0, true, false);
                bi_index frexpi = bi_s32_to_f32(b, frexp, BI_ROUND_RTZ);
                bi_index add = bi_fadd_lscale_f32(b, bi_imm_f32(-1.0f), s0);
                bi_fma_f32_to(b, dst, bi_flogd_f32(b, s0), add, frexpi,
                                BI_ROUND_NONE);
                break;
        }

        case nir_op_b8csel:
        case nir_op_b16csel:
        case nir_op_b32csel:
                if (sz == 8)
                        bi_mux_v4i8_to(b, dst, s2, s1, s0, BI_MUX_INT_ZERO);
                else
                        bi_csel_to(b, nir_type_float, sz, dst, s0, bi_zero(), s1, s2, BI_CMPF_NE);
                break;

        case nir_op_ishl:
                bi_lshift_or_to(b, sz, dst, s0, bi_zero(), bi_byte(s1, 0));
                break;
        case nir_op_ushr:
                bi_rshift_or_to(b, sz, dst, s0, bi_zero(), bi_byte(s1, 0));
                break;

        case nir_op_ishr:
                bi_arshift_to(b, sz, dst, s0, bi_null(), bi_byte(s1, 0));
                break;

        case nir_op_flt32:
        case nir_op_fge32:
        case nir_op_feq32:
        case nir_op_fneu32:
                bi_fcmp_to(b, sz, dst, s0, s1, bi_cmpf_nir(instr->op),
                                BI_RESULT_TYPE_M1);
                break;

        case nir_op_ieq32:
        case nir_op_ine32:
                if (sz == 32) {
                        bi_icmp_i32_to(b, dst, s0, s1, bi_cmpf_nir(instr->op),
                                        BI_RESULT_TYPE_M1);
                } else if (sz == 16) {
                        bi_icmp_v2i16_to(b, dst, s0, s1, bi_cmpf_nir(instr->op),
                                        BI_RESULT_TYPE_M1);
                } else {
                        bi_icmp_v4i8_to(b, dst, s0, s1, bi_cmpf_nir(instr->op),
                                        BI_RESULT_TYPE_M1);
                }
                break;

        case nir_op_ilt32:
        case nir_op_ige32:
                if (sz == 32) {
                        bi_icmp_s32_to(b, dst, s0, s1, bi_cmpf_nir(instr->op),
                                        BI_RESULT_TYPE_M1);
                } else if (sz == 16) {
                        bi_icmp_v2s16_to(b, dst, s0, s1, bi_cmpf_nir(instr->op),
                                        BI_RESULT_TYPE_M1);
                } else {
                        bi_icmp_v4s8_to(b, dst, s0, s1, bi_cmpf_nir(instr->op),
                                        BI_RESULT_TYPE_M1);
                }
                break;

        case nir_op_ult32:
        case nir_op_uge32:
                if (sz == 32) {
                        bi_icmp_u32_to(b, dst, s0, s1, bi_cmpf_nir(instr->op),
                                        BI_RESULT_TYPE_M1);
                } else if (sz == 16) {
                        bi_icmp_v2u16_to(b, dst, s0, s1, bi_cmpf_nir(instr->op),
                                        BI_RESULT_TYPE_M1);
                } else {
                        bi_icmp_v4u8_to(b, dst, s0, s1, bi_cmpf_nir(instr->op),
                                        BI_RESULT_TYPE_M1);
                }
                break;

        case nir_op_fddx:
        case nir_op_fddy: {
                bi_index lane1 = bi_lshift_and_i32(b,
                                bi_fau(BIR_FAU_LANE_ID, false),
                                bi_imm_u32(instr->op == nir_op_fddx ? 2 : 1),
                                bi_imm_u8(0));

                bi_index lane2 = bi_iadd_u32(b, lane1,
                                bi_imm_u32(instr->op == nir_op_fddx ? 1 : 2),
                                false);

                bi_index left, right;

                if (b->shader->arch == 6) {
                        left = bi_clper_v6_i32(b, s0, lane1);
                        right = bi_clper_v6_i32(b, s0, lane2);
                } else {
                        left = bi_clper_v7_i32(b, s0, lane1,
                                        BI_INACTIVE_RESULT_ZERO, BI_LANE_OP_NONE,
                                        BI_SUBGROUP_SUBGROUP4);

                        right = bi_clper_v7_i32(b, s0, lane2,
                                        BI_INACTIVE_RESULT_ZERO, BI_LANE_OP_NONE,
                                        BI_SUBGROUP_SUBGROUP4);
                }

                bi_fadd_to(b, sz, dst, right, bi_neg(left), BI_ROUND_NONE);
                break;
        }

        case nir_op_f2f16:
                bi_v2f32_to_v2f16_to(b, dst, s0, s0, BI_ROUND_NONE);
                break;

        case nir_op_f2f32:
                bi_f16_to_f32_to(b, dst, s0);
                break;

        case nir_op_f2i32:
                if (src_sz == 32)
                        bi_f32_to_s32_to(b, dst, s0, BI_ROUND_RTZ);
                else
                        bi_f16_to_s32_to(b, dst, s0, BI_ROUND_RTZ);
                break;

        case nir_op_f2u16:
                if (src_sz == 32)
                        unreachable("should've been lowered");
                else
                        bi_v2f16_to_v2u16_to(b, dst, s0, BI_ROUND_RTZ);
                break;

        case nir_op_f2i16:
                if (src_sz == 32)
                        unreachable("should've been lowered");
                else
                        bi_v2f16_to_v2s16_to(b, dst, s0, BI_ROUND_RTZ);
                break;

        case nir_op_f2u32:
                if (src_sz == 32)
                        bi_f32_to_u32_to(b, dst, s0, BI_ROUND_RTZ);
                else
                        bi_f16_to_u32_to(b, dst, s0, BI_ROUND_RTZ);
                break;

        case nir_op_u2f16:
                if (src_sz == 32)
                        unreachable("should've been lowered");
                else if (src_sz == 16)
                        bi_v2u16_to_v2f16_to(b, dst, s0, BI_ROUND_RTZ);
                else if (src_sz == 8)
                        bi_v2u8_to_v2f16_to(b, dst, s0);
                break;

        case nir_op_u2f32:
                if (src_sz == 32)
                        bi_u32_to_f32_to(b, dst, s0, BI_ROUND_RTZ);
                else if (src_sz == 16)
                        bi_u16_to_f32_to(b, dst, s0);
                else
                        bi_u8_to_f32_to(b, dst, bi_byte(s0, 0));
                break;

        case nir_op_i2f16:
                if (src_sz == 32)
                        unreachable("should've been lowered");
                else
                        bi_v2s16_to_v2f16_to(b, dst, s0, BI_ROUND_RTZ);
                break;

        case nir_op_i2f32:
                if (src_sz == 32)
                        bi_s32_to_f32_to(b, dst, s0, BI_ROUND_RTZ);
                else
                        bi_s16_to_f32_to(b, dst, s0);
                break;

        case nir_op_i2i32:
                if (src_sz == 16)
                        bi_s16_to_s32_to(b, dst, s0);
                else
                        bi_s8_to_s32_to(b, dst, s0);
                break;

        case nir_op_u2u32:
                if (src_sz == 16)
                        bi_u16_to_u32_to(b, dst, s0);
                else
                        bi_u8_to_u32_to(b, dst, s0);
                break;

        /* todo optimize out downcasts */
        case nir_op_i2i16:
                assert(src_sz == 8 || src_sz == 32);

                if (src_sz == 8)
                        bi_v2s8_to_v2s16_to(b, dst, s0);
                else
                        bi_mkvec_v2i16_to(b, dst, bi_half(s0, false), bi_imm_u16(0));
                break;

        case nir_op_u2u16:
                assert(src_sz == 8 || src_sz == 32);

                if (src_sz == 8)
                        bi_v2u8_to_v2u16_to(b, dst, s0);
                else
                        bi_mkvec_v2i16_to(b, dst, bi_half(s0, false), bi_imm_u16(0));
                break;

        case nir_op_i2i8:
        case nir_op_u2u8:
                /* No vectorization in this part of the loop, so downcasts are
                 * a noop. When vectorization support lands, some case
                 * handlingg will be needed, but for the scalar case this is
                 * optimal as it can be copypropped away */
                bi_mov_i32_to(b, dst, s0);
                break;

        case nir_op_fround_even:
        case nir_op_fceil:
        case nir_op_ffloor:
        case nir_op_ftrunc:
                bi_fround_to(b, sz, dst, s0, bi_nir_round(instr->op));
                break;

        case nir_op_fmin:
                bi_fmin_to(b, sz, dst, s0, s1);
                break;

        case nir_op_fmax:
                bi_fmax_to(b, sz, dst, s0, s1);
                break;

        case nir_op_iadd:
                bi_iadd_to(b, nir_type_int, sz, dst, s0, s1, false);
                break;

        case nir_op_iadd_sat:
                bi_iadd_to(b, nir_type_int, sz, dst, s0, s1, true);
                break;

        case nir_op_uadd_sat:
                bi_iadd_to(b, nir_type_uint, sz, dst, s0, s1, true);
                break;

        case nir_op_ihadd:
                bi_hadd_to(b, nir_type_int, sz, dst, s0, s1, BI_ROUND_RTN);
                break;

        case nir_op_irhadd:
                bi_hadd_to(b, nir_type_int, sz, dst, s0, s1, BI_ROUND_RTP);
                break;

        case nir_op_isub:
                bi_isub_to(b, nir_type_int, sz, dst, s0, s1, false);
                break;

        case nir_op_isub_sat:
                bi_isub_to(b, nir_type_int, sz, dst, s0, s1, true);
                break;

        case nir_op_usub_sat:
                bi_isub_to(b, nir_type_uint, sz, dst, s0, s1, true);
                break;

        case nir_op_imul:
                bi_imul_to(b, sz, dst, s0, s1);
                break;

        case nir_op_iabs:
                bi_iabs_to(b, sz, dst, s0);
                break;

        case nir_op_iand:
                bi_lshift_and_to(b, sz, dst, s0, s1, bi_imm_u8(0));
                break;

        case nir_op_ior:
                bi_lshift_or_to(b, sz, dst, s0, s1, bi_imm_u8(0));
                break;

        case nir_op_ixor:
                bi_lshift_xor_to(b, sz, dst, s0, s1, bi_imm_u8(0));
                break;

        case nir_op_inot:
                bi_lshift_or_to(b, sz, dst, bi_zero(), bi_not(s0), bi_imm_u8(0));
                break;

        case nir_op_frsq:
                if (sz == 32 && b->shader->quirks & BIFROST_NO_FP32_TRANSCENDENTALS)
                        bi_lower_frsq_32(b, dst, s0);
                else
                        bi_frsq_to(b, sz, dst, s0);
                break;

        case nir_op_frcp:
                if (sz == 32 && b->shader->quirks & BIFROST_NO_FP32_TRANSCENDENTALS)
                        bi_lower_frcp_32(b, dst, s0);
                else
                        bi_frcp_to(b, sz, dst, s0);
                break;

        case nir_op_uclz:
                bi_clz_to(b, sz, dst, s0, false);
                break;

        case nir_op_bit_count:
                bi_popcount_i32_to(b, dst, s0);
                break;

        case nir_op_bitfield_reverse:
                bi_bitrev_i32_to(b, dst, s0);
                break;

        case nir_op_ufind_msb: {
                bi_index clz = bi_clz(b, src_sz, s0, false);

                if (sz == 8)
                        clz = bi_byte(clz, 0);
                else if (sz == 16)
                        clz = bi_half(clz, false);

                bi_isub_u32_to(b, dst, bi_imm_u32(src_sz - 1), clz, false);
                break;
        }

        default:
                fprintf(stderr, "Unhandled ALU op %s\n", nir_op_infos[instr->op].name);
                unreachable("Unknown ALU op");
        }
}

/* Returns dimension with 0 special casing cubemaps. Shamelessly copied from Midgard */
static unsigned
bifrost_tex_format(enum glsl_sampler_dim dim)
{
        switch (dim) {
        case GLSL_SAMPLER_DIM_1D:
        case GLSL_SAMPLER_DIM_BUF:
                return 1;

        case GLSL_SAMPLER_DIM_2D:
        case GLSL_SAMPLER_DIM_MS:
        case GLSL_SAMPLER_DIM_EXTERNAL:
        case GLSL_SAMPLER_DIM_RECT:
                return 2;

        case GLSL_SAMPLER_DIM_3D:
                return 3;

        case GLSL_SAMPLER_DIM_CUBE:
                return 0;

        default:
                DBG("Unknown sampler dim type\n");
                assert(0);
                return 0;
        }
}

static enum bifrost_texture_format_full
bi_texture_format(nir_alu_type T, enum bi_clamp clamp)
{
        switch (T) {
        case nir_type_float16: return BIFROST_TEXTURE_FORMAT_F16 + clamp;
        case nir_type_float32: return BIFROST_TEXTURE_FORMAT_F32 + clamp;
        case nir_type_uint16:  return BIFROST_TEXTURE_FORMAT_U16;
        case nir_type_int16:   return BIFROST_TEXTURE_FORMAT_S16;
        case nir_type_uint32:  return BIFROST_TEXTURE_FORMAT_U32;
        case nir_type_int32:   return BIFROST_TEXTURE_FORMAT_S32;
        default:              unreachable("Invalid type for texturing");
        }
}

/* Array indices are specified as 32-bit uints, need to convert. In .z component from NIR */
static bi_index
bi_emit_texc_array_index(bi_builder *b, bi_index idx, nir_alu_type T)
{
        /* For (u)int we can just passthrough */
        nir_alu_type base = nir_alu_type_get_base_type(T);
        if (base == nir_type_int || base == nir_type_uint)
                return idx;

        /* Otherwise we convert */
        assert(T == nir_type_float32);

        /* OpenGL ES 3.2 specification section 8.14.2 ("Coordinate Wrapping and
         * Texel Selection") defines the layer to be taken from clamp(RNE(r),
         * 0, dt - 1). So we use round RTE, clamping is handled at the data
         * structure level */

        return bi_f32_to_u32(b, idx, BI_ROUND_NONE);
}

/* TEXC's explicit and bias LOD modes requires the LOD to be transformed to a
 * 16-bit 8:8 fixed-point format. We lower as:
 *
 * F32_TO_S32(clamp(x, -16.0, +16.0) * 256.0) & 0xFFFF =
 * MKVEC(F32_TO_S32(clamp(x * 1.0/16.0, -1.0, 1.0) * (16.0 * 256.0)), #0)
 */

static bi_index
bi_emit_texc_lod_88(bi_builder *b, bi_index lod, bool fp16)
{
        /* Sort of arbitrary. Must be less than 128.0, greater than or equal to
         * the max LOD (16 since we cap at 2^16 texture dimensions), and
         * preferably small to minimize precision loss */
        const float max_lod = 16.0;

        bi_instr *fsat = bi_fma_f32_to(b, bi_temp(b->shader),
                        fp16 ? bi_half(lod, false) : lod,
                        bi_imm_f32(1.0f / max_lod), bi_zero(), BI_ROUND_NONE);

        fsat->clamp = BI_CLAMP_CLAMP_M1_1;

        bi_index fmul = bi_fma_f32(b, fsat->dest[0], bi_imm_f32(max_lod * 256.0f),
                        bi_zero(), BI_ROUND_NONE);

        return bi_mkvec_v2i16(b,
                        bi_half(bi_f32_to_s32(b, fmul, BI_ROUND_RTZ), false),
                        bi_imm_u16(0));
}

/* FETCH takes a 32-bit staging register containing the LOD as an integer in
 * the bottom 16-bits and (if present) the cube face index in the top 16-bits.
 * TODO: Cube face.
 */

static bi_index
bi_emit_texc_lod_cube(bi_builder *b, bi_index lod)
{
        return bi_lshift_or_i32(b, lod, bi_zero(), bi_imm_u8(8));
}

/* The hardware specifies texel offsets and multisample indices together as a
 * u8vec4 <offset, ms index>. By default all are zero, so if have either a
 * nonzero texel offset or a nonzero multisample index, we build a u8vec4 with
 * the bits we need and return that to be passed as a staging register. Else we
 * return 0 to avoid allocating a data register when everything is zero. */

static bi_index
bi_emit_texc_offset_ms_index(bi_builder *b, nir_tex_instr *instr)
{
        bi_index dest = bi_zero();

        int offs_idx = nir_tex_instr_src_index(instr, nir_tex_src_offset);
        if (offs_idx >= 0 &&
            (!nir_src_is_const(instr->src[offs_idx].src) ||
             nir_src_as_uint(instr->src[offs_idx].src) != 0)) {
                unsigned nr = nir_src_num_components(instr->src[offs_idx].src);
                bi_index idx = bi_src_index(&instr->src[offs_idx].src);
                dest = bi_mkvec_v4i8(b, 
                                (nr > 0) ? bi_byte(bi_word(idx, 0), 0) : bi_imm_u8(0),
                                (nr > 1) ? bi_byte(bi_word(idx, 1), 0) : bi_imm_u8(0),
                                (nr > 2) ? bi_byte(bi_word(idx, 2), 0) : bi_imm_u8(0),
                                bi_imm_u8(0));
        }

        int ms_idx = nir_tex_instr_src_index(instr, nir_tex_src_ms_index);
        if (ms_idx >= 0 &&
            (!nir_src_is_const(instr->src[ms_idx].src) ||
             nir_src_as_uint(instr->src[ms_idx].src) != 0)) {
                dest = bi_lshift_or_i32(b,
                                bi_src_index(&instr->src[ms_idx].src), dest,
                                bi_imm_u8(24));
        }

        return dest;
}

static void
bi_emit_cube_coord(bi_builder *b, bi_index coord,
                    bi_index *face, bi_index *s, bi_index *t)
{
        /* Compute max { |x|, |y|, |z| } */
        bi_instr *cubeface = bi_cubeface_to(b, bi_temp(b->shader),
                        bi_temp(b->shader), coord,
                        bi_word(coord, 1), bi_word(coord, 2));

        /* Select coordinates */

        bi_index ssel = bi_cube_ssel(b, bi_word(coord, 2), coord,
                        cubeface->dest[1]);

        bi_index tsel = bi_cube_tsel(b, bi_word(coord, 1), bi_word(coord, 2),
                        cubeface->dest[1]);

        /* The OpenGL ES specification requires us to transform an input vector
         * (x, y, z) to the coordinate, given the selected S/T:
         *
         * (1/2 ((s / max{x,y,z}) + 1), 1/2 ((t / max{x, y, z}) + 1))
         *
         * We implement (s shown, t similar) in a form friendlier to FMA
         * instructions, and clamp coordinates at the end for correct
         * NaN/infinity handling:
         *
         * fsat(s * (0.5 * (1 / max{x, y, z})) + 0.5)
         *
         * Take the reciprocal of max{x, y, z}
         */

        bi_index rcp = bi_frcp_f32(b, cubeface->dest[0]);

        /* Calculate 0.5 * (1.0 / max{x, y, z}) */
        bi_index fma1 = bi_fma_f32(b, rcp, bi_imm_f32(0.5f), bi_zero(),
                        BI_ROUND_NONE);

        /* Transform the coordinates */
        *s = bi_temp(b->shader);
        *t = bi_temp(b->shader);

        bi_instr *S = bi_fma_f32_to(b, *s, fma1, ssel, bi_imm_f32(0.5f),
                        BI_ROUND_NONE);
        bi_instr *T = bi_fma_f32_to(b, *t, fma1, tsel, bi_imm_f32(0.5f),
                        BI_ROUND_NONE);

        S->clamp = BI_CLAMP_CLAMP_0_1;
        T->clamp = BI_CLAMP_CLAMP_0_1;

        /* Cube face is stored in bit[29:31], we don't apply the shift here
         * because the TEXS_CUBE and TEXC instructions expect the face index to
         * be at this position.
         */
        *face = cubeface->dest[1];
}

/* Emits a cube map descriptor, returning lower 32-bits and putting upper
 * 32-bits in passed pointer t */

static bi_index
bi_emit_texc_cube_coord(bi_builder *b, bi_index coord, bi_index *t)
{
        bi_index face, s;
        bi_emit_cube_coord(b, coord, &face, &s, t);

        bi_index and1 = bi_lshift_and_i32(b, face, bi_imm_u32(0xe0000000),
                        bi_imm_u8(0));

        bi_index and2 = bi_lshift_and_i32(b, s, bi_imm_u32(0x1fffffff),
                        bi_imm_u8(0));

        return bi_lshift_or_i32(b, and1, and2, bi_imm_u8(0));
}

/* Map to the main texture op used. Some of these (txd in particular) will
 * lower to multiple texture ops with different opcodes (GRDESC_DER + TEX in
 * sequence). We assume that lowering is handled elsewhere.
 */

static enum bifrost_tex_op
bi_tex_op(nir_texop op)
{
        switch (op) {
        case nir_texop_tex:
        case nir_texop_txb:
        case nir_texop_txl:
        case nir_texop_txd:
        case nir_texop_tex_prefetch:
                return BIFROST_TEX_OP_TEX;
        case nir_texop_txf:
        case nir_texop_txf_ms:
        case nir_texop_txf_ms_fb:
        case nir_texop_txf_ms_mcs:
        case nir_texop_tg4:
                return BIFROST_TEX_OP_FETCH;
        case nir_texop_txs:
        case nir_texop_lod:
        case nir_texop_query_levels:
        case nir_texop_texture_samples:
        case nir_texop_samples_identical:
                unreachable("should've been lowered");
        default:
                unreachable("unsupported tex op");
        }
}

/* Data registers required by texturing in the order they appear. All are
 * optional, the texture operation descriptor determines which are present.
 * Note since 3D arrays are not permitted at an API level, Z_COORD and
 * ARRAY/SHADOW are exlusive, so TEXC in practice reads at most 8 registers */

enum bifrost_tex_dreg {
        BIFROST_TEX_DREG_Z_COORD = 0,
        BIFROST_TEX_DREG_Y_DELTAS = 1,
        BIFROST_TEX_DREG_LOD = 2,
        BIFROST_TEX_DREG_GRDESC_HI = 3,
        BIFROST_TEX_DREG_SHADOW = 4,
        BIFROST_TEX_DREG_ARRAY = 5,
        BIFROST_TEX_DREG_OFFSETMS = 6,
        BIFROST_TEX_DREG_SAMPLER = 7,
        BIFROST_TEX_DREG_TEXTURE = 8,
        BIFROST_TEX_DREG_COUNT,
};

static void
bi_emit_texc(bi_builder *b, nir_tex_instr *instr)
{
        /* TODO: support more with other encodings */
        assert(instr->sampler_index < 16);

        struct bifrost_texture_operation desc = {
                .op = bi_tex_op(instr->op),
                .offset_or_bias_disable = false, /* TODO */
                .shadow_or_clamp_disable = instr->is_shadow,
                .array = instr->is_array,
                .dimension = bifrost_tex_format(instr->sampler_dim),
                .format = bi_texture_format(instr->dest_type | nir_dest_bit_size(instr->dest), BI_CLAMP_NONE), /* TODO */
                .mask = 0xF,
        };

        switch (desc.op) {
        case BIFROST_TEX_OP_TEX:
                desc.lod_or_fetch = BIFROST_LOD_MODE_COMPUTE;
                break;
        case BIFROST_TEX_OP_FETCH:
                desc.lod_or_fetch = instr->op == nir_texop_tg4 ?
                        BIFROST_TEXTURE_FETCH_GATHER4_R + instr->component :
                        BIFROST_TEXTURE_FETCH_TEXEL;
                break;
        default:
                unreachable("texture op unsupported");
        }

        /* 32-bit indices to be allocated as consecutive staging registers */
        bi_index dregs[BIFROST_TEX_DREG_COUNT] = { };
        bi_index cx = bi_null(), cy = bi_null();

        for (unsigned i = 0; i < instr->num_srcs; ++i) {
                bi_index index = bi_src_index(&instr->src[i].src);
                unsigned sz = nir_src_bit_size(instr->src[i].src);
                ASSERTED nir_alu_type base = nir_tex_instr_src_type(instr, i);
                nir_alu_type T = base | sz;

                switch (instr->src[i].src_type) {
                case nir_tex_src_coord:
                        if (instr->sampler_dim == GLSL_SAMPLER_DIM_CUBE) {
                                cx = bi_emit_texc_cube_coord(b, index, &cy);
			} else {
                                unsigned components = nir_src_num_components(instr->src[i].src);

                                /* Copy XY (for 2D+) or XX (for 1D) */
                                cx = index;
                                cy = bi_word(index, MIN2(1, components - 1));

                                assert(components >= 1 && components <= 3);

                                if (components < 3) {
                                        /* nothing to do */
                                } else if (desc.array) {
                                        /* 2D array */
                                        dregs[BIFROST_TEX_DREG_ARRAY] =
                                                bi_emit_texc_array_index(b,
                                                                bi_word(index, 2), T);
                                } else {
                                        /* 3D */
                                        dregs[BIFROST_TEX_DREG_Z_COORD] =
                                                bi_word(index, 2);
                                }
                        }
                        break;

                case nir_tex_src_lod:
                        if (desc.op == BIFROST_TEX_OP_TEX &&
                            nir_src_is_const(instr->src[i].src) &&
                            nir_src_as_uint(instr->src[i].src) == 0) {
                                desc.lod_or_fetch = BIFROST_LOD_MODE_ZERO;
                        } else if (desc.op == BIFROST_TEX_OP_TEX) {
                                assert(base == nir_type_float);

                                assert(sz == 16 || sz == 32);
                                dregs[BIFROST_TEX_DREG_LOD] =
                                        bi_emit_texc_lod_88(b, index, sz == 16);
                                desc.lod_or_fetch = BIFROST_LOD_MODE_EXPLICIT;
                        } else {
                                assert(desc.op == BIFROST_TEX_OP_FETCH);
                                assert(base == nir_type_uint || base == nir_type_int);
                                assert(sz == 16 || sz == 32);

                                dregs[BIFROST_TEX_DREG_LOD] =
                                        bi_emit_texc_lod_cube(b, index);
                        }

                        break;

                case nir_tex_src_bias:
                        /* Upper 16-bits interpreted as a clamp, leave zero */
                        assert(desc.op == BIFROST_TEX_OP_TEX);
                        assert(base == nir_type_float);
                        assert(sz == 16 || sz == 32);
                        dregs[BIFROST_TEX_DREG_LOD] =
                                bi_emit_texc_lod_88(b, index, sz == 16);
                        desc.lod_or_fetch = BIFROST_LOD_MODE_BIAS;
                        break;

                case nir_tex_src_ms_index:
                case nir_tex_src_offset:
                        if (desc.offset_or_bias_disable)
                                break;

                        dregs[BIFROST_TEX_DREG_OFFSETMS] =
	                        bi_emit_texc_offset_ms_index(b, instr);
                        if (!bi_is_equiv(dregs[BIFROST_TEX_DREG_OFFSETMS], bi_zero()))
                                desc.offset_or_bias_disable = true;
                        break;

                case nir_tex_src_comparator:
                        dregs[BIFROST_TEX_DREG_SHADOW] = index;
                        break;

                case nir_tex_src_texture_offset:
                        assert(instr->texture_index == 0);
                        dregs[BIFROST_TEX_DREG_TEXTURE] = index;
                        break;

                case nir_tex_src_sampler_offset:
                        assert(instr->sampler_index == 0);
                        dregs[BIFROST_TEX_DREG_SAMPLER] = index;
                        break;

                default:
                        unreachable("Unhandled src type in texc emit");
                }
        }

        if (desc.op == BIFROST_TEX_OP_FETCH && bi_is_null(dregs[BIFROST_TEX_DREG_LOD])) {
                dregs[BIFROST_TEX_DREG_LOD] =
                        bi_emit_texc_lod_cube(b, bi_zero());
        }

        /* Choose an index mode */

        bool direct_tex = bi_is_null(dregs[BIFROST_TEX_DREG_TEXTURE]);
        bool direct_samp = bi_is_null(dregs[BIFROST_TEX_DREG_SAMPLER]);
        bool direct = direct_tex && direct_samp;

        desc.immediate_indices = direct && (instr->sampler_index < 16);

        if (desc.immediate_indices) {
                desc.sampler_index_or_mode = instr->sampler_index;
                desc.index = instr->texture_index;
        } else {
                enum bifrost_index mode = 0;

                if (direct && instr->sampler_index == instr->texture_index) {
                        mode = BIFROST_INDEX_IMMEDIATE_SHARED;
                        desc.index = instr->texture_index;
                } else if (direct) {
                        mode = BIFROST_INDEX_IMMEDIATE_SAMPLER;
                        desc.index = instr->sampler_index;
                        dregs[BIFROST_TEX_DREG_TEXTURE] = bi_mov_i32(b,
                                        bi_imm_u32(instr->texture_index));
                } else if (direct_tex) {
                        assert(!direct_samp);
                        mode = BIFROST_INDEX_IMMEDIATE_TEXTURE;
                        desc.index = instr->texture_index;
                } else if (direct_samp) {
                        assert(!direct_tex);
                        mode = BIFROST_INDEX_IMMEDIATE_SAMPLER;
                        desc.index = instr->sampler_index;
                } else {
                        mode = BIFROST_INDEX_REGISTER;
                }

                desc.sampler_index_or_mode = mode | (0x3 << 2);
        }

        /* Allocate staging registers contiguously by compacting the array.
         * Index is not SSA (tied operands) */

        bi_index idx = bi_temp_reg(b->shader);
        unsigned sr_count = 0;

        for (unsigned i = 0; i < ARRAY_SIZE(dregs); ++i) {
                if (!bi_is_null(dregs[i]))
                        dregs[sr_count++] = dregs[i];
        }

        if (sr_count)
                bi_make_vec_to(b, idx, dregs, NULL, sr_count, 32);
        else
                bi_mov_i32_to(b, idx, bi_zero()); /* XXX: shouldn't be necessary */

        uint32_t desc_u = 0;
        memcpy(&desc_u, &desc, sizeof(desc_u));
        bi_texc_to(b, idx, idx, cx, cy, bi_imm_u32(desc_u), sr_count);

        /* Explicit copy to facilitate tied operands */
        bi_index srcs[4] = { idx, idx, idx, idx };
        unsigned channels[4] = { 0, 1, 2, 3 };
        bi_make_vec_to(b, bi_dest_index(&instr->dest), srcs, channels, 4, 32);
}

/* Simple textures ops correspond to NIR tex or txl with LOD = 0 on 2D/cube
 * textures with sufficiently small immediate indices. Anything else
 * needs a complete texture op. */

static void
bi_emit_texs(bi_builder *b, nir_tex_instr *instr)
{
        int coord_idx = nir_tex_instr_src_index(instr, nir_tex_src_coord);
        assert(coord_idx >= 0);
        bi_index coords = bi_src_index(&instr->src[coord_idx].src);

        if (instr->sampler_dim == GLSL_SAMPLER_DIM_CUBE) {
                bi_index face, s, t;
                bi_emit_cube_coord(b, coords, &face, &s, &t);

                bi_texs_cube_to(b, nir_dest_bit_size(instr->dest),
                                bi_dest_index(&instr->dest),
                                s, t, face,
                                instr->sampler_index, instr->texture_index);
        } else {
                bi_texs_2d_to(b, nir_dest_bit_size(instr->dest),
                                bi_dest_index(&instr->dest),
                                coords, bi_word(coords, 1),
                                instr->op != nir_texop_tex, /* zero LOD */
                                instr->sampler_index, instr->texture_index);
        }
}

static bool
bi_is_simple_tex(nir_tex_instr *instr)
{
        if (instr->op != nir_texop_tex && instr->op != nir_texop_txl)
                return false;

        if (instr->dest_type != nir_type_float32 &&
            instr->dest_type != nir_type_float16)
                return false;

        if (instr->is_shadow || instr->is_array)
                return false;

        switch (instr->sampler_dim) {
        case GLSL_SAMPLER_DIM_2D:
        case GLSL_SAMPLER_DIM_EXTERNAL:
                break;

        case GLSL_SAMPLER_DIM_CUBE:
                /* LOD can't be specified with TEXS_CUBE */
                if (instr->op == nir_texop_txl)
                        return false;
                break;

        default:
                return false;
        }

        for (unsigned i = 0; i < instr->num_srcs; ++i) {
                if (instr->src[i].src_type != nir_tex_src_lod &&
                    instr->src[i].src_type != nir_tex_src_coord)
                        return false;
        }

        /* Indices need to fit in provided bits */
        unsigned idx_bits = instr->sampler_dim == GLSL_SAMPLER_DIM_CUBE ? 2 : 3;
        if (MAX2(instr->sampler_index, instr->texture_index) >= (1 << idx_bits))
                return false;

        int lod_idx = nir_tex_instr_src_index(instr, nir_tex_src_lod);
        if (lod_idx < 0)
                return true;

        nir_src lod = instr->src[lod_idx].src;
        return nir_src_is_const(lod) && nir_src_as_uint(lod) == 0;
}

static void
bi_emit_tex(bi_builder *b, nir_tex_instr *instr)
{
        switch (instr->op) {
        case nir_texop_txs:
                bi_load_sysval_to(b, bi_dest_index(&instr->dest),
                                panfrost_sysval_for_instr(&instr->instr, NULL),
                                4, 0);
                return;
        case nir_texop_tex:
        case nir_texop_txl:
        case nir_texop_txb:
        case nir_texop_txf:
        case nir_texop_txf_ms:
        case nir_texop_tg4:
                break;
        default:
                unreachable("Invalid texture operation");
        }

        if (bi_is_simple_tex(instr))
                bi_emit_texs(b, instr);
        else
                bi_emit_texc(b, instr);
}

static void
bi_emit_instr(bi_builder *b, struct nir_instr *instr)
{
        switch (instr->type) {
        case nir_instr_type_load_const:
                bi_emit_load_const(b, nir_instr_as_load_const(instr));
                break;

        case nir_instr_type_intrinsic:
                bi_emit_intrinsic(b, nir_instr_as_intrinsic(instr));
                break;

        case nir_instr_type_alu:
                bi_emit_alu(b, nir_instr_as_alu(instr));
                break;

        case nir_instr_type_tex:
                bi_emit_tex(b, nir_instr_as_tex(instr));
                break;

        case nir_instr_type_jump:
                bi_emit_jump(b, nir_instr_as_jump(instr));
                break;

        default:
                unreachable("should've been lowered");
        }
}

static bi_block *
create_empty_block(bi_context *ctx)
{
        bi_block *blk = rzalloc(ctx, bi_block);

        blk->base.predecessors = _mesa_set_create(blk,
                        _mesa_hash_pointer,
                        _mesa_key_pointer_equal);

        return blk;
}

static bi_block *
emit_block(bi_context *ctx, nir_block *block)
{
        if (ctx->after_block) {
                ctx->current_block = ctx->after_block;
                ctx->after_block = NULL;
        } else {
                ctx->current_block = create_empty_block(ctx);
        }

        list_addtail(&ctx->current_block->base.link, &ctx->blocks);
        list_inithead(&ctx->current_block->base.instructions);

        bi_builder _b = bi_init_builder(ctx, bi_after_block(ctx->current_block));

        nir_foreach_instr(instr, block) {
                bi_emit_instr(&_b, instr);
                ++ctx->instruction_count;
        }

        return ctx->current_block;
}

static void
emit_if(bi_context *ctx, nir_if *nif)
{
        bi_block *before_block = ctx->current_block;

        /* Speculatively emit the branch, but we can't fill it in until later */
        bi_builder _b = bi_init_builder(ctx, bi_after_block(ctx->current_block));
        bi_instr *then_branch = bi_branchz_i32(&_b,
                        bi_src_index(&nif->condition), bi_zero(), BI_CMPF_EQ);

        /* Emit the two subblocks. */
        bi_block *then_block = emit_cf_list(ctx, &nif->then_list);
        bi_block *end_then_block = ctx->current_block;

        /* Emit second block, and check if it's empty */

        int count_in = ctx->instruction_count;
        bi_block *else_block = emit_cf_list(ctx, &nif->else_list);
        bi_block *end_else_block = ctx->current_block;
        ctx->after_block = create_empty_block(ctx);

        /* Now that we have the subblocks emitted, fix up the branches */

        assert(then_block);
        assert(else_block);

        if (ctx->instruction_count == count_in) {
                then_branch->branch_target = ctx->after_block;
                pan_block_add_successor(&end_then_block->base, &ctx->after_block->base); /* fallthrough */
        } else {
                then_branch->branch_target = else_block;

                /* Emit a jump from the end of the then block to the end of the else */
                _b.cursor = bi_after_block(end_then_block);
                bi_instr *then_exit = bi_jump(&_b, bi_zero());
                then_exit->branch_target = ctx->after_block;

                pan_block_add_successor(&end_then_block->base, &then_exit->branch_target->base);
                pan_block_add_successor(&end_else_block->base, &ctx->after_block->base); /* fallthrough */
        }

        pan_block_add_successor(&before_block->base, &then_branch->branch_target->base); /* then_branch */
        pan_block_add_successor(&before_block->base, &then_block->base); /* fallthrough */
}

static void
emit_loop(bi_context *ctx, nir_loop *nloop)
{
        /* Remember where we are */
        bi_block *start_block = ctx->current_block;

        bi_block *saved_break = ctx->break_block;
        bi_block *saved_continue = ctx->continue_block;

        ctx->continue_block = create_empty_block(ctx);
        ctx->break_block = create_empty_block(ctx);
        ctx->after_block = ctx->continue_block;

        /* Emit the body itself */
        emit_cf_list(ctx, &nloop->body);

        /* Branch back to loop back */
        bi_builder _b = bi_init_builder(ctx, bi_after_block(ctx->current_block));
        bi_instr *I = bi_jump(&_b, bi_zero());
        I->branch_target = ctx->continue_block;
        pan_block_add_successor(&start_block->base, &ctx->continue_block->base);
        pan_block_add_successor(&ctx->current_block->base, &ctx->continue_block->base);

        ctx->after_block = ctx->break_block;

        /* Pop off */
        ctx->break_block = saved_break;
        ctx->continue_block = saved_continue;
        ++ctx->loop_count;
}

static bi_block *
emit_cf_list(bi_context *ctx, struct exec_list *list)
{
        bi_block *start_block = NULL;

        foreach_list_typed(nir_cf_node, node, node, list) {
                switch (node->type) {
                case nir_cf_node_block: {
                        bi_block *block = emit_block(ctx, nir_cf_node_as_block(node));

                        if (!start_block)
                                start_block = block;

                        break;
                }

                case nir_cf_node_if:
                        emit_if(ctx, nir_cf_node_as_if(node));
                        break;

                case nir_cf_node_loop:
                        emit_loop(ctx, nir_cf_node_as_loop(node));
                        break;

                default:
                        unreachable("Unknown control flow");
                }
        }

        return start_block;
}

/* shader-db stuff */

static void
bi_print_stats(bi_context *ctx, unsigned size, FILE *fp)
{
        unsigned nr_clauses = 0, nr_tuples = 0, nr_ins = 0;

        /* Count instructions, clauses, and tuples */
        bi_foreach_block(ctx, _block) {
                bi_block *block = (bi_block *) _block;

                bi_foreach_clause_in_block(block, clause) {
                        nr_clauses++;
                        nr_tuples += clause->tuple_count;

                        for (unsigned i = 0; i < clause->tuple_count; ++i) {
                                if (clause->tuples[i].fma)
                                        nr_ins++;

                                if (clause->tuples[i].add)
                                        nr_ins++;
                        }
                }
        }

        /* tuples = ((# of instructions) + (# of nops)) / 2 */
        unsigned nr_nops = (2 * nr_tuples) - nr_ins;

        /* In the future, we'll calculate thread count for v7. For now we
         * always use fewer threads than we should (v6 style) due to missing
         * piping, TODO: fix that for a nice perf win */
        unsigned nr_threads = 1;

        /* Dump stats */

        fprintf(stderr, "%s - %s shader: "
                        "%u inst, %u nops, %u clauses, "
                        "%u quadwords, %u threads, %u loops, "
                        "%u:%u spills:fills\n",
                        ctx->nir->info.label ?: "",
                        ctx->inputs->is_blend ? "PAN_SHADER_BLEND" :
                        gl_shader_stage_name(ctx->stage),
                        nr_ins, nr_nops, nr_clauses,
                        size / 16, nr_threads,
                        ctx->loop_count,
                        ctx->spills, ctx->fills);
}

static int
glsl_type_size(const struct glsl_type *type, bool bindless)
{
        return glsl_count_attribute_slots(type, false);
}

static void
bi_optimize_nir(nir_shader *nir)
{
        bool progress;
        unsigned lower_flrp = 16 | 32 | 64;

        NIR_PASS(progress, nir, nir_lower_regs_to_ssa);

        nir_lower_tex_options lower_tex_options = {
                .lower_txs_lod = true,
                .lower_txp = ~0,
                .lower_tex_without_implicit_lod = true,
                .lower_tg4_broadcom_swizzle = true,
                .lower_txd = true,
        };

        NIR_PASS(progress, nir, pan_nir_lower_64bit_intrin);
        NIR_PASS(progress, nir, pan_lower_helper_invocation);

        NIR_PASS(progress, nir, nir_lower_int64);

        NIR_PASS(progress, nir, nir_lower_idiv, nir_lower_idiv_fast);

        NIR_PASS(progress, nir, nir_lower_tex, &lower_tex_options);
        NIR_PASS(progress, nir, nir_lower_alu_to_scalar, NULL, NULL);
        NIR_PASS(progress, nir, nir_lower_load_const_to_scalar);

        do {
                progress = false;

                NIR_PASS(progress, nir, nir_lower_var_copies);
                NIR_PASS(progress, nir, nir_lower_vars_to_ssa);

                NIR_PASS(progress, nir, nir_copy_prop);
                NIR_PASS(progress, nir, nir_opt_remove_phis);
                NIR_PASS(progress, nir, nir_opt_dce);
                NIR_PASS(progress, nir, nir_opt_dead_cf);
                NIR_PASS(progress, nir, nir_opt_cse);
                NIR_PASS(progress, nir, nir_opt_peephole_select, 64, false, true);
                NIR_PASS(progress, nir, nir_opt_algebraic);
                NIR_PASS(progress, nir, nir_opt_constant_folding);

                NIR_PASS(progress, nir, nir_lower_alu);

                if (lower_flrp != 0) {
                        bool lower_flrp_progress = false;
                        NIR_PASS(lower_flrp_progress,
                                 nir,
                                 nir_lower_flrp,
                                 lower_flrp,
                                 false /* always_precise */);
                        if (lower_flrp_progress) {
                                NIR_PASS(progress, nir,
                                         nir_opt_constant_folding);
                                progress = true;
                        }

                        /* Nothing should rematerialize any flrps, so we only
                         * need to do this lowering once.
                         */
                        lower_flrp = 0;
                }

                NIR_PASS(progress, nir, nir_opt_undef);
                NIR_PASS(progress, nir, nir_lower_undef_to_zero);

                NIR_PASS(progress, nir, nir_opt_loop_unroll,
                         nir_var_shader_in |
                         nir_var_shader_out |
                         nir_var_function_temp);
        } while (progress);

        /* We need to cleanup after each iteration of late algebraic
         * optimizations, since otherwise NIR can produce weird edge cases
         * (like fneg of a constant) which we don't handle */
        bool late_algebraic = true;
        while (late_algebraic) {
                late_algebraic = false;
                NIR_PASS(late_algebraic, nir, nir_opt_algebraic_late);
                NIR_PASS(progress, nir, nir_opt_constant_folding);
                NIR_PASS(progress, nir, nir_copy_prop);
                NIR_PASS(progress, nir, nir_opt_dce);
                NIR_PASS(progress, nir, nir_opt_cse);
        }

        NIR_PASS(progress, nir, nir_lower_bool_to_int32);
        NIR_PASS(progress, nir, bifrost_nir_lower_algebraic_late);
        NIR_PASS(progress, nir, nir_lower_alu_to_scalar, NULL, NULL);

        /* Backend scheduler is purely local, so do some global optimizations
         * to reduce register pressure */
        NIR_PASS_V(nir, nir_opt_sink, nir_move_const_undef);
        NIR_PASS_V(nir, nir_opt_move, nir_move_const_undef);

        NIR_PASS(progress, nir, nir_lower_load_const_to_scalar);

        /* Take us out of SSA */
        NIR_PASS(progress, nir, nir_lower_locals_to_regs);
        NIR_PASS(progress, nir, nir_move_vec_src_uses_to_dest);
        NIR_PASS(progress, nir, nir_convert_from_ssa, true);
}

/* The cmdstream lowers 8-bit fragment output as 16-bit, so we need to do the
 * same lowering here to zero-extend correctly */

static bool
bifrost_nir_lower_i8_fragout_impl(struct nir_builder *b,
                nir_instr *instr, UNUSED void *data)
{
        if (instr->type != nir_instr_type_intrinsic)
                return false;

        nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
        if (intr->intrinsic != nir_intrinsic_store_output)
                return false;

        if (nir_src_bit_size(intr->src[0]) != 8)
                return false;

        nir_alu_type type =
                nir_alu_type_get_base_type(nir_intrinsic_src_type(intr));

        assert(type == nir_type_int || type == nir_type_uint);

        b->cursor = nir_before_instr(instr);
        nir_ssa_def *cast = type == nir_type_int ?
                nir_i2i(b, intr->src[0].ssa, 16) :
                nir_u2u(b, intr->src[0].ssa, 16);

        nir_intrinsic_set_src_type(intr, type | 16);
        nir_instr_rewrite_src(&intr->instr, &intr->src[0],
                        nir_src_for_ssa(cast));
        return true;
}

static bool
bifrost_nir_lower_i8_fragout(nir_shader *shader)
{
        if (shader->info.stage != MESA_SHADER_FRAGMENT)
                return false;

        return nir_shader_instructions_pass(shader,
                        bifrost_nir_lower_i8_fragout_impl,
                        nir_metadata_block_index | nir_metadata_dominance,
                        NULL);
}

/* Dead code elimination for branches at the end of a block - only one branch
 * per block is legal semantically, but unreachable jumps can be generated.
 * Likewise we can generate jumps to the terminal block which need to be
 * lowered away to a jump to #0x0, which induces successful termination. */

static void
bi_lower_branch(bi_block *block)
{
        bool branched = false;
        ASSERTED bool was_jump = false;

        bi_foreach_instr_in_block_safe(block, ins) {
                if (!ins->branch_target) continue;

                if (branched) {
                        assert(was_jump && (ins->op == BI_OPCODE_JUMP));
                        bi_remove_instruction(ins);
                        continue;
                }

                branched = true;
                was_jump = ins->op == BI_OPCODE_JUMP;

                if (bi_is_terminal_block(ins->branch_target))
                        ins->branch_target = NULL;
        }
}

void
bifrost_compile_shader_nir(nir_shader *nir,
                           const struct panfrost_compile_inputs *inputs,
                           struct util_dynarray *binary,
                           struct pan_shader_info *info)
{
        bifrost_debug = debug_get_option_bifrost_debug();

        bi_context *ctx = rzalloc(NULL, bi_context);
        ctx->sysval_to_id = panfrost_init_sysvals(&info->sysvals, ctx);

        ctx->inputs = inputs;
        ctx->nir = nir;
        ctx->info = info;
        ctx->stage = nir->info.stage;
        ctx->quirks = bifrost_get_quirks(inputs->gpu_id);
        ctx->arch = inputs->gpu_id >> 12;
        list_inithead(&ctx->blocks);

        /* Lower gl_Position pre-optimisation, but after lowering vars to ssa
         * (so we don't accidentally duplicate the epilogue since mesa/st has
         * messed with our I/O quite a bit already) */

        NIR_PASS_V(nir, nir_lower_vars_to_ssa);

        if (ctx->stage == MESA_SHADER_VERTEX) {
                NIR_PASS_V(nir, nir_lower_viewport_transform);
                NIR_PASS_V(nir, nir_lower_point_size, 1.0, 1024.0);
        }

        NIR_PASS_V(nir, nir_split_var_copies);
        NIR_PASS_V(nir, nir_lower_global_vars_to_local);
        NIR_PASS_V(nir, nir_lower_var_copies);
        NIR_PASS_V(nir, nir_lower_vars_to_ssa);
        NIR_PASS_V(nir, nir_lower_io, nir_var_shader_in | nir_var_shader_out,
                        glsl_type_size, 0);
        NIR_PASS_V(nir, nir_lower_ssbo);
        NIR_PASS_V(nir, pan_nir_lower_zs_store);
        NIR_PASS_V(nir, pan_lower_sample_pos);
        NIR_PASS_V(nir, bifrost_nir_lower_i8_fragout);
        // TODO: re-enable when fp16 is flipped on
        // NIR_PASS_V(nir, nir_lower_mediump_outputs);

        bi_optimize_nir(nir);

        NIR_PASS_V(nir, pan_nir_reorder_writeout);

        bool skip_internal = nir->info.internal;
        skip_internal &= !(bifrost_debug & BIFROST_DBG_INTERNAL);

        if (bifrost_debug & BIFROST_DBG_SHADERS && !skip_internal) {
                nir_print_shader(nir, stdout);
        }

        info->tls_size = nir->scratch_size;

        nir_foreach_function(func, nir) {
                if (!func->impl)
                        continue;

                ctx->ssa_alloc += func->impl->ssa_alloc;
                ctx->reg_alloc += func->impl->reg_alloc;

                emit_cf_list(ctx, &func->impl->body);
                break; /* TODO: Multi-function shaders */
        }

        unsigned block_source_count = 0;

        bi_foreach_block(ctx, _block) {
                bi_block *block = (bi_block *) _block;

                /* Name blocks now that we're done emitting so the order is
                 * consistent */
                block->base.name = block_source_count++;
        }

        /* Runs before copy prop */
        bi_opt_push_ubo(ctx);

        bool progress = false;

        do {
                progress = false;

                progress |= bi_opt_copy_prop(ctx);
                progress |= bi_opt_dead_code_eliminate(ctx, false);
        } while(progress);

        bi_foreach_block(ctx, _block) {
                bi_block *block = (bi_block *) _block;
                bi_lower_branch(block);
        }

        if (bifrost_debug & BIFROST_DBG_SHADERS && !skip_internal)
                bi_print_shader(ctx, stdout);
        bi_schedule(ctx);
        bi_assign_scoreboard(ctx);
        bi_register_allocate(ctx);
        if (bifrost_debug & BIFROST_DBG_SHADERS && !skip_internal)
                bi_print_shader(ctx, stdout);

        unsigned final_clause = bi_pack(ctx, binary);

        /* If we need to wait for ATEST or BLEND in the first clause, pass the
         * corresponding bits through to the renderer state descriptor */
        pan_block *first_block = list_first_entry(&ctx->blocks, pan_block, link);
        bi_clause *first_clause = bi_next_clause(ctx, first_block, NULL);

        unsigned first_deps = first_clause ? first_clause->dependencies : 0;
        info->bifrost.wait_6 = (first_deps & (1 << 6));
        info->bifrost.wait_7 = (first_deps & (1 << 7));

        if (bifrost_debug & BIFROST_DBG_SHADERS && !skip_internal) {
                disassemble_bifrost(stdout, binary->data, binary->size,
                                    bifrost_debug & BIFROST_DBG_VERBOSE);
        }

        /* Pad the shader with enough zero bytes to trick the prefetcher,
         * unless we're compiling an empty shader (in which case we don't pad
         * so the size remains 0) */
        unsigned prefetch_size = BIFROST_SHADER_PREFETCH - final_clause;

        if (binary->size) {
                memset(util_dynarray_grow(binary, uint8_t, prefetch_size),
                       0, prefetch_size);
        }

        if ((bifrost_debug & BIFROST_DBG_SHADERDB || inputs->shaderdb) &&
            !skip_internal) {
                bi_print_stats(ctx, binary->size, stderr);
        }

        ralloc_free(ctx);
}
