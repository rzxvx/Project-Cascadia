/*
 * Copyright 2026 Project Cascadia
 * SPDX-License-Identifier: MIT
 *
 * Resources: one buffer object each, linear, mapped write-combined on the
 * CPU side; maps wait for the renders that use the buffer.
 */
#include "sgx_resource.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <xf86drm.h>

#include "drm-uapi/apple_sgx_drm.h"
#include "drm-uapi/drm_fourcc.h"
#include "frontend/winsys_handle.h"
#include "util/format/u_format.h"
#include "util/log.h"
#include "util/u_inlines.h"
#include "util/u_math.h"
#include "util/u_memory.h"

#include "sgx_context.h"
#include "sgx_device.h"
#include "sgx_draw.h"
#include "sgx_frame.h"
#include "sgx_screen.h"

/* SGX_FRAME=align: a render target at a 1 MiB-aligned address of our own
 * choosing (0xc0000000 up), to tell whether the pixel back end or the
 * background object want more than a page's alignment */
static struct sgx_bo *
aligned_bo(struct sgx_device *dev, uint32_t size)
{
   for (uint32_t va = 0xc0000000u; va + size <= 0xe0000000u; va += 1u << 20) {
      struct sgx_bo *bo = sgx_bo_create(dev, size, APPLE_SGX_BO_FIXED_VA, va);

      if (bo || errno != EEXIST)
         return bo;
   }
   return NULL;
}

static struct pipe_resource *
sgx_resource_create(struct pipe_screen *pscreen, const struct pipe_resource *templ)
{
   struct sgx_screen *screen = sgx_screen(pscreen);
   struct sgx_resource *res = CALLOC_STRUCT(sgx_resource);
   uint32_t size = 0;

   if (!res)
      return NULL;
   res->base = *templ;
   res->base.screen = pscreen;
   pipe_reference_init(&res->base.reference, 1);

   if (templ->target == PIPE_BUFFER) {
      size = templ->width0;
   } else {
      for (unsigned l = 0; l <= templ->last_level; l++) {
         unsigned w = u_minify(templ->width0, l), h = u_minify(templ->height0, l);
         unsigned layers = templ->target == PIPE_TEXTURE_3D ?
                           u_minify(templ->depth0, l) : templ->array_size;

         res->stride[l] = align(util_format_get_stride(templ->format, w), 64);
         res->layer_size[l] = res->stride[l] * util_format_get_nblocksy(templ->format, h);
         res->offset[l] = align(size, 64);
         size = res->offset[l] + res->layer_size[l] * layers;
      }
   }

   /* a buffer: CPU memory, which maps read and write without waiting
    * (M25); the GPU's copy is made when a draw reads it (M26:
    * sgx_buffer_bo) */
   if (templ->target == PIPE_BUFFER) {
      if (!(res->data = MALLOC(MAX2(size, 1)))) {
         FREE(res);
         return NULL;
      }
      return &res->base;
   }
   size = align(MAX2(size, 1), 4096);
   if ((templ->bind & PIPE_BIND_RENDER_TARGET) && templ->target != PIPE_BUFFER &&
       (sgx_frame_options() & SGX_FRAME_ALIGN))
      res->bo = aligned_bo(&screen->dev, size);
   if (!res->bo)
      res->bo = sgx_bo_create(&screen->dev, size, 0, 0);
   if (!res->bo) {
      FREE(res);
      return NULL;
   }
   return &res->base;
}

static void
sgx_resource_destroy(struct pipe_screen *pscreen, struct pipe_resource *prsc)
{
   struct sgx_resource *res = sgx_resource(prsc);

   /* a render still using it holds the kernel's reference */
   sgx_bo_destroy(res->tw);
   sgx_bo_destroy(res->zls);
   sgx_bo_destroy(res->bo);
   FREE(res->data);
   FREE(res);
}

