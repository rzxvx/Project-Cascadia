/* SPDX-License-Identifier: MIT */
/*
 * Resources: one CPU-mapped buffer object each, in the GPU's general heap.
 * The CPU maps them in place; a map first sends and waits for the render
 * the context has gathered (renders are waited for, so nothing else is in
 * flight).
 */
#include "g9p_resource.h"

#include "pipe/p_context.h"
#include "pipe/p_screen.h"
#include "util/format/u_format.h"
#include "util/u_math.h"
#include "util/u_inlines.h"
#include "util/u_memory.h"
#include "util/u_transfer.h"

#include "g9p_context.h"
#include "g9p_screen.h"

static struct pipe_resource *
g9p_resource_create(struct pipe_screen *pscreen, const struct pipe_resource *templ)
{
   struct g9p_resource *res = CALLOC_STRUCT(g9p_resource);
   uint64_t size = 0;

   if (!res)
      return NULL;
   res->base = *templ;
   res->base.screen = pscreen;
   pipe_reference_init(&res->base.reference, 1);
   if (templ->target == PIPE_BUFFER) {
      size = templ->width0;
   } else {
      for (unsigned l = 0; l <= templ->last_level && l < G9P_MAX_LEVELS; l++) {
         unsigned w = u_minify(templ->width0, l), h = u_minify(templ->height0, l);

         res->stride[l] = align(util_format_get_stride(templ->format, w), 64);
         res->offset[l] = align64(size, 64);
         size = res->offset[l] + (uint64_t)res->stride[l] * util_format_get_nblocksy(templ->format, h) *
                                 MAX2(templ->array_size, 1);
      }
   }
   if (!(res->bo = g9p_bo_create(&g9p_screen(pscreen)->dev, size))) {
      FREE(res);
      return NULL;
   }
   return &res->base;
}

static void
g9p_resource_destroy(struct pipe_screen *pscreen, struct pipe_resource *prsc)
{
   struct g9p_resource *res = g9p_resource(prsc);

   g9p_bo_destroy(&g9p_screen(pscreen)->dev, res->tw);
   g9p_bo_destroy(&g9p_screen(pscreen)->dev, res->bo);
   FREE(res);
}

static void *
g9p_transfer_map(struct pipe_context *pctx, struct pipe_resource *prsc, unsigned level,
                 unsigned usage, const struct pipe_box *box, struct pipe_transfer **out)
{
   struct g9p_context *ctx = g9p_context(pctx);
   struct g9p_resource *res = g9p_resource(prsc);
   struct pipe_transfer *t;

   if (!(usage & PIPE_MAP_UNSYNCHRONIZED))
      g9p_flush_frame(ctx);
   if (!(t = slab_zalloc(&ctx->transfer_pool)))
      return NULL;
   pipe_resource_reference(&t->resource, prsc);
   t->level = level;
   t->usage = usage;
   t->box = *box;
   *out = t;
   if (prsc->target == PIPE_BUFFER)
      return res->bo->map + box->x;
   t->stride = res->stride[level];
   t->layer_stride = res->stride[level] * util_format_get_nblocksy(prsc->format, u_minify(prsc->height0, level));
   return res->bo->map + res->offset[level] + box->z * t->layer_stride +
          (box->y / util_format_get_blockheight(prsc->format)) * t->stride +
          (box->x / util_format_get_blockwidth(prsc->format)) * util_format_get_blocksize(prsc->format);
}

static void
g9p_transfer_unmap(struct pipe_context *pctx, struct pipe_transfer *t)
{
   if (t->usage & PIPE_MAP_WRITE)
      g9p_resource(t->resource)->seq++;
   pipe_resource_reference(&t->resource, NULL);
   slab_free(&g9p_context(pctx)->transfer_pool, t);
}

static void
g9p_transfer_flush_region(struct pipe_context *pctx, struct pipe_transfer *t, const struct pipe_box *box)
{
}

/* the texel's place in a twiddled texture of 2^lw x 2^lh: x and y bits
 * interleaved, y's first, up to the smaller side; the larger side's
 * other bits above */
static unsigned
twiddle(unsigned x, unsigned y, unsigned lw, unsigned lh)
{
   const unsigned m = MIN2(lw, lh);
   unsigned i = 0;

   for (unsigned b = 0; b < m; b++)
      i |= ((y >> b) & 1) << (2 * b) | ((x >> b) & 1) << (2 * b + 1);
   if (lw > lh)
      i |= (x >> m) << (2 * m);
   else
      i |= (y >> m) << (2 * m);
   return i;
}

struct g9p_bo *
g9p_resource_twiddled(struct g9p_device *dev, struct g9p_resource *res)
{
   const struct pipe_resource *p = &res->base;
   const unsigned w = p->width0, h = p->height0;
   const unsigned lw = util_logbase2_ceil(w), lh = util_logbase2_ceil(h);

   if (util_format_get_blocksize(p->format) != 4)
      return NULL;
   if (!res->tw) {
      if (!(res->tw = g9p_bo_create(dev, (uint64_t)4 << (lw + lh))))
         return NULL;
      res->tw_seq = res->seq - 1;
   }
   if (res->tw_seq == res->seq)
      return res->tw;
   /* RGBA8 in memory order R, G, B, A: BGRA swapped, X made opaque */
   const bool bgr = p->format == PIPE_FORMAT_B8G8R8A8_UNORM || p->format == PIPE_FORMAT_B8G8R8X8_UNORM;
   const bool x = p->format == PIPE_FORMAT_B8G8R8X8_UNORM || p->format == PIPE_FORMAT_R8G8B8X8_UNORM;
   uint32_t *dst = (uint32_t *)res->tw->map;
   for (unsigned yy = 0; yy < h; yy++) {
      const uint32_t *src = (const uint32_t *)(res->bo->map + res->offset[0] + yy * res->stride[0]);
      for (unsigned xx = 0; xx < w; xx++) {
         uint32_t v = src[xx];
         if (bgr)
            v = (v & 0xff00ff00) | (v & 0xff) << 16 | (v >> 16 & 0xff);
         if (x)
            v |= 0xff000000;
         dst[twiddle(xx, yy, lw, lh)] = v;
      }
   }
   res->tw_seq = res->seq;
   return res->tw;
}

void
g9p_resource_screen_init(struct pipe_screen *pscreen)
{
   pscreen->resource_create = g9p_resource_create;
   pscreen->resource_destroy = g9p_resource_destroy;
}

void
g9p_resource_context_init(struct pipe_context *pctx)
{
   pctx->buffer_map = g9p_transfer_map;
   pctx->texture_map = g9p_transfer_map;
   pctx->buffer_unmap = g9p_transfer_unmap;
   pctx->texture_unmap = g9p_transfer_unmap;
   pctx->transfer_flush_region = g9p_transfer_flush_region;
   pctx->buffer_subdata = u_default_buffer_subdata;
   pctx->texture_subdata = u_default_texture_subdata;
}
