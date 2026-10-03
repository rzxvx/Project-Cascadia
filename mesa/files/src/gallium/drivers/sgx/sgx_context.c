/*
 * Copyright 2026 Project Cascadia
 * SPDX-License-Identifier: MIT
 *
 * The context.  State is kept and not used yet: nothing draws until the
 * compiler exists (docs/research/p105-mesa.md, M13).  A clear of a whole
 * render target the template frame can render into is a render on the GPU;
 * every other clear, copy and blit is done by the CPU through the buffers'
 * mappings, which wait for the renders using them.
 */
#include "sgx_context.h"

#include "compiler/nir/nir.h"
#include "util/log.h"
#include "util/os_time.h"
#include "util/ralloc.h"
#include "util/u_framebuffer.h"
#include "util/u_helpers.h"
#include "util/u_inlines.h"
#include "util/u_memory.h"
#include "util/u_surface.h"
#include "util/u_upload_mgr.h"

#include "sgx_device.h"
#include "sgx_frame.h"
#include "sgx_resource.h"
#include "sgx_screen.h"

/* ---- fences ------------------------------------------------------------- */

static void
sgx_fence_reference_hook(struct pipe_screen *pscreen, struct pipe_fence_handle **ptr,
                         struct pipe_fence_handle *fence)
{
   sgx_fence_reference((struct sgx_fence **)ptr, (struct sgx_fence *)fence);
}

static bool
sgx_fence_finish(struct pipe_screen *pscreen, struct pipe_context *pctx,
                 struct pipe_fence_handle *fence, uint64_t timeout)
{
   return sgx_fence_wait((struct sgx_fence *)fence, timeout);
}

void
sgx_context_screen_init(struct sgx_screen *screen)
{
   screen->base.fence_reference = sgx_fence_reference_hook;
   screen->base.fence_finish = sgx_fence_finish;
}

/* Renders are submitted as they are made, so a flush only hands out the
 * last one's fence. */
static void
sgx_flush(struct pipe_context *pctx, struct pipe_fence_handle **fence, unsigned flags)
{
   struct sgx_context *ctx = sgx_context(pctx);
   struct sgx_fence *f = NULL;

   if (!fence)
      return;
   if (ctx->last)
      sgx_fence_reference(&f, ctx->last);
   else
      f = sgx_fence_create(&sgx_screen(pctx->screen)->dev, true);
   sgx_fence_reference((struct sgx_fence **)fence, NULL);
   *fence = (struct pipe_fence_handle *)f;
}

/* ---- clears ------------------------------------------------------------- */

/* The whole of a render target, on the GPU; false if the frame cannot */
static bool
sgx_clear_gpu(struct sgx_context *ctx, struct pipe_surface *surf,
              const union pipe_color_union *color)
{
   struct sgx_screen *screen = sgx_screen(ctx->base.screen);
   struct sgx_resource *rt = sgx_resource(surf->texture);
   struct sgx_fence *done;
   int ret;

   if (!screen->frame || surf->level || surf->first_layer ||
       !sgx_frame_can_render(screen->frame, rt))
      return false;
   if (!(done = sgx_fence_create(&screen->dev, false)))
      return false;
   simple_mtx_lock(&screen->frame_lock);
   ret = sgx_frame_clear(screen->frame, rt, color->f, done);
   simple_mtx_unlock(&screen->frame_lock);
   if (!ret)
      sgx_fence_reference(&ctx->last, done);
   sgx_fence_reference(&done, NULL);
   return !ret;
}