static bool
sgx_resource_get_handle(struct pipe_screen *pscreen, struct pipe_context *pctx,
                        struct pipe_resource *prsc, struct winsys_handle *handle,
                        unsigned usage)
{
   struct sgx_resource *res = sgx_resource(prsc);
   int fd;

   if (prsc->target == PIPE_BUFFER || !res->bo)
      return false;
   handle->stride = res->stride[0];
   handle->offset = 0;
   handle->modifier = DRM_FORMAT_MOD_LINEAR;
   switch (handle->type) {
   case WINSYS_HANDLE_TYPE_KMS:
      handle->handle = res->bo->handle;
      res->external = true;
      return true;
   case WINSYS_HANDLE_TYPE_FD:
      if (drmPrimeHandleToFD(res->bo->dev->fd, res->bo->handle, DRM_CLOEXEC | DRM_RDWR, &fd))
         return false;
      handle->handle = fd;
      res->external = true;
      return true;
   default:
      return false;
   }
}

/* A dma-buf (or a handle on this device) as a texture or a render target:
 * linear, its stride and offset as given. */
static struct pipe_resource *
sgx_resource_from_handle(struct pipe_screen *pscreen, const struct pipe_resource *templ,
                         struct winsys_handle *whandle, unsigned usage)
{
   struct sgx_screen *screen = sgx_screen(pscreen);
   struct sgx_device *dev = &screen->dev;
   struct sgx_resource *res;
   uint32_t handle;

   if ((templ->target != PIPE_TEXTURE_2D && templ->target != PIPE_TEXTURE_RECT) ||
       templ->last_level || templ->array_size > 1 || whandle->plane ||
       (whandle->modifier != DRM_FORMAT_MOD_INVALID &&
        whandle->modifier != DRM_FORMAT_MOD_LINEAR))
      return NULL;
   switch (whandle->type) {
   case WINSYS_HANDLE_TYPE_FD:
      if (drmPrimeFDToHandle(dev->fd, whandle->handle, &handle))
         return NULL;
      break;
   case WINSYS_HANDLE_TYPE_KMS:
      handle = whandle->handle;
      break;
   default:
      return NULL;
   }
   if (!(res = CALLOC_STRUCT(sgx_resource)))
      return NULL;
   res->base = *templ;
   res->base.screen = pscreen;
   pipe_reference_init(&res->base.reference, 1);
   res->stride[0] = whandle->stride;
   res->offset[0] = whandle->offset;
   res->layer_size[0] = whandle->stride * util_format_get_nblocksy(templ->format,
                                                                     templ->height0);
   res->external = true;
   if (!(res->bo = sgx_bo_import(dev, handle)) ||
       (uint64_t)res->offset[0] + res->layer_size[0] > res->bo->size) {
      sgx_bo_destroy(res->bo);
      FREE(res);
      return NULL;
   }
   return &res->base;
}

/* Morton order, y in the even bits (the layout iOS's GL driver uploads); a
 * rectangle is a row (or column) of such squares, the side of the shorter
 * edge, one after another (docs/research/p105-gpu.md, M7) */
static uint32_t
twiddle(uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
   uint32_t m = MIN2(w, h), i = 0, xs = x % m, ys = y % m;

   for (unsigned b = 0; (1u << b) < m; b++)
      i |= ((ys >> b) & 1) << (2 * b) | ((xs >> b) & 1) << (2 * b + 1);
   return (w >= h ? x / m : y / m) * m * m + i;
}

/* a row of the texture's content as 8-bit RGBA (depth: grey, alpha 1 --
 * OES_depth_texture's d, d, d, 1) */
static void
unpack_row(enum pipe_format format, const uint8_t *line, unsigned w, uint32_t *row)
{
   if (util_format_is_depth_or_stencil(format)) {
      float z[w];

      util_format_unpack_z_float(format, z, line, w);
      for (unsigned x = 0; x < w; x++)
         row[x] = 0xff000000u | 0x010101u * (uint32_t)(CLAMP(z[x], 0.0f, 1.0f) * 255.0f + 0.5f);
   } else {
      util_format_unpack_rgba_8unorm_rect(format, (uint8_t *)row, w * 4, line, 0, w, 1);
   }
}

/* The copy the sampler reads, made again from the linear content (level 0):
 * twiddled, or (lin) linear BGRA rows -- what the sampler takes for a size
 * not a power of two, which a twiddled copy would pad (and the coordinates
 * are not scaled) */
