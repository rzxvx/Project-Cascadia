/*
 * Copyright 2026 Project Cascadia
 * SPDX-License-Identifier: MIT
 *
 * The context.  Draws go through sgx_draw.c (M13a: vertex shaders on the
 * CPU in the draw module, triangles on the GPU through the template frame),
 * which is handed the state it uses; the rest is kept for later.  A clear
 * of a whole render target the template frame can render into is a render
 * on the GPU; every other clear, copy and blit is done by the CPU through
 * the buffers' mappings, which wait for the renders using them.
 */
#include "sgx_context.h"

#include "compiler/nir/nir.h"
#include "draw/draw_context.h"
#include "util/log.h"
#include "util/os_time.h"
#include "util/ralloc.h"
#include "util/u_debug.h"
#include "util/u_framebuffer.h"
#include "util/u_helpers.h"
#include "util/u_inlines.h"
#include "util/u_memory.h"
#include "util/u_surface.h"
#include "util/u_upload_mgr.h"

#include "sgx_compiler.h"
#include "sgx_device.h"
#include "sgx_draw.h"
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

   sgx_batch_flush(ctx);
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
   if (!ret) {
      sgx_fence_reference(&ctx->last, done);
      rt->seq++;
      rt->gpu_written = true;
   }
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

   /* what was drawn before comes first; and a render starts at the far
    * depth, so a depth clear starts a new one */
   sgx_batch_flush(ctx);
   /* every render starts at the last depth clear's value (there is no
    * loading of depth from memory yet) */
   if (buffers & PIPE_CLEAR_DEPTH)
      ctx->batch.depth_clear = depth;

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
   sgx_batch_flush(sgx_context(pctx));
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
   sgx_batch_flush(sgx_context(pctx));
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
   if (sgx_batch_uses(sgx_context(pctx), prsc))
      sgx_batch_flush(sgx_context(pctx));
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
   struct sgx_vertex_elements *ve = CALLOC_STRUCT(sgx_vertex_elements);

   if (!ve)
      return NULL;
   ve->count = MIN2(count, PIPE_MAX_ATTRIBS);
   memcpy(ve->e, e, ve->count * sizeof(*e));
   return ve;
}

static void
sgx_bind_vertex_elements(struct pipe_context *pctx, void *state)
{
   struct sgx_vertex_elements *ve = state;

   sgx_context(pctx)->velems = ve;
   if (ve)
      draw_set_vertex_elements(sgx_context(pctx)->draw, ve->count, ve->e);
}

/* The state tracker hands over its NIR, which the shader keeps (the
 * compiler, M13, will take it from there).  For now a vertex shader also
 * goes to the draw module, which runs it on the CPU (a copy: it is turned
 * into TGSI and freed), and a fragment shader's colour is read off it. */
static void *
sgx_create_shader_state(struct pipe_context *pctx, const struct pipe_shader_state *s)
{
   struct sgx_context *ctx = sgx_context(pctx);
   struct sgx_shader *sh = CALLOC_STRUCT(sgx_shader);

   if (!sh)
      return NULL;
   if (s->type != PIPE_SHADER_IR_NIR)
      return sh;
   sh->nir = s->ir.nir;
   if (sh->nir->info.stage == MESA_SHADER_VERTEX && ctx->draw) {
      struct pipe_shader_state copy = *s;

      copy.ir.nir = nir_shader_clone(NULL, sh->nir);
      sh->draw = draw_create_vertex_shader(ctx->draw, &copy);
      if (!sh->draw)
         mesa_logw("sgx: the draw module did not take a vertex shader");
   } else if (sh->nir->info.stage == MESA_SHADER_FRAGMENT) {
      char why[128];

      sgx_fs_colour_analyse(sh->nir, &sh->colour);
      /* SGX_NOCOMPILE=1: M13a's way only */
      if (!debug_get_bool_option("SGX_NOCOMPILE", false)) {
         sh->compiled = sgx_compile_fs(sh->nir, NULL, why, sizeof(why));
         if (!sh->compiled)
            mesa_logw("sgx: a fragment shader not compiled (%s): its colour is worked "
                      "out per vertex", why);
         else if (ctx->debug_draw || debug_get_bool_option("SGX_DEBUG_SHADER", false)) {
            mesa_logi("sgx: a fragment shader compiled: %u instructions, %u temps, "
                      "%u inputs, %u sa words, %u textures", sh->compiled->prog.ncode,
                      sh->compiled->prog.ntemps, sh->compiled->prog.ninputs,
                      sh->compiled->prog.nsa, sh->compiled->nsamplers);
            /* SGX_DEBUG_SHADER=1: the code, for tools/iosgpu/usse-dis.py words */
            for (unsigned i = 0; debug_get_bool_option("SGX_DEBUG_SHADER", false) &&
                                 i < sh->compiled->prog.ncode; i++)
               mesa_logi("sgx:   %016llx", (unsigned long long)sh->compiled->prog.code[i]);
         }
      }
   }
   return sh;
}

