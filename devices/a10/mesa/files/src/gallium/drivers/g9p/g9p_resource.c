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
#include "util/log.h"
#include "util/os_time.h"
#include "util/u_memory.h"
#include "util/u_transfer.h"

#include "frontend/sw_winsys.h"

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
      /* depth / stencil: whole 32 x 32 tiles, the ISP's load and store
       * moves tiles */
      const bool zs = util_format_is_depth_or_stencil(templ->format);
      for (unsigned l = 0; l <= templ->last_level && l < G9P_MAX_LEVELS; l++) {
         unsigned w = u_minify(templ->width0, l), h = u_minify(templ->height0, l);

         if (zs) {
            w = align(w, 32);
            h = align(h, 32);
         }

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
   struct sw_winsys *ws = g9p_screen(pscreen)->sw;
   if (ws && (templ->bind & (PIPE_BIND_DISPLAY_TARGET | PIPE_BIND_SCANOUT | PIPE_BIND_SHARED)))
      res->dt = ws->displaytarget_create(ws, templ->bind, templ->format, templ->width0, templ->height0, 64, NULL,
                                         &res->dt_stride);
   return &res->base;
}

static void
g9p_resource_destroy(struct pipe_screen *pscreen, struct pipe_resource *prsc)
{
   struct g9p_resource *res = g9p_resource(prsc);

   if (res->dt)
      g9p_screen(pscreen)->sw->displaytarget_destroy(g9p_screen(pscreen)->sw, res->dt);
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

   if (!(usage & PIPE_MAP_UNSYNCHRONIZED) && g9p_frame_uses(ctx, prsc))
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
g9p_resource_twiddled(struct g9p_device *dev, struct g9p_resource *res, unsigned *nlevels)
{
   const struct pipe_resource *p = &res->base;
   const unsigned lw = util_logbase2_ceil(p->width0), lh = util_logbase2_ceil(p->height0);
   const unsigned levels = MIN2(p->last_level + 1, G9P_MAX_LEVELS);
   uint64_t size = 0;

   if (util_format_get_blocksize(p->format) != 4)
      return NULL;
   /* the levels one after the other, each a twiddled power-of-two
    * rectangle: level 0's sides rounded up, halved (a level's own sides
    * rounded up put a 100x60 chain's last level in the wrong place); a
    * cube's faces / an array's layers one chain each, 64-byte aligned */
   for (unsigned l = 0; l < levels; l++)
      size += (uint64_t)4 << (MAX2((int)lw - (int)l, 0) + MAX2((int)lh - (int)l, 0));
   const unsigned layers = MAX2(p->array_size, 1);
   const uint64_t lstride = align64(size, 64);    /* (4 or 16384: only the first face right) */
   size = lstride * layers;
   res->tw_layer_stride = lstride;
   *nlevels = levels;
   if (!res->tw) {
      if (!(res->tw = g9p_bo_create(dev, size)))
         return NULL;
      res->tw_seq = res->seq - 1;
   }
   if (res->tw_seq == res->seq)
      return res->tw;
   /* RGBA8 in memory order R, G, B, A: BGRA swapped, X made opaque */
   const bool bgr = p->format == PIPE_FORMAT_B8G8R8A8_UNORM || p->format == PIPE_FORMAT_B8G8R8X8_UNORM;
   const bool x = p->format == PIPE_FORMAT_B8G8R8X8_UNORM || p->format == PIPE_FORMAT_R8G8B8X8_UNORM;
   for (unsigned z = 0; z < layers; z++) {
   uint32_t *dst = (uint32_t *)(res->tw->map + z * lstride);
   for (unsigned l = 0; l < levels; l++) {
      const unsigned w = u_minify(p->width0, l), h = u_minify(p->height0, l);
      const unsigned llw = MAX2((int)lw - (int)l, 0), llh = MAX2((int)lh - (int)l, 0);
      const uint8_t *layer = res->bo->map + res->offset[l] + (size_t)z * res->stride[l] * h;

      /* the padding filled from the edges: the sampler reads past a
       * non-power-of-two level's last texel (black stripes at a 100x60
       * chain's small levels while it was left empty) */
      for (unsigned yy = 0; yy < 1u << llh; yy++) {
         const uint32_t *src = (const uint32_t *)(layer + MIN2(yy, h - 1) * res->stride[l]);
         for (unsigned xx = 0; xx < 1u << llw; xx++) {
            uint32_t v = src[MIN2(xx, w - 1)];
            if (bgr)
               v = (v & 0xff00ff00) | (v & 0xff) << 16 | (v >> 16 & 0xff);
            if (x)
               v |= 0xff000000;
            dst[twiddle(xx, yy, llw, llh)] = v;
         }
      }
      dst += 1u << (llw + llh);
   }
   }
   res->tw_seq = res->seq;
   return res->tw;
}

/* show a display target: the render's pixels into the winsys's buffer */
static void
g9p_flush_frontbuffer(struct pipe_screen *pscreen, struct pipe_context *pctx, struct pipe_resource *prsc,
                      unsigned level, unsigned layer, void *context_private, unsigned nboxes,
                      struct pipe_box *sub_box)
{
   struct sw_winsys *ws = g9p_screen(pscreen)->sw;
   struct g9p_resource *res = g9p_resource(prsc);
   uint8_t *map;

   if (!ws || !res->dt)
      return;
   if (pctx)
      g9p_flush_frame(g9p_context(pctx));
   if (!(map = ws->displaytarget_map(ws, res->dt, PIPE_MAP_WRITE)))
      return;
   const unsigned row = util_format_get_stride(prsc->format, prsc->width0);
   for (unsigned y = 0; y < prsc->height0; y++)
      memcpy(map + y * res->dt_stride, res->bo->map + res->offset[0] + y * res->stride[0], row);
   ws->displaytarget_unmap(ws, res->dt);
   ws->displaytarget_display(ws, res->dt, context_private, nboxes, sub_box);

   /* G9P_FPS=1: the frames shown, each second */
   static int fps = -1;
   static unsigned frames;
   static int64_t since;
   if (fps < 0)
      fps = getenv("G9P_FPS") && atoi(getenv("G9P_FPS"));
   if (fps) {
      const int64_t now = os_time_get_nano();
      if (!since)
         since = now;
      frames++;
      if (now - since >= 1000000000) {
         mesa_logi("g9p: %.1f fps (%ux%u)", frames * 1e9 / (now - since), prsc->width0, prsc->height0);
         frames = 0;
         since = now;
      }
   }
}

void
g9p_resource_screen_init(struct pipe_screen *pscreen)
{
   pscreen->flush_frontbuffer = g9p_flush_frontbuffer;
   pscreen->resource_create = g9p_resource_create;
   pscreen->resource_destroy = g9p_resource_destroy;
}

/* the levels below base_level by the CPU: each texel the mean of its 2x2
 * (rows and columns past an odd side clamped); 4-byte formats of 8-bit
 * channels */
static bool
g9p_generate_mipmap(struct pipe_context *pctx, struct pipe_resource *prsc, enum pipe_format format,
                    unsigned base_level, unsigned last_level, unsigned first_layer, unsigned last_layer)
{
   struct g9p_resource *res = g9p_resource(prsc);
   const struct util_format_description *d = util_format_description(prsc->format);

   if ((prsc->target != PIPE_TEXTURE_2D && prsc->target != PIPE_TEXTURE_CUBE &&
        prsc->target != PIPE_TEXTURE_2D_ARRAY) || util_format_get_blocksize(prsc->format) != 4 ||
       d->layout != UTIL_FORMAT_LAYOUT_PLAIN || d->channel[0].size != 8 || last_level > prsc->last_level)
      return false;
   if (g9p_frame_uses(g9p_context(pctx), prsc))
      g9p_flush_frame(g9p_context(pctx));
   /* (a cube's faces / an array's layers each on its own) */
   for (unsigned z = first_layer; z <= last_layer; z++)
   for (unsigned l = base_level + 1; l <= last_level; l++) {
      const unsigned sw = u_minify(prsc->width0, l - 1), sh = u_minify(prsc->height0, l - 1);
      const unsigned w = u_minify(prsc->width0, l), h = u_minify(prsc->height0, l);
      const uint8_t *src = res->bo->map + res->offset[l - 1] + (size_t)z * res->stride[l - 1] * sh;
      uint8_t *dst = res->bo->map + res->offset[l] + (size_t)z * res->stride[l] * h;

      for (unsigned y = 0; y < h; y++) {
         const uint8_t *r0 = src + MIN2(2 * y, sh - 1) * res->stride[l - 1];
         const uint8_t *r1 = src + MIN2(2 * y + 1, sh - 1) * res->stride[l - 1];
         for (unsigned x = 0; x < w; x++) {
            const unsigned x0 = 4 * MIN2(2 * x, sw - 1), x1 = 4 * MIN2(2 * x + 1, sw - 1);
            for (unsigned c = 0; c < 4; c++)
               dst[y * res->stride[l] + 4 * x + c] = (r0[x0 + c] + r0[x1 + c] + r1[x0 + c] + r1[x1 + c] + 2) / 4;
         }
      }
   }
   res->seq++;
   return true;
}

void
g9p_resource_context_init(struct pipe_context *pctx)
{
   pctx->generate_mipmap = g9p_generate_mipmap;
   pctx->buffer_map = g9p_transfer_map;
   pctx->texture_map = g9p_transfer_map;
   pctx->buffer_unmap = g9p_transfer_unmap;
   pctx->texture_unmap = g9p_transfer_unmap;
   pctx->transfer_flush_region = g9p_transfer_flush_region;
   pctx->buffer_subdata = u_default_buffer_subdata;
   pctx->texture_subdata = u_default_texture_subdata;
}