static bool
copy_texture(struct sgx_screen *screen, struct sgx_resource *res, bool lin)
{
   struct pipe_resource *p = &res->base;
   unsigned w = p->width0, h = p->height0;
   unsigned tw = lin ? align(MAX2(w * 4, 32), 16) / 4 : util_next_power_of_two(w);
   unsigned th = lin ? h : util_next_power_of_two(h);
   uint32_t *dst, *row;
   const uint8_t *src;

   if (!res->tw || res->tw_w != tw || res->tw_h != th || res->tw_lin != lin) {
      sgx_bo_destroy(res->tw);
      res->tw = sgx_bo_create(&screen->dev, tw * th * 4, 0, 0);
      if (!res->tw || !sgx_bo_map(res->tw)) {
         sgx_bo_destroy(res->tw);
         res->tw = NULL;
         return false;
      }
      res->tw_w = tw;
      res->tw_h = th;
      res->tw_lin = lin;
   } else {
      /* a render may still be sampling the old copy */
      sgx_frame_finish(screen->frame);
   }
   if (!(src = sgx_bo_map(res->bo)) || !(row = MALLOC(w * 4)))
      return false;
   /* what the GPU wrote into the texture, done first */
   sgx_bo_wait(res->bo, -1);
   dst = (uint32_t *)res->tw->map;
   for (unsigned y = 0; y < th; y++) {
      unsigned sy = MIN2(y, h - 1);

      unpack_row(p->format, src + res->offset[0] + sy * res->stride[0], w, row);
      if (lin) {
         /* B G R A in memory, as the sampler reads a linear texture */
         for (unsigned x = 0; x < w; x++)
            dst[y * tw + x] = (row[x] & 0xff00ff00u) | (row[x] & 0xff) << 16 |
                              (row[x] >> 16 & 0xff);
         continue;
      }
      /* padding: the last column and row repeated, so a clamped lookup at
       * the edge finds the edge */
      for (unsigned x = 0; x < tw; x++)
         dst[twiddle(x, y, tw, th)] = row[MIN2(x, w - 1)];
   }
   FREE(row);
   res->tw_seq = res->seq;
   return true;
}

/* a wrap mode as the sampler takes it: 0 repeat, 1 mirrored, 2 clamped */
static uint32_t
wrap_bits(unsigned wrap)
{
   return wrap == PIPE_TEX_WRAP_REPEAT ? 0 : wrap == PIPE_TEX_WRAP_MIRROR_REPEAT ? 1 : 2;
}

/* The sampler's half of the state's word 0.  Wrap: t in bits 5:3, s in
 * 8:6 (iOS's for CLAMP_TO_EDGE on both axes is 0x90, the corpus's t* cases;
 * the axes and mirroring found with a texture drawn three times over, M24).
 * Filters, found by flipping bits under gltex: bits 13:12 the
 * magnification filter, 11:10 the minification one, 0 point and 1 (or 2)
 * bilinear -- 3 samples as point again. */
static uint32_t
sampler_bits(const struct pipe_sampler_state *ss)
{
   if (!ss)
      return 0;
   return wrap_bits(ss->wrap_t) << 3 | wrap_bits(ss->wrap_s) << 6 |
          (ss->min_img_filter == PIPE_TEX_FILTER_LINEAR ? 1u << 10 : 0) |
          (ss->mag_img_filter == PIPE_TEX_FILTER_LINEAR ? 1u << 12 : 0);
}

bool
sgx_resource_linear(const struct sgx_resource *res, bool *swap, bool *x8)
{
   const struct pipe_resource *p = &res->base;
   static int force = -1;

   if (force < 0)
      force = getenv("SGX_LINEAR_TEX") ? atoi(getenv("SGX_LINEAR_TEX")) : 2;
   *swap = *x8 = false;
   switch (p->format) {
   case PIPE_FORMAT_R8G8B8X8_UNORM:
      *x8 = true;
      FALLTHROUGH;
   case PIPE_FORMAT_R8G8B8A8_UNORM:
      *swap = true;
      break;
   case PIPE_FORMAT_B8G8R8X8_UNORM:
      *x8 = true;
      FALLTHROUGH;
   case PIPE_FORMAT_B8G8R8A8_UNORM:
      break;
   default:
      return false;
   }
   /* SGX_LINEAR_TEX=0: never, 1: whenever it can */
   if (force == 0 || p->last_level || p->array_size > 1 || (res->stride[0] & 15) ||
       res->stride[0] < 32 || ((res->offset[0] + res->bo->va) & 15) ||
       (p->target != PIPE_TEXTURE_2D && p->target != PIPE_TEXTURE_RECT))
      return false;
   /* and when the twiddled copy would be padded (its coordinates are not
    * scaled): a size not a power of two */
   return force == 1 || res->external || res->gpu_written ||
          !util_is_power_of_two_nonzero(p->width0) || !util_is_power_of_two_nonzero(p->height0);
}