static void
sgx_delete_shader_state(struct pipe_context *pctx, void *state)
{
   struct sgx_context *ctx = sgx_context(pctx);
   struct sgx_shader *sh = state;

   if (ctx->vs == sh) {
      ctx->vs = NULL;
      if (ctx->draw)
         draw_bind_vertex_shader(ctx->draw, NULL);
   }
   if (sh->draw)
      draw_delete_vertex_shader(ctx->draw, sh->draw);
   if (ctx->fs == sh)
      ctx->fs = NULL;
   /* its code stays where the frame put it: that place is not reused; a
    * render gathered with it goes first */
   if (sh->nvs)
      sgx_batch_flush(ctx);
   sgx_fs_destroy(sh->compiled);
   for (unsigned i = 0; i < sh->nvariants; i++)
      sgx_fs_destroy(sh->variant[i]);
   for (unsigned i = 0; i < sh->nvs; i++)
      sgx_vs_destroy(sh->vs_variant[i]);
   ralloc_free(sh->nir);
   FREE(sh);
}

static void
sgx_bind_vs_state(struct pipe_context *pctx, void *state)
{
   struct sgx_context *ctx = sgx_context(pctx);

   ctx->vs = state;
   if (ctx->draw)
      draw_bind_vertex_shader(ctx->draw, ctx->vs ? ctx->vs->draw : NULL);
}

static void
sgx_bind_fs_state(struct pipe_context *pctx, void *state)
{
   sgx_context(pctx)->fs = state;
}

static void
sgx_bind_rs_state(struct pipe_context *pctx, void *state)
{
   struct sgx_context *ctx = sgx_context(pctx);

   ctx->rast = state;
   if (ctx->draw && state)
      draw_set_rasterizer_state(ctx->draw, state, state);
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
   struct sgx_context *ctx = sgx_context(pctx);

   if (shader != MESA_SHADER_FRAGMENT)
      return;
   for (unsigned i = 0; i < count && start + i < PIPE_MAX_SAMPLERS; i++)
      ctx->fs_samplers[start + i] = states ? states[i] : NULL;
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
   struct sgx_context *ctx = sgx_context(pctx);

   if (shader != MESA_SHADER_FRAGMENT)
      return;
   for (unsigned i = 0; i < count + unbind_trailing; i++)
      if (start + i < PIPE_MAX_SHADER_SAMPLER_VIEWS)
         pipe_sampler_view_reference(&ctx->fs_views[start + i],
                                     views && i < count ? views[i] : NULL);
}

static void
sgx_set_framebuffer_state(struct pipe_context *pctx,
                          const struct pipe_framebuffer_state *fb)
{
   struct sgx_context *ctx = sgx_context(pctx);

   if (!util_framebuffer_state_equal(&ctx->fb, fb))
      sgx_batch_flush(ctx);
   util_copy_framebuffer_state(&ctx->fb, fb);
}

static void
sgx_set_blend_color(struct pipe_context *pctx, const struct pipe_blend_color *c)
{
   sgx_context(pctx)->blend_color = *c;
}

static void
sgx_bind_blend_state(struct pipe_context *pctx, void *state)
{
   sgx_context(pctx)->blend = state;
}

