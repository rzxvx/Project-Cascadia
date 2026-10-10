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

#include <time.h>
#include <unistd.h>

#include "compiler/nir/nir.h"
#include "draw/draw_context.h"
#include "util/format/u_format.h"
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

/* native fence fds (EGL_ANDROID_native_fence_sync): sync files of the
 * syncobjs -- what lets a KMS client (SDL) commit a flip without waiting
 * for the render, the kernel waiting for it instead (M25) */
static int
sgx_fence_get_fd(struct pipe_screen *pscreen, struct pipe_fence_handle *fence)
{
   return sgx_fence_export((struct sgx_fence *)fence);
}

static void
sgx_create_fence_fd(struct pipe_context *pctx, struct pipe_fence_handle **fence, int fd,
                    enum pipe_fd_type type)
{
   *fence = type == PIPE_FD_TYPE_NATIVE_SYNC ?
            (struct pipe_fence_handle *)sgx_fence_import(&sgx_screen(pctx->screen)->dev, fd) :
            NULL;
}

/* the next submit waits for it on the GPU (a copy of its syncobj's fence:
 * the submit lets go of it) */
static void
sgx_fence_server_sync(struct pipe_context *pctx, struct pipe_fence_handle *fence, uint64_t value)
{
   struct sgx_device *dev = &sgx_screen(pctx->screen)->dev;
   struct sgx_fence *f = (struct sgx_fence *)fence;
   struct sgx_fence *copy = NULL;
   int fd;

   sgx_trace_mark("server sync");
   sgx_batch_flush(sgx_context(pctx));
   /* (a binary syncobj's fence copied through a sync file) */
   if (dev->nwait_syncs < ARRAY_SIZE(dev->wait_syncs) && (fd = sgx_fence_export(f)) >= 0) {
      copy = sgx_fence_import(dev, fd);
      close(fd);
   }
   if (!copy) {
      sgx_fence_wait(f, OS_TIMEOUT_INFINITE);
      return;
   }
   /* the syncobj is the submit's to destroy */
   dev->wait_syncs[dev->nwait_syncs++] = copy->syncobj;
   copy->syncobj = 0;
   FREE(copy);
}

void
sgx_context_screen_init(struct sgx_screen *screen)
{
   screen->base.fence_reference = sgx_fence_reference_hook;
   screen->base.fence_finish = sgx_fence_finish;
   screen->base.fence_get_fd = sgx_fence_get_fd;
}

/* Renders are submitted as they are made, so a flush only hands out the
 * last one's fence. */