bool
sgx_resource_texture(struct sgx_screen *screen, struct sgx_resource *res,
                     const struct pipe_sampler_state *ss, uint32_t words[4])
{
   bool swap, x8;

   struct pipe_resource *p = &res->base;

   if ((p->target != PIPE_TEXTURE_2D && p->target != PIPE_TEXTURE_RECT) ||
       p->array_size != 1 || !p->width0 || !p->height0)
      return false;
   if (sgx_resource_linear(res, &swap, &x8)) {
      /* the 2D engine's way (apple_sgx_hw.c, blt_tex: iOS's for an
       * IOSurface): the content itself, its stride in 16 bytes less 2;
       * bits 11:9 all set, or the rows come out skewed -- every other value
       * of the three was tried (gltex), and the minification filter they
       * are for twiddled textures is lost: point sampling */
      words[0] = (res->stride[0] / 16 - 2) << 16 | (sampler_bits(ss) & ~0xe00u) | 7u << 9;
      words[1] = 0xcc000000 | (p->width0 - 1) << 12 | (p->height0 - 1);
      words[2] = res->bo->va + res->offset[0];
      words[3] = 0x10000000;
      res->sampled = res->bo;
   } else {
      bool lin = !util_is_power_of_two_nonzero(p->width0) ||
                 !util_is_power_of_two_nonzero(p->height0);

      if ((!res->tw || res->tw_seq != res->seq || res->tw_lin != lin) &&
          !copy_texture(screen, res, lin))
         return false;
      if (lin) {
         /* a linear BGRA copy, sampled as above */
         words[0] = (res->tw_w * 4 / 16 - 2) << 16 | (sampler_bits(ss) & ~0xe00u) | 7u << 9;
         words[1] = 0xcc000000 | (p->width0 - 1) << 12 | (p->height0 - 1);
         words[3] = 0x10000000;
      } else {
         words[0] = 0x03fe0000 | sampler_bits(ss);
         words[1] = 0x0c000000 | util_logbase2(res->tw_w) << 16 | util_logbase2(res->tw_h);
         words[3] = 0;
      }
      words[2] = res->tw->va;
      res->sampled = res->tw;
   }
   /* SGX_TEX_WORDn=mask: bits of word n flipped, to find what they do */
   {
      static int64_t flip[4] = { -1, -1, -1, -1 };

      for (unsigned i = 0; i < 4; i++) {
         char name[16];

         if (flip[i] < 0) {
            snprintf(name, sizeof(name), "SGX_TEX_WORD%u", i);
            flip[i] = getenv(name) ? strtoul(getenv(name), NULL, 0) : 0;
         }
         words[i] ^= (uint32_t)flip[i];
      }
   }
   return true;
}

void
sgx_resource_screen_init(struct sgx_screen *screen)
{
   screen->base.resource_create = sgx_resource_create;
   screen->base.resource_destroy = sgx_resource_destroy;
   screen->base.resource_get_handle = sgx_resource_get_handle;
   screen->base.resource_from_handle = sgx_resource_from_handle;
}

static void *
sgx_transfer_map(struct pipe_context *pctx, struct pipe_resource *prsc, unsigned level,
                 unsigned usage, const struct pipe_box *box, struct pipe_transfer **out)
{
   struct sgx_context *ctx = sgx_context(pctx);
   struct sgx_resource *res = sgx_resource(prsc);
   struct sgx_transfer *trans;
   uint8_t *map;

   /* draws gathered but not rendered that write or read it go first, and
    * renders that do -- not for a buffer: its CPU copy is no render's
    * (M26: sgx_buffer_bo) */
   if (!(usage & PIPE_MAP_UNSYNCHRONIZED) && prsc->target != PIPE_BUFFER) {
      if (sgx_batch_uses(ctx, prsc))
         sgx_batch_flush(ctx);
      if (!sgx_bo_wait(res->bo, -1))
         return NULL;
   }
   if (!(map = res->data ? res->data : sgx_bo_map(res->bo)))
      return NULL;
   trans = slab_zalloc(&ctx->transfer_pool);
   if (!trans)
      return NULL;
   pipe_resource_reference(&trans->base.resource, prsc);
   trans->base.level = level;
   trans->base.usage = usage;
   trans->base.box = *box;
   *out = &trans->base;

   if (prsc->target == PIPE_BUFFER)
      return map + box->x;

   trans->base.stride = res->stride[level];
   trans->base.layer_stride = res->layer_size[level];
   return map + res->offset[level] + box->z * res->layer_size[level] +
          (box->y / util_format_get_blockheight(prsc->format)) * res->stride[level] +
          (box->x / util_format_get_blockwidth(prsc->format)) *
          util_format_get_blocksize(prsc->format);
}