static void
sgx_bind_dsa_state(struct pipe_context *pctx, void *state)
{
   sgx_context(pctx)->dsa = state;
}

static void
sgx_set_clip_state(struct pipe_context *pctx, const struct pipe_clip_state *c)
{
   struct sgx_context *ctx = sgx_context(pctx);

   if (ctx->draw)
      draw_set_clip_state(ctx->draw, c);
}

/* buffer 0 of the vertex and the fragment shader: read at draw time */
static void
sgx_set_constant_buffer(struct pipe_context *pctx, mesa_shader_stage shader,
                        uint index, const struct pipe_constant_buffer *cb)
{
   struct sgx_context *ctx = sgx_context(pctx);

   if (index == 0 && (shader == MESA_SHADER_VERTEX || shader == MESA_SHADER_FRAGMENT))
      util_copy_constant_buffer(&ctx->cb[shader == MESA_SHADER_FRAGMENT], cb);
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
   struct sgx_context *ctx = sgx_context(pctx);

   if (start == 0 && count)
      ctx->viewport = v[0];
   if (ctx->draw)
      draw_set_viewport_states(ctx->draw, start, count, v);
}

static void
sgx_set_vertex_buffers(struct pipe_context *pctx, unsigned count,
                       const struct pipe_vertex_buffer *buffers)
{
   struct sgx_context *ctx = sgx_context(pctx);

   /* the draw module takes references of its own; then ours takes over the
    * ones the state tracker hands in */
   if (ctx->draw)
      draw_set_vertex_buffers(ctx->draw, count, buffers);
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

   sgx_draw_fini(ctx);
   for (unsigned i = 0; i < ARRAY_SIZE(ctx->fs_views); i++)
      pipe_sampler_view_reference(&ctx->fs_views[i], NULL);
   util_unreference_framebuffer_state(&ctx->fb);
   util_set_vertex_buffers_mask(ctx->vb, &ctx->vb_mask, NULL, 0);
   for (unsigned i = 0; i < ARRAY_SIZE(ctx->cb); i++)
      pipe_resource_reference(&ctx->cb[i].buffer, NULL);
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
   ctx->batch.depth_clear = 1.0f;
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
   ctx->debug_draw = debug_get_bool_option("SGX_DEBUG_DRAW", false);
   p->texture_barrier = sgx_texture_barrier;
   p->memory_barrier = sgx_memory_barrier;
   p->get_device_reset_status = sgx_get_device_reset_status;
   p->resource_release = u_default_resource_release;

   p->create_blend_state = sgx_create_blend_state;
   p->bind_blend_state = sgx_bind_blend_state;
   p->delete_blend_state = sgx_delete_state;
   p->create_depth_stencil_alpha_state = sgx_create_dsa_state;
   p->bind_depth_stencil_alpha_state = sgx_bind_dsa_state;
   p->delete_depth_stencil_alpha_state = sgx_delete_state;
   p->create_rasterizer_state = sgx_create_rs_state;
   p->bind_rasterizer_state = sgx_bind_rs_state;
   p->delete_rasterizer_state = sgx_delete_state;
   p->create_sampler_state = sgx_create_sampler_state;
   p->bind_sampler_states = sgx_bind_sampler_states;
   p->delete_sampler_state = sgx_delete_state;
   p->create_vertex_elements_state = sgx_create_vertex_elements;
   p->bind_vertex_elements_state = sgx_bind_vertex_elements;
   p->delete_vertex_elements_state = sgx_delete_state;
   p->create_vs_state = sgx_create_shader_state;
   p->bind_vs_state = sgx_bind_vs_state;
   p->delete_vs_state = sgx_delete_shader_state;
   p->create_fs_state = sgx_create_shader_state;
   p->bind_fs_state = sgx_bind_fs_state;
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

   /* after the hooks: the draw module looks at them */
   if (!sgx_draw_init(ctx))
      mesa_logw("sgx: no draw module: draws are dropped");

   p->stream_uploader = u_upload_create_default(p);
   if (!p->stream_uploader) {
      sgx_context_destroy(p);
      return NULL;
   }
   p->const_uploader = p->stream_uploader;
   return p;
}
