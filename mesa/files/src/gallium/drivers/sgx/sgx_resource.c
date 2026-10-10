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
      simple_mtx_init(&res->dirty_lock, mtx_plain);
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
   if (res->data)
      simple_mtx_destroy(&res->dirty_lock);
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

/* The copy the sampler reads, made again from the linear content: every
 * level of every face (a cube map's six, in GL's order), one after another,
 * each RGBA8 in Morton order at its own size padded to powers of two --
 * iOS's layout (a 64x64's levels at +0, +0x4000, +0x5000, ...,
 * p105-gpu.md's mipmap transfers); a cube map's faces each as far apart as
 * a whole chain down to 1 x 1, whatever levels it has, rounded up to 2 KiB
 * from 16 x 16 up (glmip --probe: 4 x 4 faces 21 texels apart, 8 x 8 85,
 * 16 x 16 512, 32 x 32 1536, 64 x 64 5632; Vita3K's texture cache has the
 * same rules for the same GPU).  A 2D texture's size not a power of two is
 * padded, the last column and row repeated: the sampler takes the size as
 * it is (sgx_resource_texture), the padding is its memory's layout.  A cube
 * map's is scaled up to one, the nearest texel (GXM's arbitrary cube map,
 * type 7, read past the copy here and hung the renders): a direction cannot
 * be scaled to a part of a face. */