/* bytes [lo, hi) of a buffer written: what its GPU copy is behind by */
static void
buffer_written(struct sgx_resource *res, uint32_t lo, uint32_t hi, unsigned usage)
{
   if (lo >= hi)
      return;
   if (res->dirty_lo >= res->dirty_hi) {
      res->dirty_lo = lo;
      res->dirty_hi = hi;
   } else {
      res->dirty_lo = MIN2(res->dirty_lo, lo);
      res->dirty_hi = MAX2(res->dirty_hi, hi);
   }
   res->dirty_sync |= !(usage & PIPE_MAP_UNSYNCHRONIZED);
}

/* a map with PIPE_MAP_FLUSH_EXPLICIT wrote what it flushes (u_upload_mgr
 * maps the rest of its buffer, and flushes what it handed out) */
static void
sgx_transfer_flush_region(struct pipe_context *pctx, struct pipe_transfer *ptrans,
                          const struct pipe_box *box)
{
   if (ptrans->resource->target == PIPE_BUFFER)
      buffer_written(sgx_resource(ptrans->resource), ptrans->box.x + box->x,
                     ptrans->box.x + box->x + box->width, ptrans->usage);
}

static void
sgx_transfer_unmap(struct pipe_context *pctx, struct pipe_transfer *ptrans)
{
   struct sgx_context *ctx = sgx_context(pctx);
   struct sgx_resource *res = sgx_resource(ptrans->resource);

   if (ptrans->usage & PIPE_MAP_WRITE) {
      res->seq++;
      if (ptrans->resource->target == PIPE_BUFFER && !(ptrans->usage & PIPE_MAP_FLUSH_EXPLICIT))
         buffer_written(res, ptrans->box.x, ptrans->box.x + ptrans->box.width, ptrans->usage);
   }

   pipe_resource_reference(&ptrans->resource, NULL);
   slab_free(&ctx->transfer_pool, ptrans);
}

struct sgx_bo *
sgx_buffer_bo(struct sgx_context *ctx, struct sgx_resource *res)
{
   struct sgx_screen *screen = sgx_screen(ctx->base.screen);
   struct sgx_bo *bo = res->bo;
   uint32_t lo = res->dirty_lo, hi = res->dirty_hi;

   if (bo && lo >= hi)
      return bo;
   if (bo && res->dirty_sync && (sgx_batch_reads(ctx, bo) || !sgx_bo_idle(bo))) {
      /* a render reads the old content: the gathered draws or the kernel
       * keep the old copy for it */
      sgx_bo_destroy(bo);
      res->bo = bo = NULL;
   }
   if (!bo) {
      /* (four bytes more: a fetch reads whole words, three bytes of RGB8
       * at the end too) */
      if (!(bo = sgx_bo_cache_get(&screen->dev, res->base.width0 + 4)))
         return NULL;
      res->bo = bo;
      lo = 0;
      hi = res->base.width0;
   }
   memcpy(bo->map + lo, res->data + lo, hi - lo);
   res->dirty_lo = res->dirty_hi = 0;
   res->dirty_sync = false;
   return bo;
}

void
sgx_resource_context_init(struct pipe_context *pctx)
{
   pctx->buffer_map = sgx_transfer_map;
   pctx->texture_map = sgx_transfer_map;
   pctx->buffer_unmap = sgx_transfer_unmap;
   pctx->texture_unmap = sgx_transfer_unmap;
   pctx->transfer_flush_region = sgx_transfer_flush_region;
   pctx->buffer_subdata = u_default_buffer_subdata;
   pctx->texture_subdata = u_default_texture_subdata;
}