static void
sgx_clear(struct pipe_context *pctx, unsigned buffers, uint32_t color_clear_mask,
          uint8_t stencil_clear_mask, const struct pipe_scissor_state *scissor,
          const union pipe_color_union *color, double depth, unsigned stencil)
{
   struct sgx_context *ctx = sgx_context(pctx);
   unsigned x = scissor ? scissor->minx : 0, y = scissor ? scissor->miny : 0;
   unsigned w = scissor ? scissor->maxx - scissor->minx : ctx->fb.width;
   unsigned h = scissor ? scissor->maxy - scissor->miny : ctx->fb.height;

   for (unsigned i = 0; i < ctx->fb.nr_cbufs; i++) {
      struct pipe_surface *surf = &ctx->fb.cbufs[i];

      if (!(buffers & (PIPE_CLEAR_COLOR0 << i)) || !surf->texture)
         continue;
      /* four channel bits per draw buffer */
      if (!scissor && ((color_clear_mask >> (4 * i)) & 0xf) == 0xf &&
          sgx_clear_gpu(ctx, surf, color))
         continue;
      util_clear_render_target(pctx, surf, color, x, y, w, h);
   }
   if ((buffers & PIPE_CLEAR_DEPTHSTENCIL) && ctx->fb.zsbuf.texture)
      util_clear_depth_stencil(pctx, &ctx->fb.zsbuf, buffers & PIPE_CLEAR_DEPTHSTENCIL,
                               depth, stencil, x, y, w, h);
}

static void
sgx_clear_render_target(struct pipe_context *pctx, struct pipe_surface *dst,
                        const union pipe_color_union *color, unsigned x, unsigned y,
                        unsigned w, unsigned h, bool render_condition_enabled)
{
   if (!x && !y && w == dst->texture->width0 && h == dst->texture->height0 &&
       sgx_clear_gpu(sgx_context(pctx), dst, color))
      return;
   util_clear_render_target(pctx, dst, color, x, y, w, h);
}

static void
sgx_clear_depth_stencil(struct pipe_context *pctx, struct pipe_surface *dst,
                        unsigned flags, double depth, unsigned stencil, unsigned x,
                        unsigned y, unsigned w, unsigned h, bool render_condition_enabled)
{
   util_clear_depth_stencil(pctx, dst, flags, depth, stencil, x, y, w, h);
}

/* ---- copies and blits, by the CPU --------------------------------------- */

static void
sgx_blit(struct pipe_context *pctx, const struct pipe_blit_info *info)
{
   if (util_try_blit_via_copy_region(pctx, info, false))
      return;
   mesa_logw_once("sgx: blits that convert or scale are not done yet");
}

static void
sgx_flush_resource(struct pipe_context *pctx, struct pipe_resource *prsc)
{
}

/* ---- draws -------------------------------------------------------------- */

static void
sgx_draw_vbo(struct pipe_context *pctx, const struct pipe_draw_info *info,
             unsigned drawid_offset, const struct pipe_draw_indirect_info *indirect,
             const struct pipe_draw_start_count_bias *draws, unsigned num_draws)
{
   struct sgx_context *ctx = sgx_context(pctx);

   if (!ctx->warned_draw)
      mesa_logw("sgx: draws are not done yet (no shader compiler): dropped");
   ctx->warned_draw = true;
}

/* ---- state, kept for later ---------------------------------------------- */

static void *
sgx_create_copy(const void *state, size_t size)
{
   void *copy = MALLOC(size);

   if (copy)
      memcpy(copy, state, size);
   return copy;
}

static void *
sgx_create_blend_state(struct pipe_context *pctx, const struct pipe_blend_state *s)
{
   return sgx_create_copy(s, sizeof(*s));
}

static void *
sgx_create_dsa_state(struct pipe_context *pctx,
                     const struct pipe_depth_stencil_alpha_state *s)
{
   return sgx_create_copy(s, sizeof(*s));
}

static void *
sgx_create_rs_state(struct pipe_context *pctx, const struct pipe_rasterizer_state *s)
{
   return sgx_create_copy(s, sizeof(*s));
}

static void *
sgx_create_sampler_state(struct pipe_context *pctx, const struct pipe_sampler_state *s)
{
   return sgx_create_copy(s, sizeof(*s));
}

static void *
sgx_create_vertex_elements(struct pipe_context *pctx, unsigned count,
                           const struct pipe_vertex_element *e)
{
   return sgx_create_copy(e, count * sizeof(*e));
}

