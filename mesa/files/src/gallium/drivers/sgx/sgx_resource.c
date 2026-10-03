/*
 * Copyright 2026 Project Cascadia
 * SPDX-License-Identifier: MIT
 *
 * Resources: one buffer object each, linear, mapped write-combined on the
 * CPU side; maps wait for the renders that use the buffer.
 */
#include "sgx_resource.h"

#include <fcntl.h>
#include <xf86drm.h>

#include "drm-uapi/drm_fourcc.h"
#include "frontend/winsys_handle.h"
#include "util/format/u_format.h"
#include "util/u_inlines.h"
#include "util/u_math.h"
#include "util/u_memory.h"

#include "sgx_context.h"
#include "sgx_device.h"
#include "sgx_screen.h"

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

   res->bo = sgx_bo_create(&screen->dev, align(MAX2(size, 1), 4096), 0, 0);
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
