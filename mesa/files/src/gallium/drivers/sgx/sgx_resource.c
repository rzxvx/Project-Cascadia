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
   sgx_bo_destroy(res->bo);
   FREE(res);
}

static bool
sgx_resource_get_handle(struct pipe_screen *pscreen, struct pipe_context *pctx,
                        struct pipe_resource *prsc, struct winsys_handle *handle,
                        unsigned usage)
{
   struct sgx_resource *res = sgx_resource(prsc);
   int fd;

   handle->stride = res->stride[0];
   handle->offset = 0;
   handle->modifier = DRM_FORMAT_MOD_LINEAR;
   switch (handle->type) {
   case WINSYS_HANDLE_TYPE_KMS:
      handle->handle = res->bo->handle;
      return true;
   case WINSYS_HANDLE_TYPE_FD:
      if (drmPrimeHandleToFD(res->bo->dev->fd, res->bo->handle, DRM_CLOEXEC | DRM_RDWR, &fd))
         return false;
      handle->handle = fd;
      return true;
   default:
      return false;
   }
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

/* the copy made again from the linear content (level 0) */
static bool
twiddle_texture(struct sgx_screen *screen, struct sgx_resource *res)
{
   struct pipe_resource *p = &res->base;
   unsigned w = p->width0, h = p->height0, tw = util_next_power_of_two(w);
   unsigned th = util_next_power_of_two(h);
   uint32_t *dst, *row;
   const uint8_t *src;

   if (!res->tw || res->tw_w != tw || res->tw_h != th) {
      sgx_bo_destroy(res->tw);
      res->tw = sgx_bo_create(&screen->dev, tw * th * 4, 0, 0);
      if (!res->tw || !sgx_bo_map(res->tw)) {
         sgx_bo_destroy(res->tw);
         res->tw = NULL;
         return false;
      }
      res->tw_w = tw;
      res->tw_h = th;
   } else {
      /* a render may still be sampling the old copy */
      sgx_frame_finish(screen->frame);
   }
   if (!(src = sgx_bo_map(res->bo)) || !(row = MALLOC(w * 4)))
      return false;
   /* what the GPU wrote into the texture, done first */
   sgx_bo_wait(res->bo, -1);
   dst = (uint32_t *)res->tw->map;
   /* padding: the last column and row repeated, so a clamped lookup at the
    * edge finds the edge */
   for (unsigned y = 0; y < th; y++) {
      unsigned sy = MIN2(y, h - 1);

      util_format_unpack_rgba_8unorm_rect(p->format, (uint8_t *)row, w * 4,
                                          src + res->offset[0] + sy * res->stride[0],
                                          res->stride[0], w, 1);
      for (unsigned x = 0; x < tw; x++)
         dst[twiddle(x, y, tw, th)] = row[MIN2(x, w - 1)];
   }
   FREE(row);
   res->tw_seq = res->seq;
   return true;
}

/* The sampler's half of the state's word 0.  Wrap: iOS's for CLAMP_TO_EDGE
 * on both axes is 0x90 (the corpus's t* cases), REPEAT is what sgx2d draws
 * with (0).  Filters, found by flipping bits under gltex: bits 13:12 the
 * magnification filter, 11:10 the minification one, 0 point and 1 (or 2)
 * bilinear -- 3 samples as point again. */
static uint32_t
sampler_bits(const struct pipe_sampler_state *ss)
{
   if (!ss)
      return 0;
   return (ss->wrap_s == PIPE_TEX_WRAP_REPEAT ? 0 : 1u << 4) |
          (ss->wrap_t == PIPE_TEX_WRAP_REPEAT ? 0 : 1u << 7) |
          (ss->min_img_filter == PIPE_TEX_FILTER_LINEAR ? 1u << 10 : 0) |
          (ss->mag_img_filter == PIPE_TEX_FILTER_LINEAR ? 1u << 12 : 0);
}

bool
sgx_resource_texture(struct sgx_screen *screen, struct sgx_resource *res,
                     const struct pipe_sampler_state *ss, uint32_t words[4])
{
   struct pipe_resource *p = &res->base;

   if ((p->target != PIPE_TEXTURE_2D && p->target != PIPE_TEXTURE_RECT) ||
       p->array_size != 1 || !p->width0 || !p->height0)
      return false;
   if ((!res->tw || res->tw_seq != res->seq) && !twiddle_texture(screen, res))
      return false;
   if ((p->width0 & (p->width0 - 1)) || (p->height0 & (p->height0 - 1)))
      mesa_logw_once("sgx: textures that are not a power of two in size are sampled "
                     "from a padded copy; their coordinates are not scaled yet");
   words[0] = 0x03fe0000 | sampler_bits(ss);
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
   words[1] = 0x0c000000 | util_logbase2(res->tw_w) << 16 | util_logbase2(res->tw_h);
   words[2] = res->tw->va;
   words[3] = 0;
   return true;
}

void
sgx_resource_screen_init(struct sgx_screen *screen)
{
   screen->base.resource_create = sgx_resource_create;
   screen->base.resource_destroy = sgx_resource_destroy;
   screen->base.resource_get_handle = sgx_resource_get_handle;
}

static void *
sgx_transfer_map(struct pipe_context *pctx, struct pipe_resource *prsc, unsigned level,
                 unsigned usage, const struct pipe_box *box, struct pipe_transfer **out)
{
   struct sgx_context *ctx = sgx_context(pctx);
   struct sgx_resource *res = sgx_resource(prsc);
   struct sgx_transfer *trans;
   uint8_t *map;

   /* draws gathered but not rendered that write or read it go first */
   if (!(usage & PIPE_MAP_UNSYNCHRONIZED) && sgx_batch_uses(ctx, prsc))
      sgx_batch_flush(ctx);
   if (!(usage & PIPE_MAP_UNSYNCHRONIZED) && !sgx_bo_wait(res->bo, -1))
      return NULL;
   if (!(map = sgx_bo_map(res->bo)))
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

static void
sgx_transfer_unmap(struct pipe_context *pctx, struct pipe_transfer *ptrans)
{
   struct sgx_context *ctx = sgx_context(pctx);

   if (ptrans->usage & PIPE_MAP_WRITE)
      sgx_resource(ptrans->resource)->seq++;

   pipe_resource_reference(&ptrans->resource, NULL);
   slab_free(&ctx->transfer_pool, ptrans);
}

void
sgx_resource_context_init(struct pipe_context *pctx)
{
   pctx->buffer_map = sgx_transfer_map;
   pctx->texture_map = sgx_transfer_map;
   pctx->buffer_unmap = sgx_transfer_unmap;
   pctx->texture_unmap = sgx_transfer_unmap;
   pctx->transfer_flush_region = u_default_transfer_flush_region;
   pctx->buffer_subdata = u_default_buffer_subdata;
   pctx->texture_subdata = u_default_texture_subdata;
}