static void *
sgx_create_shader_state(struct pipe_context *pctx, const struct pipe_shader_state *s)
{
   struct sgx_shader *sh = CALLOC_STRUCT(sgx_shader);

   if (!sh)
      return NULL;
   /* the state tracker hands over its NIR; the compiler (M13) will take
    * it from here */
   if (s->type == PIPE_SHADER_IR_NIR)
      sh->nir = s->ir.nir;
   return sh;
}

static void
sgx_delete_shader_state(struct pipe_context *pctx, void *state)
{
   struct sgx_shader *sh = state;

   ralloc_free(sh->nir);
   FREE(sh);
}

static void
sgx_bind_state(struct pipe_context *pctx, void *state)
{
}

static void
sgx_delete_state(struct pipe_context *pctx, void *state)
{
   FREE(state);
}

static void
sgx_bind_sampler_states(struct pipe_context *pctx, mesa_shader_stage shader,
                        unsigned start, unsigned count, void **states)
{
}

static struct pipe_sampler_view *
sgx_create_sampler_view(struct pipe_context *pctx, struct pipe_resource *prsc,
                        const struct pipe_sampler_view *templ)
{
   struct pipe_sampler_view *view = CALLOC_STRUCT(pipe_sampler_view);

   if (!view)
      return NULL;
   *view = *templ;
   view->texture = NULL;
   pipe_resource_reference(&view->texture, prsc);
   pipe_reference_init(&view->reference, 1);
   view->context = pctx;
   return view;
}

static void
sgx_sampler_view_destroy(struct pipe_context *pctx, struct pipe_sampler_view *view)
{
   pipe_resource_reference(&view->texture, NULL);
   FREE(view);
}

static void
sgx_set_sampler_views(struct pipe_context *pctx, mesa_shader_stage shader,
                      unsigned start, unsigned count, unsigned unbind_trailing,
                      struct pipe_sampler_view **views)
{
}

static void
sgx_set_framebuffer_state(struct pipe_context *pctx,
                          const struct pipe_framebuffer_state *fb)
{
   util_copy_framebuffer_state(&sgx_context(pctx)->fb, fb);
}

static void
sgx_set_blend_color(struct pipe_context *pctx, const struct pipe_blend_color *c)
{
}

static void
sgx_set_clip_state(struct pipe_context *pctx, const struct pipe_clip_state *c)
{
}

static void
sgx_set_constant_buffer(struct pipe_context *pctx, mesa_shader_stage shader,
                        uint index, const struct pipe_constant_buffer *cb)
{
}

static void
sgx_set_polygon_stipple(struct pipe_context *pctx, const struct pipe_poly_stipple *s)
{
}

static void
sgx_set_sample_mask(struct pipe_context *pctx, unsigned mask)
{
}

static void
sgx_set_scissor_states(struct pipe_context *pctx, unsigned start, unsigned count,
                       const struct pipe_scissor_state *s)
{
}

static void
sgx_set_stencil_ref(struct pipe_context *pctx, const struct pipe_stencil_ref ref)
{
}

static void
sgx_set_viewport_states(struct pipe_context *pctx, unsigned start, unsigned count,
                        const struct pipe_viewport_state *v)
{
}

static void
sgx_set_vertex_buffers(struct pipe_context *pctx, unsigned count,
                       const struct pipe_vertex_buffer *buffers)
{
   struct sgx_context *ctx = sgx_context(pctx);

   /* takes over the references the state tracker hands in */
   util_set_vertex_buffers_mask(ctx->vb, &ctx->vb_mask, buffers, count);
}

static void
sgx_texture_barrier(struct pipe_context *pctx, unsigned flags)
{
}

static void
sgx_memory_barrier(struct pipe_context *pctx, unsigned flags)
{
}

static enum pipe_reset_status
sgx_get_device_reset_status(struct pipe_context *pctx)
{
   return PIPE_NO_RESET;
}

/* ---- the context -------------------------------------------------------- */

