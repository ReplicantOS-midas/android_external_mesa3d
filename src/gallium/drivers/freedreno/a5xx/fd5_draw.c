/*
 * Copyright (C) 2016 Rob Clark <robclark@freedesktop.org>
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
 * Authors:
 *    Rob Clark <robclark@freedesktop.org>
 */

#include "pipe/p_state.h"
#include "util/u_string.h"
#include "util/u_memory.h"
#include "util/u_prim.h"

#include "freedreno_state.h"
#include "freedreno_resource.h"

#include "fd5_draw.h"
#include "fd5_context.h"
#include "fd5_emit.h"
#include "fd5_program.h"
#include "fd5_format.h"
#include "fd5_zsa.h"


static void
draw_impl(struct fd_context *ctx, struct fd_ringbuffer *ring,
		struct fd5_emit *emit, unsigned index_offset)
	assert_dt
{
	const struct pipe_draw_info *info = emit->info;
	enum pc_di_primtype primtype = ctx->primtypes[info->mode];

	fd5_emit_state(ctx, ring, emit);

	if (emit->dirty & (FD_DIRTY_VTXBUF | FD_DIRTY_VTXSTATE))
		fd5_emit_vertex_bufs(ring, emit);

	OUT_PKT4(ring, REG_A5XX_VFD_INDEX_OFFSET, 2);
	OUT_RING(ring, info->index_size ? info->index_bias : emit->draw->start); /* VFD_INDEX_OFFSET */
	OUT_RING(ring, info->start_instance);   /* VFD_INSTANCE_START_OFFSET */

	OUT_PKT4(ring, REG_A5XX_PC_RESTART_INDEX, 1);
	OUT_RING(ring, info->primitive_restart ? /* PC_RESTART_INDEX */
			info->restart_index : 0xffffffff);

	fd5_emit_render_cntl(ctx, false, emit->binning_pass);
	fd5_draw_emit(ctx->batch, ring, primtype,
			emit->binning_pass ? IGNORE_VISIBILITY : USE_VISIBILITY,
			info, emit->indirect, emit->draw, index_offset);
}

static bool
fd5_draw_vbo(struct fd_context *ctx, const struct pipe_draw_info *info,
             const struct pipe_draw_indirect_info *indirect,
             const struct pipe_draw_start_count *draw,
             unsigned index_offset)
	in_dt
{
	struct fd5_context *fd5_ctx = fd5_context(ctx);
	struct fd5_emit emit = {
		.debug = &ctx->debug,
		.vtx  = &ctx->vtx,
		.prog = &ctx->prog,
		.info = info,
		.indirect = indirect,
		.draw = draw,
		.key = {
			.rasterflat = ctx->rasterizer->flatshade,
			.ucp_enables = ctx->rasterizer->clip_plane_enable,
			.has_per_samp = fd5_ctx->fastc_srgb || fd5_ctx->vastc_srgb,
			.vastc_srgb = fd5_ctx->vastc_srgb,
			.fastc_srgb = fd5_ctx->fastc_srgb,
		},
		.rasterflat = ctx->rasterizer->flatshade,
		.sprite_coord_enable = ctx->rasterizer->sprite_coord_enable,
		.sprite_coord_mode = ctx->rasterizer->sprite_coord_mode,
	};

	/* Technically a5xx should not require this, but it avoids a crash in
	 * piglit 'spec@!opengl 1.1@ppgtt_memory_alignment' due to a draw with
	 * no VBO bound but a VS that expects an input.  The draw is a single
	 * vertex with PIPE_PRIM_TRIANGLES so the u_trim_pipe_prim() causes it
	 * to be skipped.
	 */
	if (info->mode != PIPE_PRIM_MAX &&
			!indirect &&
			!info->primitive_restart &&
			!u_trim_pipe_prim(info->mode, (unsigned*)&draw->count))
		return false;

	ir3_fixup_shader_state(&ctx->base, &emit.key);

	unsigned dirty = ctx->dirty;
	const struct ir3_shader_variant *vp = fd5_emit_get_vp(&emit);
	const struct ir3_shader_variant *fp = fd5_emit_get_fp(&emit);

	/* do regular pass first, since that is more likely to fail compiling: */

	if (!vp || !fp)
		return false;

	if (unlikely(ctx->stats_users > 0)) {
		ctx->stats.vs_regs += ir3_shader_halfregs(vp);
		ctx->stats.fs_regs += ir3_shader_halfregs(fp);
	}

	/* figure out whether we need to disable LRZ write for binning
	 * pass using draw pass's fp:
	 */
	emit.no_lrz_write = fp->writes_pos || fp->no_earlyz || fp->has_kill;

	emit.binning_pass = false;
	emit.dirty = dirty;

	draw_impl(ctx, ctx->batch->draw, &emit, index_offset);

	/* and now binning pass: */
	emit.binning_pass = true;
	emit.dirty = dirty & ~(FD_DIRTY_BLEND);
	emit.vs = NULL;   /* we changed key so need to refetch vp */
	emit.fs = NULL;
	draw_impl(ctx, ctx->batch->binning, &emit, index_offset);

	if (emit.streamout_mask) {
		struct fd_ringbuffer *ring = ctx->batch->draw;

		for (unsigned i = 0; i < PIPE_MAX_SO_BUFFERS; i++) {
			if (emit.streamout_mask & (1 << i)) {
				fd5_event_write(ctx->batch, ring, FLUSH_SO_0 + i, false);
			}
		}
	}

	fd_context_all_clean(ctx);

	return true;
}

