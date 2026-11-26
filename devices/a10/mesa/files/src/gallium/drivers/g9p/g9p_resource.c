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
   pipe_resource_reference(&t->resource, NULL);
   slab_free(&g9p_context(pctx)->transfer_pool, t);
}

static void
g9p_transfer_flush_region(struct pipe_context *pctx, struct pipe_transfer *t, const struct pipe_box *box)
{
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