static bool
copy_texture(struct sgx_screen *screen, struct sgx_resource *res)
{
   struct pipe_resource *p = &res->base;
   unsigned w = p->width0, h = p->height0;
   unsigned tw = util_next_power_of_two(w), th = util_next_power_of_two(h);
   unsigned faces = p->target == PIPE_TEXTURE_CUBE ? 6 : 1;
   unsigned levels = p->last_level + 1;
   bool scale = faces > 1 && (tw != w || th != h);
   uint32_t size = 0, face = 0, *dst, *row;
   const uint8_t *map;

   for (unsigned l = 0; l < levels; l++)
      size += MAX2(tw >> l, 1) * MAX2(th >> l, 1) * 4;
   for (unsigned l = 0; faces > 1 && (tw >> l || th >> l); l++)
      face += MAX2(tw >> l, 1) * MAX2(th >> l, 1) * 4;
   if (faces > 1 && tw >= 16 && th >= 16)
      face = align(face, 2048);
   if (faces > 1)
      size = 5 * face + size;
   if (!res->tw || res->tw_w != tw || res->tw_h != th || res->tw_levels != levels) {
      sgx_bo_destroy(res->tw);
      res->tw = sgx_bo_create(&screen->dev, size, 0, 0);
      if (!res->tw || !sgx_bo_map(res->tw)) {
         sgx_bo_destroy(res->tw);
         res->tw = NULL;
         return false;
      }
      res->tw_w = tw;
      res->tw_h = th;
      res->tw_levels = levels;
   } else {
      /* a render may still be sampling the old copy */
      sgx_frame_finish(screen->frame);
   }
   if (!(map = sgx_bo_map(res->bo)) || !(row = MALLOC(MAX2(w, 1) * 4)))
      return false;
   /* what the GPU wrote into the texture, done first */
   sgx_bo_wait(res->bo, -1);
   for (unsigned f = 0; f < faces; f++) {
      dst = (uint32_t *)(res->tw->map + f * face);
      for (unsigned l = 0; l < levels; l++) {
         unsigned lw = u_minify(w, l), lh = u_minify(h, l);
         unsigned dw = MAX2(tw >> l, 1), dh = MAX2(th >> l, 1);
         const uint8_t *src = map + res->offset[l] + f * res->layer_size[l];

         for (unsigned y = 0; y < dh; y++) {
            unsigned sy = scale ? (2 * y + 1) * lh / (2 * dh) : MIN2(y, lh - 1);

            unpack_row(p->format, src + sy * res->stride[l], lw, row);
            /* padding: the last column and row repeated (a bilinear lookup
             * at the edge reads it) */
            for (unsigned x = 0; x < dw; x++)
               dst[twiddle(x, y, dw, dh)] =
                  row[scale ? (2 * x + 1) * lw / (2 * dw) : MIN2(x, lw - 1)];
         }
         dst += dw * dh;
      }
   }
   FREE(row);
   /* SGX_TEX_PROBE=1: a cube map's copy holds each texel's own index
    * instead (finding the layout: glmip --probe) */
   if (faces > 1 && getenv("SGX_TEX_PROBE"))
      for (uint32_t i = 0; i < res->tw->size / 4; i++)
         ((uint32_t *)res->tw->map)[i] = 0xff000000u | i;
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
 * bilinear -- 3 samples as point again; bit 9 the linear mip filter, a
 * level and the next mixed (glmip, M28: the nearest level without it).
 * Whether there is mipmapping at all is the last level's field
 * (sgx_resource_texture). */
static uint32_t
sampler_bits(const struct pipe_sampler_state *ss)
{
   if (!ss)
      return 0;
   return wrap_bits(ss->wrap_t) << 3 | wrap_bits(ss->wrap_s) << 6 |
          (ss->min_mip_filter == PIPE_TEX_MIPFILTER_LINEAR ? 1u << 9 : 0) |
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
   /* (a size not a power of two is twiddled too, M28: the minification
    * filter is lost here) */
   return force == 1 || res->external || res->gpu_written;
}

bool
sgx_resource_texture(struct sgx_screen *screen, struct sgx_resource *res,
                     const struct pipe_sampler_state *ss, uint32_t words[4])
{
   bool swap, x8;

   struct pipe_resource *p = &res->base;

   if ((p->target != PIPE_TEXTURE_2D && p->target != PIPE_TEXTURE_RECT &&
        p->target != PIPE_TEXTURE_CUBE) ||
       p->array_size != (p->target == PIPE_TEXTURE_CUBE ? 6 : 1) || !p->width0 || !p->height0)
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
      bool cube = p->target == PIPE_TEXTURE_CUBE;

      if ((!res->tw || res->tw_seq != res->seq) && !copy_texture(screen, res))
         return false;
      /* bits 20:17 the last level the sampler goes down to (iOS's 4 x 4
       * cube map, one level, has 0; glmip): with no mip filter, level 0's
       * alone; bits 26:21 the LOD bias, 31 none (Vita3K's SceGxmTexture,
       * the same words) */
      words[0] = 0x03e00000 |
                 (ss && ss->min_mip_filter != PIPE_TEX_MIPFILTER_NONE ?
                  (res->tw_levels - 1) << 17 : 0) |
                 sampler_bits(ss);
      /* word 1: the type in bits 31:29 (0 twiddled, 2 a cube map -- iOS's
       * t04, 0x4c020002 -- 5 twiddled of any size, 6 strided: GXM's
       * SceGxmTextureType), the format in 28:24, the size: log2 w in 19:16
       * and log2 h in 3:0, or w - 1 in 23:12 and h - 1 in 11:0 */
      if (cube || (util_is_power_of_two_nonzero(p->width0) &&
                   util_is_power_of_two_nonzero(p->height0)))
         words[1] = (cube ? 2u << 29 : 0) | 0x0c000000 |
                    util_logbase2(res->tw_w) << 16 | util_logbase2(res->tw_h);
      else
         words[1] = 5u << 29 | 0x0c000000 | (p->width0 - 1) << 12 | (p->height0 - 1);
      words[2] = res->tw->va;
      words[3] = 0;
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
   /* (a map glthread makes in the application's thread: nothing of the
    * context's -- M34) */
   trans = usage & PIPE_MAP_THREAD_SAFE ? CALLOC_STRUCT(sgx_transfer) :
                                          slab_zalloc(&ctx->transfer_pool);
   if (!trans)
      return NULL;
   if (prsc->target == PIPE_BUFFER && (usage & (PIPE_MAP_THREAD_SAFE | PIPE_MAP_PERSISTENT)))
      p_atomic_inc(&res->live_maps);
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
dirty_add(struct sgx_resource *res, uint32_t lo, uint32_t hi)
{
   if (res->dirty_lo >= res->dirty_hi) {
      res->dirty_lo = lo;
      res->dirty_hi = hi;
   } else {
      res->dirty_lo = MIN2(res->dirty_lo, lo);
      res->dirty_hi = MAX2(res->dirty_hi, hi);
   }
}

static void
buffer_written(struct sgx_resource *res, uint32_t lo, uint32_t hi, unsigned usage)
{
   if (lo >= hi)
      return;
   simple_mtx_lock(&res->dirty_lock);
   dirty_add(res, lo, hi);
   res->dirty_sync |= !(usage & PIPE_MAP_UNSYNCHRONIZED);
   simple_mtx_unlock(&res->dirty_lock);
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
   if (ptrans->resource->target == PIPE_BUFFER &&
       (ptrans->usage & (PIPE_MAP_THREAD_SAFE | PIPE_MAP_PERSISTENT)))
      p_atomic_dec(&res->live_maps);

   pipe_resource_reference(&ptrans->resource, NULL);
   if (ptrans->usage & PIPE_MAP_THREAD_SAFE)
      FREE(ptrans);
   else
      slab_free(&ctx->transfer_pool, ptrans);
}

struct sgx_bo *
sgx_buffer_bo(struct sgx_context *ctx, struct sgx_resource *res, uint32_t read_lo,
              uint32_t read_hi)
{
   struct sgx_screen *screen = sgx_screen(ctx->base.screen);
   struct sgx_bo *bo;
   uint32_t lo, hi;

   simple_mtx_lock(&res->dirty_lock);
   /* mapped while it is drawn from: what the draw reads, as it is (glthread
    * writes its uploads on and on, before the draws that read them) */
   if (p_atomic_read(&res->live_maps) > 0 && read_lo < MIN2(read_hi, res->base.width0))
      dirty_add(res, read_lo, MIN2(read_hi, res->base.width0));
   bo = res->bo;
   lo = res->dirty_lo;
   hi = res->dirty_hi;
   if (bo && lo >= hi) {
      simple_mtx_unlock(&res->dirty_lock);
      return bo;
   }
   if (bo && res->dirty_sync && (sgx_batch_reads(ctx, bo) || !sgx_bo_idle(bo))) {
      /* a render reads the old content: the gathered draws or the kernel
       * keep the old copy for it */
      sgx_bo_destroy(bo);
      res->bo = bo = NULL;
   }
   if (!bo) {
      /* (four bytes more: a fetch reads whole words, three bytes of RGB8
       * at the end too) */
      if (!(bo = sgx_bo_cache_get(&screen->dev, res->base.width0 + 4))) {
         simple_mtx_unlock(&res->dirty_lock);
         return NULL;
      }
      res->bo = bo;
      lo = 0;
      hi = res->base.width0;
   }
   memcpy(bo->map + lo, res->data + lo, hi - lo);
   res->dirty_lo = res->dirty_hi = 0;
   res->dirty_sync = false;
   simple_mtx_unlock(&res->dirty_lock);
   return bo;
}

/* glGenerateMipmap by the CPU (M28): each level a 2 x 2 box filter of the
 * one above (one texel of it where the level above has only one across or
 * down), through util_format's floats -- the blit util_gen_mipmap would
 * use scales, which the driver does not */
static bool
sgx_generate_mipmap(struct pipe_context *pctx, struct pipe_resource *prsc,
                    enum pipe_format format, unsigned base_level, unsigned last_level,
                    unsigned first_layer, unsigned last_layer)
{
   struct sgx_context *ctx = sgx_context(pctx);
   struct sgx_resource *res = sgx_resource(prsc);
   unsigned w = prsc->width0;
   float *buf;
   uint8_t *map;

   if (prsc->target == PIPE_BUFFER || prsc->target == PIPE_TEXTURE_3D || !res->bo ||
       util_format_is_compressed(format) || util_format_is_depth_or_stencil(format) ||
       util_format_is_pure_integer(format))
      return false;
   /* draws that write or read it go first, and renders that do */
   if (sgx_batch_uses(ctx, prsc))
      sgx_batch_flush(ctx);
   if (!sgx_bo_wait(res->bo, -1) || !(map = sgx_bo_map(res->bo)) ||
       !(buf = MALLOC(3 * 4 * MAX2(w, 1) * sizeof(float))))
      return false;
   for (unsigned layer = first_layer; layer <= last_layer; layer++) {
      for (unsigned l = base_level + 1; l <= last_level; l++) {
         unsigned sw = u_minify(prsc->width0, l - 1), sh = u_minify(prsc->height0, l - 1);
         unsigned dw = u_minify(prsc->width0, l), dh = u_minify(prsc->height0, l);
         const uint8_t *src = map + res->offset[l - 1] + layer * res->layer_size[l - 1];
         uint8_t *dst = map + res->offset[l] + layer * res->layer_size[l];
         float *r0 = buf, *r1 = buf + 4 * w, *out = buf + 8 * w;

         for (unsigned y = 0; y < dh; y++) {
            unsigned y0 = MIN2(2 * y, sh - 1), y1 = MIN2(2 * y + 1, sh - 1);

            util_format_unpack_rgba(format, r0, src + y0 * res->stride[l - 1], sw);
            util_format_unpack_rgba(format, r1, src + y1 * res->stride[l - 1], sw);
            for (unsigned x = 0; x < dw; x++) {
               unsigned x0 = MIN2(2 * x, sw - 1), x1 = MIN2(2 * x + 1, sw - 1);

               for (unsigned c = 0; c < 4; c++)
                  out[4 * x + c] = (r0[4 * x0 + c] + r0[4 * x1 + c] + r1[4 * x0 + c] +
                                    r1[4 * x1 + c]) / 4;
            }
            util_format_pack_rgba(format, dst + y * res->stride[l], out, dw);
         }
      }
   }
   FREE(buf);
   res->seq++;
   return true;
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
   pctx->generate_mipmap = sgx_generate_mipmap;
   pctx->texture_subdata = u_default_texture_subdata;
}