static bool is_z32(enum pipe_format format)
{
	switch (format) {
	case PIPE_FORMAT_Z32_FLOAT_S8X24_UINT:
	case PIPE_FORMAT_Z32_UNORM:
	case PIPE_FORMAT_Z32_FLOAT:
		return true;
	default:
		return false;
	}
}

static void
fd5_clear_lrz(struct fd_batch *batch, struct fd_resource *zsbuf, double depth)
{
	struct fd_ringbuffer *ring;
	uint32_t clear = util_pack_z(PIPE_FORMAT_Z16_UNORM, depth);

	ring = fd_batch_get_prologue(batch);

	OUT_WFI5(ring);

	OUT_PKT4(ring, REG_A5XX_RB_CCU_CNTL, 1);
	OUT_RING(ring, 0x10000000);

	OUT_PKT4(ring, REG_A5XX_HLSQ_UPDATE_CNTL, 1);
	OUT_RING(ring, 0x20fffff);

	OUT_PKT4(ring, REG_A5XX_GRAS_SU_CNTL, 1);
	OUT_RING(ring, A5XX_GRAS_SU_CNTL_LINEHALFWIDTH(0.0) |
			COND(zsbuf->b.b.nr_samples > 1, A5XX_GRAS_SU_CNTL_MSAA_ENABLE));

	OUT_PKT4(ring, REG_A5XX_GRAS_CNTL, 1);
	OUT_RING(ring, 0x00000000);

	OUT_PKT4(ring, REG_A5XX_GRAS_CL_CNTL, 1);
	OUT_RING(ring, 0x00000181);

	OUT_PKT4(ring, REG_A5XX_GRAS_LRZ_CNTL, 1);
	OUT_RING(ring, 0x00000000);

	OUT_PKT4(ring, REG_A5XX_RB_MRT_BUF_INFO(0), 5);
	OUT_RING(ring, A5XX_RB_MRT_BUF_INFO_COLOR_FORMAT(RB5_R16_UNORM) |
			A5XX_RB_MRT_BUF_INFO_COLOR_TILE_MODE(TILE5_LINEAR) |
			A5XX_RB_MRT_BUF_INFO_COLOR_SWAP(WZYX));
	OUT_RING(ring, A5XX_RB_MRT_PITCH(zsbuf->lrz_pitch * 2));
	OUT_RING(ring, A5XX_RB_MRT_ARRAY_PITCH(fd_bo_size(zsbuf->lrz)));
	OUT_RELOC(ring, zsbuf->lrz, 0x1000, 0, 0);

	OUT_PKT4(ring, REG_A5XX_RB_RENDER_CNTL, 1);
	OUT_RING(ring, 0x00000000);

	OUT_PKT4(ring, REG_A5XX_RB_DEST_MSAA_CNTL, 1);
	OUT_RING(ring, A5XX_RB_DEST_MSAA_CNTL_SAMPLES(MSAA_ONE));

	OUT_PKT4(ring, REG_A5XX_RB_BLIT_CNTL, 1);
	OUT_RING(ring, A5XX_RB_BLIT_CNTL_BUF(BLIT_MRT0));

	OUT_PKT4(ring, REG_A5XX_RB_CLEAR_CNTL, 1);
	OUT_RING(ring, A5XX_RB_CLEAR_CNTL_FAST_CLEAR |
			A5XX_RB_CLEAR_CNTL_MASK(0xf));

	OUT_PKT4(ring, REG_A5XX_RB_CLEAR_COLOR_DW0, 1);
	OUT_RING(ring, clear);  /* RB_CLEAR_COLOR_DW0 */

	OUT_PKT4(ring, REG_A5XX_VSC_RESOLVE_CNTL, 2);
	OUT_RING(ring, A5XX_VSC_RESOLVE_CNTL_X(zsbuf->lrz_width) |
			 A5XX_VSC_RESOLVE_CNTL_Y(zsbuf->lrz_height));
	OUT_RING(ring, 0x00000000);   // XXX UNKNOWN_0CDE

	OUT_PKT4(ring, REG_A5XX_RB_CNTL, 1);
	OUT_RING(ring, A5XX_RB_CNTL_BYPASS);

	OUT_PKT4(ring, REG_A5XX_RB_RESOLVE_CNTL_1, 2);
	OUT_RING(ring, A5XX_RB_RESOLVE_CNTL_1_X(0) |
			A5XX_RB_RESOLVE_CNTL_1_Y(0));
	OUT_RING(ring, A5XX_RB_RESOLVE_CNTL_2_X(zsbuf->lrz_width - 1) |
			A5XX_RB_RESOLVE_CNTL_2_Y(zsbuf->lrz_height - 1));

	fd5_emit_blit(batch, ring);
}