static void
sgx_flush(struct pipe_context *pctx, struct pipe_fence_handle **fence, unsigned flags)
{
   struct sgx_context *ctx = sgx_context(pctx);
   struct sgx_fence *f = NULL;

   sgx_trace_mark(flags & PIPE_FLUSH_END_OF_FRAME ? "flush, end of frame" : "flush");
   sgx_batch_flush(ctx);
   if (ctx->debug_fps && (flags & PIPE_FLUSH_END_OF_FRAME)) {
      struct timespec ts;
      int64_t now = os_time_get_nano(), cpu;

      clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
      cpu = ts.tv_sec * 1000000000ll + ts.tv_nsec;

      ctx->stat_swap = now;
      ctx->stat_swap_cpu = cpu;
      if (!ctx->stat_t0) {
         ctx->stat_t0 = now;
         ctx->stat_cpu0 = cpu;
      } else if (++ctx->stat_frames, now - ctx->stat_t0 >= 2000000000ll) {
         double s = (now - ctx->stat_t0) / 1e9;

         mesa_logi("sgx: %.1f fps, %.1f renders and %.1f draws a frame, %.0f%% CPU, "
                   "%.0f%% waiting for the GPU", ctx->stat_frames / s,
                   (double)ctx->stat_renders / ctx->stat_frames,
                   (double)ctx->stat_draws / ctx->stat_frames,
                   100.0 * (cpu - ctx->stat_cpu0) / (now - ctx->stat_t0),
                   100.0 * sgx_fence_waited / (now - ctx->stat_t0));
         sgx_fence_waited = 0;
         mesa_logi("sgx:  a frame's buffers made %.1f, mapped %.1f, waited for %.1f; submits %.1f",
                   (double)sgx_ioctls[SGX_IOCTL_CREATE] / ctx->stat_frames,
                   (double)sgx_ioctls[SGX_IOCTL_MMAP] / ctx->stat_frames,
                   (double)sgx_ioctls[SGX_IOCTL_WAIT] / ctx->stat_frames,
                   (double)sgx_ioctls[SGX_IOCTL_SUBMIT] / ctx->stat_frames);
         memset(sgx_ioctls, 0, sizeof(sgx_ioctls));
         mesa_logi("sgx:  a frame's draws: %.1f merged into the last, %.1f rendered with the "
                   "last one's state", (double)sgx_stat_merged / ctx->stat_frames,
                   (double)sgx_stat_same / ctx->stat_frames);
         sgx_stat_same = sgx_stat_verts = sgx_stat_tex_changes = sgx_stat_merged = 0;
         if (ctx->debug_sync)
            mesa_logi("sgx:  %.1f ms a render on the GPU", ctx->stat_gpu / 1e6 / ctx->stat_renders);
         ctx->stat_gpu = 0;
         mesa_logi("sgx:  %.1f ms a frame between its end and the next one's first draw "
                   "(%.1f ms of it CPU)", ctx->stat_between / 1e6 / ctx->stat_frames,
                   ctx->stat_between_cpu / 1e6 / ctx->stat_frames);
         ctx->stat_between = ctx->stat_between_cpu = 0;
         sgx_batch_why(ctx);
         ctx->stat_frames = ctx->stat_renders = ctx->stat_draws = 0;
         ctx->stat_t0 = now;
         ctx->stat_cpu0 = cpu;
      }
   }
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
   struct pipe_resource *zs = ctx->fb.zsbuf.texture;
   unsigned x = scissor ? scissor->minx : 0, y = scissor ? scissor->miny : 0;
   unsigned w = scissor ? scissor->maxx - scissor->minx : ctx->fb.width;
   unsigned h = scissor ? scissor->maxy - scissor->miny : ctx->fb.height;
   bool cd = (buffers & PIPE_CLEAR_DEPTH) && zs;
   bool cs = (buffers & PIPE_CLEAR_STENCIL) && zs &&
             util_format_has_stencil(util_format_description(zs->format));

   /* depth and stencil first (the colour's quad would end a render's
    * start): at a render's start, all of the target, the depth the render
    * starts at and its stencil 0 for free; else a quad in the render (M24) */
   if ((cd || cs) && !ctx->batch.ndraws && !scissor) {
      if (cd) {
         ctx->batch.depth_clear = depth;
         ctx->batch.zs_cleared = true;
      }
      if (cs && (stencil & stencil_clear_mask & 0xff))
         sgx_zs_clear(ctx, false, 0, true, stencil, stencil_clear_mask, NULL);
   } else if (cd || cs) {
      sgx_zs_clear(ctx, cd, depth, cs, stencil, stencil_clear_mask, scissor);
   }
   for (unsigned i = 0; i < ctx->fb.nr_cbufs; i++) {
      struct pipe_surface *surf = &ctx->fb.cbufs[i];

      if (!(buffers & (PIPE_CLEAR_COLOR0 << i)) || !surf->texture)
         continue;
      /* four channel bits per draw buffer: all of them, a quad in the
       * render (M25) */
      if (i == 0 && ((color_clear_mask >> (4 * i)) & 0xf) == 0xf &&
          sgx_colour_clear(ctx, color, scissor))
         continue;
      /* else the render ends (what was drawn comes first) */
      sgx_batch_flush(ctx);
      if (!scissor && ((color_clear_mask >> (4 * i)) & 0xf) == 0xf &&
          sgx_clear_gpu(ctx, surf, color))
         continue;
      util_clear_render_target(pctx, surf, color, x, y, w, h);
   }
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

/* A blit the copy cannot do -- another format, a scale, a flip -- by the
 * CPU, a texel at a time through util_format's floats, nearest (glCopyTex
 * Image's from a B8G8R8A8 target into an RGB texture, say: M28) */
static bool
cpu_blit(struct pipe_context *pctx, const struct pipe_blit_info *info)
{
   const struct pipe_box *sb = &info->src.box, *db = &info->dst.box;
   struct pipe_transfer *st, *dt;
   struct pipe_box sbox = *sb, dbox = *db;
   const uint8_t *src;
   uint8_t *dst;
   float *in, *out;
   int sw = abs(sb->width), sh = abs(sb->height), dw = abs(db->width), dh = abs(db->height);

   if (!info->mask || (info->mask & ~PIPE_MASK_RGBA))
      return false;
   if (util_format_is_depth_or_stencil(info->src.format) ||
       util_format_is_depth_or_stencil(info->dst.format) ||
       util_format_is_compressed(info->dst.format) || info->scissor_enable ||
       info->alpha_blend || sb->depth != 1 || db->depth != 1 || !sw || !sh || !dw || !dh)
      return false;
   /* the boxes as maps take them: x, y their least corner */
   if (sb->width < 0) {
      sbox.x += sb->width;
      sbox.width = -sb->width;
   }
   if (sb->height < 0) {
      sbox.y += sb->height;
      sbox.height = -sb->height;
   }
   if (db->width < 0) {
      dbox.x += db->width;
      dbox.width = -db->width;
   }
   if (db->height < 0) {
      dbox.y += db->height;
      dbox.height = -db->height;
   }
   src = pctx->texture_map(pctx, info->src.resource, info->src.level, PIPE_MAP_READ, &sbox, &st);
   if (!src)
      return false;
   dst = pctx->texture_map(pctx, info->dst.resource, info->dst.level, PIPE_MAP_WRITE, &dbox,
                           &dt);
   if (!dst) {
      pctx->texture_unmap(pctx, st);
      return false;
   }
   in = MALLOC(4 * sw * sizeof(float));
   out = MALLOC(4 * dw * sizeof(float));
   for (int y = 0; in && out && y < dh; y++) {
      /* the destination's row y (counted the way its box goes) from the
       * source's nearest */
      int dy = db->height < 0 ? dh - 1 - y : y;
      int sy = (int)(((float)y + 0.5f) * sh / dh);
      uint8_t *drow = dst + dy * dt->stride;

      sy = sb->height < 0 ? sh - 1 - sy : sy;
      util_format_unpack_rgba(info->src.format, in, src + sy * st->stride, sw);
      if (info->mask != PIPE_MASK_RGBA)
         util_format_unpack_rgba(info->dst.format, out, drow, dw);
      for (int x = 0; x < dw; x++) {
         int dx = db->width < 0 ? dw - 1 - x : x;
         int sx = (int)(((float)x + 0.5f) * sw / dw);

         sx = sb->width < 0 ? sw - 1 - sx : sx;
         for (unsigned c = 0; c < 4; c++)
            if (info->mask & (PIPE_MASK_R << c))
               out[4 * dx + c] = in[4 * sx + c];
      }
      util_format_pack_rgba(info->dst.format, drow, out, dw);
   }
   FREE(in);
   FREE(out);
   pctx->texture_unmap(pctx, dt);
   pctx->texture_unmap(pctx, st);
   return true;
}

static void
sgx_blit(struct pipe_context *pctx, const struct pipe_blit_info *info)
{
   if (util_try_blit_via_copy_region(pctx, info, false) || cpu_blit(pctx, info))
      return;
   mesa_logw_once("sgx: a blit of depth or stencil, or scissored or blended: not done yet");
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
   struct pipe_rasterizer_state *r = sgx_create_copy(s, sizeof(*s));

   /* lines of width 1 too through the draw module's wide line stage (made
    * triangles: all the TA takes from us), which takes width 1 for a line
    * to leave alone (M24) */
   if (r && r->line_width == 1.0f)
      r->line_width = 1.0f + 1.0f / 1024;
   /* polygon offset's unit: 24-bit depth's (sgx_draw_init) is a bit or two
    * of the tiles' F32 depth, lost between the draw module's z and the
    * ISP's interpolated one; four of them are not (dEQP's displacement
    * cases pass from two) */
   if (r)
      r->offset_units *= 4;
   return r;
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
   for (unsigned i = 0; i < ve->count; i++)
      ve->attr[i] = sgx_attr_of_format(e[i].src_format);
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
   if (sh->nir->info.stage == MESA_SHADER_VERTEX)
      sh->vs_nattrs = sgx_vs_attr_count(sh->nir);
   if (sh->nir->info.stage == MESA_SHADER_VERTEX && ctx->draw) {
      struct pipe_shader_state copy = *s;

      copy.ir.nir = nir_shader_clone(NULL, sh->nir);
      sh->draw = draw_create_vertex_shader(ctx->draw, &copy);
      if (!sh->draw)
         mesa_logw("sgx: the draw module did not take a vertex shader");
   } else if (sh->nir->info.stage == MESA_SHADER_FRAGMENT) {
      char why[128];

      /* the draw module's wide point stage reads it (only scans it) */
      if (ctx->draw)
         sh->draw_fs = draw_create_fragment_shader(ctx->draw, s);

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
   if (ctx->fs == sh) {
      ctx->fs = NULL;
      if (ctx->draw)
         draw_bind_fragment_shader(ctx->draw, NULL);
   }
   if (sh->draw_fs)
      draw_delete_fragment_shader(ctx->draw, sh->draw_fs);
   /* its programs live until the gathered draws, which may use them, are
    * rendered; then their places in GPU memory are free once the render is
    * done (st deletes the last program at the next glUseProgram: no flush
    * in the middle of a frame, M24) */
   sgx_batch_bury_fs(ctx, sh->compiled);
   for (unsigned i = 0; i < sh->nvariants; i++)
      sgx_batch_bury_fs(ctx, sh->variant[i]);
   for (unsigned i = 0; i < sh->nvs; i++)
      sgx_batch_bury_vs(ctx, sh->vs_variant[i]);
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
   struct sgx_context *ctx = sgx_context(pctx);
   struct sgx_shader *sh = state;

   ctx->fs = sh;
   if (ctx->draw)
      draw_bind_fragment_shader(ctx->draw, sh ? sh->draw_fs : NULL);
}

static void
sgx_bind_rs_state(struct pipe_context *pctx, void *state)
{
   struct sgx_context *ctx = sgx_context(pctx);

   /* the draw module first: its flush binds the state it had again (its
    * wide point and line stages restore it), and that must not be the
    * one the GPU's vertex side then goes by */
   if (ctx->draw && state)
      draw_set_rasterizer_state(ctx->draw, state, state);
   ctx->rast = state;
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
   if (start == 0 && count)
      sgx_context(pctx)->scissor = s[0];
}

static void
sgx_set_stencil_ref(struct pipe_context *pctx, const struct pipe_stencil_ref ref)
{
   sgx_context(pctx)->stencil_ref = ref;
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
   p->create_fence_fd = sgx_create_fence_fd;
   p->fence_server_sync = sgx_fence_server_sync;
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
   ctx->debug_fps = getenv("SGX_DEBUG") && strstr(getenv("SGX_DEBUG"), "fps");
   ctx->debug_sync = getenv("SGX_DEBUG") && strstr(getenv("SGX_DEBUG"), "sync");
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