static void
sgx_context_destroy(struct pipe_context *pctx)
{
   struct sgx_context *ctx = sgx_context(pctx);

   util_unreference_framebuffer_state(&ctx->fb);
   util_set_vertex_buffers_mask(ctx->vb, &ctx->vb_mask, NULL, 0);
   if (pctx->stream_uploader)
      u_upload_destroy(pctx->stream_uploader);
   sgx_fence_reference(&ctx->last, NULL);
   slab_destroy_child(&ctx->transfer_pool);
   FREE(ctx);
}

struct pipe_context *
sgx_context_create(struct pipe_screen *pscreen, void *priv, unsigned flags)
{
   struct sgx_screen *screen = sgx_screen(pscreen);
   struct sgx_context *ctx = CALLOC_STRUCT(sgx_context);
   struct pipe_context *p;

   if (!ctx)
      return NULL;
   p = &ctx->base;
   p->screen = pscreen;
   p->priv = priv;
   slab_create_child(&ctx->transfer_pool, &screen->transfer_pool);

   p->destroy = sgx_context_destroy;
   p->flush = sgx_flush;
   p->clear = sgx_clear;
   p->clear_render_target = sgx_clear_render_target;
   p->clear_depth_stencil = sgx_clear_depth_stencil;
   p->clear_texture = util_clear_texture_sw;
   p->resource_copy_region = util_resource_copy_region;
   p->blit = sgx_blit;
   p->flush_resource = sgx_flush_resource;
   p->draw_vbo = sgx_draw_vbo;
   p->texture_barrier = sgx_texture_barrier;
   p->memory_barrier = sgx_memory_barrier;
   p->get_device_reset_status = sgx_get_device_reset_status;

   p->create_blend_state = sgx_create_blend_state;
   p->bind_blend_state = sgx_bind_state;
   p->delete_blend_state = sgx_delete_state;
   p->create_depth_stencil_alpha_state = sgx_create_dsa_state;
   p->bind_depth_stencil_alpha_state = sgx_bind_state;
   p->delete_depth_stencil_alpha_state = sgx_delete_state;
   p->create_rasterizer_state = sgx_create_rs_state;
   p->bind_rasterizer_state = sgx_bind_state;
   p->delete_rasterizer_state = sgx_delete_state;
   p->create_sampler_state = sgx_create_sampler_state;
   p->bind_sampler_states = sgx_bind_sampler_states;
   p->delete_sampler_state = sgx_delete_state;
   p->create_vertex_elements_state = sgx_create_vertex_elements;
   p->bind_vertex_elements_state = sgx_bind_state;
   p->delete_vertex_elements_state = sgx_delete_state;
   p->create_vs_state = sgx_create_shader_state;
   p->bind_vs_state = sgx_bind_state;
   p->delete_vs_state = sgx_delete_shader_state;
   p->create_fs_state = sgx_create_shader_state;
   p->bind_fs_state = sgx_bind_state;
   p->delete_fs_state = sgx_delete_shader_state;
   p->create_sampler_view = sgx_create_sampler_view;
   p->sampler_view_destroy = sgx_sampler_view_destroy;
   p->sampler_view_release = u_default_sampler_view_release;
   p->set_sampler_views = sgx_set_sampler_views;
   p->set_framebuffer_state = sgx_set_framebuffer_state;
   p->set_blend_color = sgx_set_blend_color;
   p->set_clip_state = sgx_set_clip_state;
   p->set_constant_buffer = sgx_set_constant_buffer;
   p->set_polygon_stipple = sgx_set_polygon_stipple;
   p->set_sample_mask = sgx_set_sample_mask;
   p->set_scissor_states = sgx_set_scissor_states;
   p->set_stencil_ref = sgx_set_stencil_ref;
   p->set_viewport_states = sgx_set_viewport_states;
   p->set_vertex_buffers = sgx_set_vertex_buffers;
   sgx_resource_context_init(p);

   p->stream_uploader = u_upload_create_default(p);
   if (!p->stream_uploader) {
      sgx_context_destroy(p);
      return NULL;
   }
   p->const_uploader = p->stream_uploader;
   return p;
}