static bool
fd5_clear(struct fd_context *ctx, unsigned buffers,
		const union pipe_color_union *color, double depth, unsigned stencil)
	assert_dt
{
	struct fd_ringbuffer *ring = ctx->batch->draw;
	struct pipe_framebuffer_state *pfb = &ctx->batch->framebuffer;

	if ((buffers & (PIPE_CLEAR_DEPTH | PIPE_CLEAR_STENCIL)) &&
			is_z32(pfb->zsbuf->format))
		return false;

	fd5_emit_render_cntl(ctx, true, false);

	if (buffers & PIPE_CLEAR_COLOR) {
		for (int i = 0; i < pfb->nr_cbufs; i++) {
			union util_color uc = {0};

			if (!pfb->cbufs[i])
				continue;

			if (!(buffers & (PIPE_CLEAR_COLOR0 << i)))
				continue;

			enum pipe_format pfmt = pfb->cbufs[i]->format;

			// XXX I think RB_CLEAR_COLOR_DWn wants to take into account SWAP??
			union pipe_color_union swapped;
			switch (fd5_pipe2swap(pfmt)) {
			case WZYX:
				swapped.ui[0] = color->ui[0];
				swapped.ui[1] = color->ui[1];
				swapped.ui[2] = color->ui[2];
				swapped.ui[3] = color->ui[3];
				break;
			case WXYZ:
				swapped.ui[2] = color->ui[0];
				swapped.ui[1] = color->ui[1];
				swapped.ui[0] = color->ui[2];
				swapped.ui[3] = color->ui[3];
				break;
			case ZYXW:
				swapped.ui[3] = color->ui[0];
				swapped.ui[0] = color->ui[1];
				swapped.ui[1] = color->ui[2];
				swapped.ui[2] = color->ui[3];
				break;
			case XYZW:
				swapped.ui[3] = color->ui[0];
				swapped.ui[2] = color->ui[1];
				swapped.ui[1] = color->ui[2];
				swapped.ui[0] = color->ui[3];
				break;
			}

			util_pack_color_union(pfmt, &uc, &swapped);

			OUT_PKT4(ring, REG_A5XX_RB_BLIT_CNTL, 1);
			OUT_RING(ring, A5XX_RB_BLIT_CNTL_BUF(BLIT_MRT0 + i));

			OUT_PKT4(ring, REG_A5XX_RB_CLEAR_CNTL, 1);
			OUT_RING(ring, A5XX_RB_CLEAR_CNTL_FAST_CLEAR |
					A5XX_RB_CLEAR_CNTL_MASK(0xf));

			OUT_PKT4(ring, REG_A5XX_RB_CLEAR_COLOR_DW0, 4);
			OUT_RING(ring, uc.ui[0]);  /* RB_CLEAR_COLOR_DW0 */
			OUT_RING(ring, uc.ui[1]);  /* RB_CLEAR_COLOR_DW1 */
			OUT_RING(ring, uc.ui[2]);  /* RB_CLEAR_COLOR_DW2 */
			OUT_RING(ring, uc.ui[3]);  /* RB_CLEAR_COLOR_DW3 */

			fd5_emit_blit(ctx->batch, ring);
		}
	}

	if (pfb->zsbuf && (buffers & (PIPE_CLEAR_DEPTH | PIPE_CLEAR_STENCIL))) {
		uint32_t clear =
			util_pack_z_stencil(pfb->zsbuf->format, depth, stencil);
		uint32_t mask = 0;

		if (buffers & PIPE_CLEAR_DEPTH)
			mask |= 0x1;

		if (buffers & PIPE_CLEAR_STENCIL)
			mask |= 0x2;

		OUT_PKT4(ring, REG_A5XX_RB_BLIT_CNTL, 1);
		OUT_RING(ring, A5XX_RB_BLIT_CNTL_BUF(BLIT_ZS));

		OUT_PKT4(ring, REG_A5XX_RB_CLEAR_CNTL, 1);
		OUT_RING(ring, A5XX_RB_CLEAR_CNTL_FAST_CLEAR |
				A5XX_RB_CLEAR_CNTL_MASK(mask));

		OUT_PKT4(ring, REG_A5XX_RB_CLEAR_COLOR_DW0, 1);
		OUT_RING(ring, clear);    /* RB_CLEAR_COLOR_DW0 */

		fd5_emit_blit(ctx->batch, ring);

		if (pfb->zsbuf && (buffers & PIPE_CLEAR_DEPTH)) {
			struct fd_resource *zsbuf = fd_resource(pfb->zsbuf->texture);
			if (zsbuf->lrz) {
				zsbuf->lrz_valid = true;
				fd5_clear_lrz(ctx->batch, zsbuf, depth);
			}
		}
	}

	/* disable fast clear to not interfere w/ gmem->mem, etc.. */
	OUT_PKT4(ring, REG_A5XX_RB_CLEAR_CNTL, 1);
	OUT_RING(ring, 0x00000000);   /* RB_CLEAR_CNTL */

	return true;
}

void
fd5_draw_init(struct pipe_context *pctx)
	disable_thread_safety_analysis
{
	struct fd_context *ctx = fd_context(pctx);
	ctx->draw_vbo = fd5_draw_vbo;
	ctx->clear = fd5_clear;
}
