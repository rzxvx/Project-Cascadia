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

#include <string.h>
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
   /* (SGX_DEBUG=fps,flushes: every flush a frame -- the X server's glamor,
    * which never swaps) */
   if (ctx->debug_fps && ((flags & PIPE_FLUSH_END_OF_FRAME) || ctx->debug_flushes)) {
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

         mesa_logi("sgx: %.1f fps at %ux%u (viewport %.0fx%.0f), %.1f renders and %.1f draws a frame, %.0f%% CPU, "
                   "%.0f%% waiting for the GPU", ctx->stat_frames / s, ctx->fb.width, ctx->fb.height,
                   fabsf(ctx->viewport.scale[0]) * 2, fabsf(ctx->viewport.scale[1]) * 2,
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

/* ---- occlusion queries (M37) ---------------------------------------------
 *
 * iOS's m29: ISP state A bit 14 makes the ISP count the samples a draw
 * passes, into the visibility buffer the 3D block's first two words name
 * (the kext's from GL's payload words 42 and 43), a word for each core.  No
 * bit picks one of several counts (bits 15..23 tried: the object's type,
 * the depth test), so a render goes at each begin and end, and each render
 * a query was active in has a buffer of its own: the query's result is
 * theirs added up. */
static void
query_fold(struct sgx_query *q, unsigned i)
{
   struct sgx_bo *bo = q->bo[i];
   const uint32_t *w = (const uint32_t *)bo->map;

   sgx_bo_wait(bo, -1);
   q->samples += w[0] + w[SGX_VIS_CORE_STRIDE / 4];
   sgx_bo_destroy(bo);
}

void
sgx_query_add(struct sgx_query *q, struct sgx_bo *bo)
{
   /* (a query over many renders: the oldest read now) */
   if (q->nbo == SGX_QUERY_BUFS) {
      query_fold(q, 0);
      memmove(q->bo, q->bo + 1, --q->nbo * sizeof(q->bo[0]));
   }
   q->bo[q->nbo++] = bo;
}

static void
query_release(struct sgx_query *q)
{
   for (unsigned i = 0; i < q->nbo; i++)
      sgx_bo_destroy(q->bo[i]);
   q->nbo = 0;
   sgx_fence_reference(&q->last, NULL);
}

static struct pipe_query *
sgx_create_query(struct pipe_context *pctx, unsigned type, unsigned index)
{
   struct sgx_query *q;

   if (type != PIPE_QUERY_OCCLUSION_COUNTER && type != PIPE_QUERY_OCCLUSION_PREDICATE &&
       type != PIPE_QUERY_OCCLUSION_PREDICATE_CONSERVATIVE && type != PIPE_QUERY_TIMESTAMP &&
       type != PIPE_QUERY_TIME_ELAPSED)
      return NULL;
   if (!(q = CALLOC_STRUCT(sgx_query)))
      return NULL;
   q->type = type;
   return (struct pipe_query *)q;
}

static void
sgx_destroy_query(struct pipe_context *pctx, struct pipe_query *pq)
{
   struct sgx_context *ctx = sgx_context(pctx);
   struct sgx_query *q = (struct sgx_query *)pq;

   if (ctx->query == q) {
      sgx_batch_flush(ctx);
      ctx->query = NULL;
   }
   query_release(q);
   sgx_fence_reference(&q->begin_fence, NULL);
   FREE(q);
}

static bool
timer_query(const struct sgx_query *q)
{
   return q->type == PIPE_QUERY_TIMESTAMP || q->type == PIPE_QUERY_TIME_ELAPSED;
}

/* the GPU's time at a point of the stream: when the render before it was
 * done, or when the point was reached, if later (an idle GPU) */
static uint64_t
gpu_time(struct sgx_fence *f, uint64_t cpu)
{
   return MAX2(f ? sgx_fence_signal_time(f) : 0, cpu);
}

static bool
sgx_begin_query(struct pipe_context *pctx, struct pipe_query *pq)
{
   struct sgx_context *ctx = sgx_context(pctx);
   struct sgx_query *q = (struct sgx_query *)pq;

   sgx_batch_flush(ctx);
   if (timer_query(q)) {
      sgx_fence_reference(&q->begin_fence, ctx->last);
      q->begin_cpu = os_time_get_nano();
      return true;
   }
   query_release(q);
   q->samples = 0;
   ctx->query = q;
   return true;
}

static bool
sgx_end_query(struct pipe_context *pctx, struct pipe_query *pq)
{
   struct sgx_context *ctx = sgx_context(pctx);

   sgx_batch_flush(ctx);
   if (ctx->query == (struct sgx_query *)pq)
      ctx->query = NULL;
   sgx_fence_reference(&((struct sgx_query *)pq)->last, ctx->last);
   ((struct sgx_query *)pq)->end_cpu = os_time_get_nano();
   return true;
}

static bool
sgx_get_query_result(struct pipe_context *pctx, struct pipe_query *pq, bool wait,
                     union pipe_query_result *result)
{
   struct sgx_query *q = (struct sgx_query *)pq;

   if (q->last && !sgx_fence_wait(q->last, wait ? OS_TIMEOUT_INFINITE : 0))
      return false;
   if (timer_query(q)) {
      uint64_t end = gpu_time(q->last, q->end_cpu);

      if (q->type == PIPE_QUERY_TIMESTAMP)
         result->u64 = end;
      else
         result->u64 = end - MIN2(end, gpu_time(q->begin_fence, q->begin_cpu));
      return true;
   }
   for (unsigned i = 0; i < q->nbo; i++)
      if (!wait && !sgx_bo_idle(q->bo[i]))
         return false;
   for (unsigned i = 0; i < q->nbo; i++)
      query_fold(q, i);
   q->nbo = 0;
   if (q->type == PIPE_QUERY_OCCLUSION_COUNTER)
      result->u64 = q->samples;
   else
      result->b = q->samples != 0;
   return true;
}

static void
sgx_set_active_query_state(struct pipe_context *pctx, bool enable)
{
   sgx_context(pctx)->queries_off = !enable;
}

/* ---- conditional rendering (M43: NV_conditional_render) ----------------- */

static void
sgx_render_condition(struct pipe_context *pctx, struct pipe_query *query, bool condition,
                     enum pipe_render_cond_flag mode)
{
   struct sgx_context *ctx = sgx_context(pctx);

   ctx->cond_query = query;
   ctx->cond_condition = condition;
   ctx->cond_mode = mode;
}

/* Rendering goes ahead when the query's result (samples passed, or any)
 * differs from the condition -- gallium's rule; a result not there yet, a
 * mode that does not wait: ahead too, as GL allows */
bool
sgx_render_condition_ok(struct sgx_context *ctx)
{
   union pipe_query_result r;
   bool wait, value;

   if (!ctx->cond_query)
      return true;
   wait = ctx->cond_mode == PIPE_RENDER_COND_WAIT ||
          ctx->cond_mode == PIPE_RENDER_COND_BY_REGION_WAIT;
   if (!sgx_get_query_result(&ctx->base, ctx->cond_query, wait, &r))
      return true;
   value = ((struct sgx_query *)ctx->cond_query)->type == PIPE_QUERY_OCCLUSION_COUNTER ?
           r.u64 != 0 : r.b;
   return value != ctx->cond_condition;
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

   const struct sgx_frame_target t = { rt, 0, 0 };

   /* (the frame's own clear packs RGBA8: half floats' targets clear with
    * a quad, M43) */
   if (!screen->frame || surf->level || surf->first_layer ||
       sgx_frame_rt_kind(surf->format) != SGX_RT_UNORM8 || util_format_is_srgb(surf->format) ||
       !sgx_frame_can_render(screen->frame, &t))
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
   /* (a framebuffer smaller than its depth buffer, M43: only the area they
    * share is cleared -- a quad, not the render's start for all of it) */
   bool zs_whole = zs && ctx->fb.width >= u_minify(zs->width0, ctx->fb.zsbuf.level) &&
                   ctx->fb.height >= u_minify(zs->height0, ctx->fb.zsbuf.level);

   /* (M43) under a render condition that says no: nothing */
   if (!sgx_render_condition_ok(ctx))
      return;
   /* depth and stencil first (the colour's quad would end a render's
    * start): at a render's start, all of the target, the depth the render
    * starts at and its stencil 0 for free; else a quad in the render (M24) */
   if ((cd || cs) && !ctx->batch.ndraws && !scissor && zs_whole) {
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
          ctx->fb.width >= u_minify(surf->texture->width0, surf->level) &&
          ctx->fb.height >= u_minify(surf->texture->height0, surf->level) &&
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
   if (render_condition_enabled && !sgx_render_condition_ok(sgx_context(pctx)))
      return;
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
   if (render_condition_enabled && !sgx_render_condition_ok(sgx_context(pctx)))
      return;
   sgx_batch_flush(sgx_context(pctx));
   util_clear_depth_stencil(pctx, dst, flags, depth, stencil, x, y, w, h);
}

/* ---- copies and blits, by the CPU --------------------------------------- */

/* A blit the copy cannot do -- another format, a scale, a flip -- by the
 * CPU, a texel at a time through util_format's floats (glCopyTexImage's
 * from a B8G8R8A8 target into an RGB texture, say: M28).  As
 * glBlitFramebuffer wants it (M43): each destination pixel's centre taken
 * back into the source, nearest or between the four texels around it
 * (clamped to the source's edges), none written whose centre falls outside
 * the source or the scissor; depth and stencil nearest; blended with the
 * source's alpha when asked.  Colour, or depth and stencil, at a time. */
static bool
cpu_blit(struct pipe_context *pctx, const struct pipe_blit_info *info)
{
   const struct pipe_box *sb = &info->src.box, *db = &info->dst.box;
   struct pipe_resource *sres = info->src.resource, *dres = info->dst.resource;
   bool zs = info->mask & PIPE_MASK_ZS;
   bool linear = !zs && info->filter == PIPE_TEX_FILTER_LINEAR;
   int sw = u_minify(sres->width0, info->src.level), sh = u_minify(sres->height0, info->src.level);
   int dw = u_minify(dres->width0, info->dst.level), dh = u_minify(dres->height0, info->dst.level);
   int x0 = MAX2(MIN2(db->x, db->x + db->width), 0), x1 = MIN2(MAX2(db->x, db->x + db->width), dw);
   int y0 = MAX2(MIN2(db->y, db->y + db->height), 0), y1 = MIN2(MAX2(db->y, db->y + db->height), dh);
   struct pipe_transfer *st, *dt;
   struct pipe_box sbox, dbox;
   const uint8_t *src;
   uint8_t *dst;
   float *in0, *in1, *out;
   uint8_t *s8, *d8;
   bool ok;

   if (!info->mask || (zs && (info->mask & PIPE_MASK_RGBA)) ||
       (zs && (!util_format_is_depth_or_stencil(info->src.format) ||
               !util_format_is_depth_or_stencil(info->dst.format))) ||
       (!zs && (util_format_is_depth_or_stencil(info->src.format) ||
                util_format_is_depth_or_stencil(info->dst.format))))
      return false;
   if (util_format_is_compressed(info->dst.format) || util_format_is_compressed(info->src.format) ||
       sb->depth != 1 || db->depth != 1 || !sb->width || !sb->height || !db->width ||
       !db->height)
      return false;
   if (info->scissor_enable) {
      x0 = MAX2(x0, (int)info->scissor.minx);
      x1 = MIN2(x1, (int)info->scissor.maxx);
      y0 = MAX2(y0, (int)info->scissor.miny);
      y1 = MIN2(y1, (int)info->scissor.maxy);
   }
   if (x0 >= x1 || y0 >= y1)
      return true;

   /* the source's whole level (a box may reach past it), the destination's
    * pixels to write, read too (what is not written stays) */
   u_box_2d_zslice(0, 0, sb->z, sw, sh, &sbox);
   u_box_2d_zslice(x0, y0, db->z, x1 - x0, y1 - y0, &dbox);
   src = pctx->texture_map(pctx, sres, info->src.level, PIPE_MAP_READ, &sbox, &st);
   if (!src)
      return false;
   dst = pctx->texture_map(pctx, dres, info->dst.level, PIPE_MAP_READ_WRITE, &dbox, &dt);
   if (!dst) {
      pctx->texture_unmap(pctx, st);
      return false;
   }
   in0 = MALLOC(4 * sw * sizeof(float));
   in1 = MALLOC(4 * sw * sizeof(float));
   out = MALLOC(4 * (x1 - x0) * sizeof(float));
   s8 = MALLOC(sw);
   d8 = MALLOC(x1 - x0);
   ok = in0 && in1 && out && s8 && d8;
   for (int y = y0; ok && y < y1; y++) {
      /* the destination row's centre in the source, and its rows */
      float fy = sb->y + (y + 0.5f - db->y) * sb->height / (float)db->height;
      uint8_t *drow = dst + (y - y0) * dt->stride;
      int r0, r1;
      float wy = 0;

      if (fy < 0 || fy >= sh)
         continue;
      if (linear) {
         r0 = (int)floorf(fy - 0.5f);
         wy = fy - 0.5f - r0;
         r1 = CLAMP(r0 + 1, 0, sh - 1);
         r0 = CLAMP(r0, 0, sh - 1);
      } else {
         r0 = r1 = (int)fy;
      }
      if (zs) {
         if (info->mask & PIPE_MASK_Z) {
            util_format_unpack_z_float(info->src.format, in0, src + r0 * st->stride, sw);
            util_format_unpack_z_float(info->dst.format, out, drow, x1 - x0);
         }
         if (info->mask & PIPE_MASK_S) {
            util_format_unpack_s_8uint(info->src.format, s8, src + r0 * st->stride, sw);
            util_format_unpack_s_8uint(info->dst.format, d8, drow, x1 - x0);
         }
      } else {
         util_format_unpack_rgba(info->src.format, in0, src + r0 * st->stride, sw);
         if (linear)
            util_format_unpack_rgba(info->src.format, in1, src + r1 * st->stride, sw);
         util_format_unpack_rgba(info->dst.format, out, drow, x1 - x0);
      }
      for (int x = x0; x < x1; x++) {
         float fx = sb->x + (x + 0.5f - db->x) * sb->width / (float)db->width;
         float *o = out + 4 * (x - x0), texel[4];
         int c0, c1;

         if (fx < 0 || fx >= sw)
            continue;
         if (zs) {
            c0 = (int)fx;
            if (info->mask & PIPE_MASK_Z)
               out[x - x0] = in0[c0];
            if (info->mask & PIPE_MASK_S)
               d8[x - x0] = s8[c0];
            continue;
         }
         if (linear) {
            float wx;

            c0 = (int)floorf(fx - 0.5f);
            wx = fx - 0.5f - c0;
            c1 = CLAMP(c0 + 1, 0, sw - 1);
            c0 = CLAMP(c0, 0, sw - 1);
            for (unsigned c = 0; c < 4; c++) {
               float t = in0[4 * c0 + c] + (in0[4 * c1 + c] - in0[4 * c0 + c]) * wx;
               float b = in1[4 * c0 + c] + (in1[4 * c1 + c] - in1[4 * c0 + c]) * wx;

               texel[c] = t + (b - t) * wy;
            }
         } else {
            memcpy(texel, in0 + 4 * (int)fx, sizeof(texel));
         }
         for (unsigned c = 0; c < 4; c++) {
            if (!(info->mask & (PIPE_MASK_R << c)))
               continue;
            o[c] = info->alpha_blend ? texel[c] * texel[3] + o[c] * (1 - texel[3]) : texel[c];
         }
      }
      if (zs) {
         if ((info->mask & PIPE_MASK_Z) &&
             util_format_has_depth(util_format_description(info->dst.format)))
            util_format_pack_z_float(info->dst.format, drow, out, x1 - x0);
         if ((info->mask & PIPE_MASK_S) &&
             util_format_has_stencil(util_format_description(info->dst.format)))
            util_format_pack_s_8uint(info->dst.format, drow, d8, x1 - x0);
      } else {
         util_format_pack_rgba(info->dst.format, drow, out, x1 - x0);
      }
   }
   FREE(in0);
   FREE(in1);
   FREE(out);
   FREE(s8);
   FREE(d8);
   pctx->texture_unmap(pctx, dt);
   pctx->texture_unmap(pctx, st);
   return ok;
}

static void
sgx_blit(struct pipe_context *pctx, const struct pipe_blit_info *info)
{
   struct pipe_blit_info part = *info;

   if (info->render_condition_enable && !sgx_render_condition_ok(sgx_context(pctx)))
      return;
   if (util_try_blit_via_copy_region(pctx, info, false))
      return;
   /* colour, then depth and stencil (M43: glBlitFramebuffer's mask may
    * have both) */
   part.mask = info->mask & PIPE_MASK_RGBA;
   if (part.mask && !cpu_blit(pctx, &part))
      mesa_logw_once("sgx: a colour blit not done (format %s to %s)",
                     util_format_short_name(info->src.format),
                     util_format_short_name(info->dst.format));
   part.mask = info->mask & PIPE_MASK_ZS;
   if (part.mask && !cpu_blit(pctx, &part))
      mesa_logw_once("sgx: a depth or stencil blit not done (format %s to %s)",
                     util_format_short_name(info->src.format),
                     util_format_short_name(info->dst.format));
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
    * cases pass from two; SGX_OFFSET_SCALE) */
   if (r)
      r->offset_units *= SGX_OFFSET_SCALE;
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
   /* (M43) depth clamping as the program's state has it: the draw
    * module's wide line and point stages bind one of their own for their
    * triangles, with depth clipping and clamping both off */
   if (state) {
      const struct pipe_rasterizer_state *r = state;

      if (r->depth_clip_near || r->depth_clip_far || r->depth_clamp)
         ctx->depth_clamp = r->depth_clamp;
   }
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
   else if (index < SGX_FRAME_MAX_UBOS &&
            (shader == MESA_SHADER_VERTEX || shader == MESA_SHADER_FRAGMENT))
      util_copy_constant_buffer(&ctx->ubo[shader == MESA_SHADER_FRAGMENT][index], cb);
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
   for (unsigned i = 0; i < 2; i++)
      for (unsigned k = 0; k < SGX_FRAME_MAX_UBOS; k++)
         pipe_resource_reference(&ctx->ubo[i][k].buffer, NULL);
   if (pctx->stream_uploader)
      u_upload_destroy(pctx->stream_uploader);
   sgx_fence_reference(&ctx->last, NULL);
   pipe_resource_reference(&ctx->no_colour, NULL);
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
   ctx->debug_flushes = getenv("SGX_DEBUG") && strstr(getenv("SGX_DEBUG"), "flushes");
   ctx->debug_sync = getenv("SGX_DEBUG") && strstr(getenv("SGX_DEBUG"), "sync");
   p->texture_barrier = sgx_texture_barrier;
   p->memory_barrier = sgx_memory_barrier;
   p->get_device_reset_status = sgx_get_device_reset_status;
   p->create_query = sgx_create_query;
   p->destroy_query = sgx_destroy_query;
   p->begin_query = sgx_begin_query;
   p->end_query = sgx_end_query;
   p->get_query_result = sgx_get_query_result;
   p->render_condition = sgx_render_condition;
   p->set_active_query_state = sgx_set_active_query_state;
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
