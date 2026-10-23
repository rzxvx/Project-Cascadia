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
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <xf86drm.h>

#include "drm-uapi/apple_sgx_drm.h"
#include "drm-uapi/drm_fourcc.h"
#include "frontend/winsys_handle.h"
#include "util/format/u_format.h"
#include "util/half_float.h"
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
    * sgx_buffer_bo).  64-byte aligned: GL's MIN_MAP_BUFFER_ALIGNMENT
    * (musl's malloc gives 16; piglit's map_invalidate cases pass or fail
    * with where the heap is) */
   if (templ->target == PIPE_BUFFER) {
      simple_mtx_init(&res->dirty_lock, mtx_plain);
      if (!(res->data = align_malloc(MAX2(size, 1), 64))) {
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
   FREE(res->tw_cpu);
   for (unsigned i = 0; i < res->nzls; i++)
      sgx_bo_destroy(res->zls[i].bo);
   FREE(res->zls);
   sgx_bo_destroy(res->bo);
   if (res->data)
      simple_mtx_destroy(&res->dirty_lock);
   align_free(res->data);
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
 * edge, one after another (docs/research/p105-gpu.md, M7).  By tables
 * (M37): x's bits spread to the odd places, y's to the even ones, for the
 * square's side -- not a loop and two divisions a texel. */
struct twiddle {
   uint32_t *mx, *my;           /* the side's entries each */
   unsigned shift;              /* log2 of the side */
   bool wide;                   /* the squares go along x */
};

static bool
twiddle_init(struct twiddle *t, unsigned w, unsigned h)
{
   unsigned m = MIN2(w, h);

   t->shift = util_logbase2(m);
   t->wide = w >= h;
   if (!(t->mx = MALLOC(2 * m * sizeof(uint32_t))))
      return false;
   t->my = t->mx + m;
   for (unsigned v = 0; v < m; v++) {
      t->mx[v] = t->my[v] = 0;
      for (unsigned b = 0; (1u << b) < m; b++) {
         t->mx[v] |= ((v >> b) & 1) << (2 * b + 1);
         t->my[v] |= ((v >> b) & 1) << (2 * b);
      }
   }
   return true;
}

static inline uint32_t
twiddle_at(const struct twiddle *t, unsigned x, unsigned y)
{
   unsigned mask = (1u << t->shift) - 1;

   return ((t->wide ? x : y) >> t->shift << 2 * t->shift) + t->mx[x & mask] + t->my[y & mask];
}

/* a row of the texture's content as 8-bit RGBA -- depth as F32 (M37: the
 * sampler's F32 format, iOS's m04..m08 read it raw and compare in the
 * program; the program makes it d, d, d, 1) */
static void
unpack_row(enum pipe_format format, const uint8_t *line, unsigned w, uint32_t *row)
{
   if (util_format_is_depth_or_stencil(format)) {
      util_format_unpack_z_float(format, (float *)row, line, w);
   } else {
      /* (sRGB as the bytes are: the program decodes them, or not, as the
       * view says) */
      util_format_unpack_rgba_8unorm_rect(util_format_linear(format), (uint8_t *)row, w * 4,
                                          line, 0, w, 1);
   }
}

/* The copy the sampler reads, made again from the linear content: every
 * level of every face (a cube map's six, in GL's order), one after another,
 * each RGBA8 in Morton order at its own size padded to powers of two --
 * iOS's layout (a 64x64's levels at +0, +0x4000, +0x5000, ...,
 * p105-gpu.md's mipmap transfers); a cube map's faces each as far apart as
 * a whole chain down to 1 x 1, whatever levels it has, rounded up to 2 KiB
 * once level 0 is 512 bytes or more (glmip --probe, RGBA8: 4 x 4 faces 21
 * texels apart, 8 x 8 85, 16 x 16 512, 32 x 32 1536, 64 x 64 5632; Vita3K's
 * texture cache has the same rules for the same GPU -- M43: in bytes, as
 * one-byte and eight-byte texels' cube maps showed).  A 2D texture's size not a power of two is
 * padded, the last column and row repeated: the sampler takes the size as
 * it is (sgx_resource_texture), the padding is its memory's layout.  A cube
 * map's is scaled up to one, the nearest texel (GXM's arbitrary cube map,
 * type 7, read past the copy here and hung the renders): a direction cannot
 * be scaled to a part of a face.
 *
 * M37: made in cached memory and copied over in one go (Morton order's
 * scattered stores into a write-combined map were most of the time); into
 * a new buffer from the cache when a render may still read the old one --
 * gathered or running -- the old one left to it (the batch references it),
 * not waiting for the GPU to be idle.  A texture of one level written in
 * boxes of it since (glamor's glyphs, a game's streamed sprites) keeps its
 * copy's shadow, tw_cpu: only the box is made again there, and the range of
 * the copy it falls in copied over. */
/* M37: formats the sampler reads as they are in memory -- iOS's corpus
 * m10..m19, the words of PS Vita's SceGxmTexture: the base format in word 1
 * bits 28:24, the swizzle in word 3 bits 30:28 (named from the top bits
 * down: ABGR is R in the low byte).  Their texels are twiddled as they are,
 * 1, 2 or 4 bytes; the sampler hands each back as the four bytes an RGBA8
 * texel's are, in the same order (iOS's program for 565 and 4444 is `or o0,
 * pa0`).  0: not one of them -- made RGBA8 by util_format.  SGX_TEX_NATIVE=0:
 * none. */
static unsigned
native_format(const struct pipe_resource *p, uint32_t *fmt, uint32_t *swizzle)
{
   static int on = -1;

   if (on < 0)
      on = !getenv("SGX_TEX_NATIVE") || atoi(getenv("SGX_TEX_NATIVE"));
   *fmt = 0x0c;
   *swizzle = 0;
   if (!on)
      return 0;
   switch (util_format_linear(p->format)) {
   case PIPE_FORMAT_R8G8B8A8_UNORM: *fmt = 0x0c; *swizzle = 0; return 4;  /* ABGR */
   case PIPE_FORMAT_B8G8R8A8_UNORM: *fmt = 0x0c; *swizzle = 1; return 4;  /* ARGB */
   case PIPE_FORMAT_R8G8B8X8_UNORM: *fmt = 0x0c; *swizzle = 4; return 4;  /* 1BGR: iOS's RGB8 */
   case PIPE_FORMAT_B8G8R8X8_UNORM: *fmt = 0x0c; *swizzle = 5; return 4;  /* 1RGB */
   case PIPE_FORMAT_B5G6R5_UNORM:   *fmt = 0x05; *swizzle = 5; return 2;  /* 1RGB: iOS's 565 */
   case PIPE_FORMAT_B4G4R4A4_UNORM: *fmt = 0x02; *swizzle = 1; return 2;  /* ARGB */
   case PIPE_FORMAT_B5G5R5A1_UNORM: *fmt = 0x04; *swizzle = 1; return 2;  /* ARGB */
   case PIPE_FORMAT_L8_UNORM:       *fmt = 0x00; *swizzle = 5; return 1;  /* 1RRR: iOS's L8 */
   case PIPE_FORMAT_A8_UNORM:       *fmt = 0x00; *swizzle = 6; return 1;  /* R000: A8 */
   case PIPE_FORMAT_I8_UNORM:       *fmt = 0x00; *swizzle = 3; return 1;  /* RRRR: I8 (M43) */
   case PIPE_FORMAT_L8A8_UNORM:     *fmt = 0x07; *swizzle = 2; return 2;  /* GRRR: LA8 */
   default:                         return 0;
   }
}

/* Whether a format the sampler has not is copied as four half floats
 * (GXM's F16F16F16F16, 0x1b, which it filters) instead of RGBA8 (M43):
 * floats, signed and wider than 8-bit channels, the packed float and shared
 * exponent formats -- ARB_texture_float, EXT_texture_snorm and the rest of
 * GL 3.0's.  Its texels come back F32 (sgx_blend_key's tex_f16). */
bool
sgx_resource_copy_f16(const struct pipe_resource *p)
{
   const struct util_format_description *desc = util_format_description(p->format);
   uint32_t fmt, swizzle;

   if (!desc || util_format_is_depth_or_stencil(p->format) ||
       util_format_is_pure_integer(p->format) || util_format_is_compressed(p->format) ||
       native_format(p, &fmt, &swizzle))
      return false;
   if (p->format == PIPE_FORMAT_R11G11B10_FLOAT || p->format == PIPE_FORMAT_R9G9B9E5_FLOAT)
      return true;
   for (unsigned c = 0; c < desc->nr_channels; c++)
      if (desc->channel[c].type == UTIL_FORMAT_TYPE_FLOAT ||
          desc->channel[c].type == UTIL_FORMAT_TYPE_SIGNED || desc->channel[c].size > 8)
         return true;
   return false;
}

/* the texel bytes of res's copy: its own, half floats' or RGBA8's */
static unsigned
copy_bytes(const struct sgx_resource *res)
{
   uint32_t fmt, swizzle;
   unsigned b = native_format(&res->base, &fmt, &swizzle);

   return b ? b : sgx_resource_copy_f16(&res->base) ? 8 : 4;
}

/* a row of a format the copy holds as half floats: R, G, B, A each */
static void
unpack_row_f16(enum pipe_format format, const uint8_t *line, unsigned w, uint32_t *row)
{
   float rgba[4 * 64];

   for (unsigned x = 0; x < w; x += 64) {
      unsigned n = MIN2(w - x, 64);

      util_format_unpack_rgba(format, rgba, line + x * util_format_get_blocksize(format), n);
      for (unsigned i = 0; i < 4 * n; i++)
         ((uint16_t *)row)[4 * x + i] = _mesa_float_to_half(rgba[i]);
   }
}

/* Rows y0..y1, columns x0..x1 of a level of the copy, from the linear
 * content: the texels as they are (native, b bytes) or made RGBA8 (row: a
 * row's worth of room) */
static void
copy_level(const struct sgx_resource *res, unsigned level, unsigned face_layer, uint8_t *dst,
           unsigned dw, unsigned dh, bool scale, unsigned x0, unsigned y0, unsigned x1,
           unsigned y1, uint32_t *row, const struct twiddle *t)
{
   const struct pipe_resource *p = &res->base;
   unsigned lw = u_minify(p->width0, level), lh = u_minify(p->height0, level);
   const uint8_t *src = (const uint8_t *)res->bo->map + res->offset[level] +
                        face_layer * res->layer_size[level];
   uint32_t fmt, swizzle;
   unsigned native = native_format(p, &fmt, &swizzle);
   bool f16 = !native && sgx_resource_copy_f16(p);
   unsigned out = native ? native : f16 ? 8 : 4;
   /* the columns of content the box reads (padding: the last one) */
   unsigned cx0 = scale ? 0 : MIN2(x0, lw - 1), cx1 = scale ? lw : MAX2(MIN2(x1, lw), cx0 + 1);
   unsigned bytes = util_format_get_blocksize(p->format);

   for (unsigned y = y0; y < y1; y++) {
      unsigned sy = scale ? (2 * y + 1) * lh / (2 * dh) : MIN2(y, lh - 1);
      const uint8_t *line = src + sy * res->stride[level];

      /* padding: the last column and row repeated (a bilinear lookup at
       * the edge reads it) */
#define SX(x) ((scale ? (2 * (x) + 1) * lw / (2 * dw) : MIN2((x), lw - 1)) - cx0)
      /* (the row read in one go: the linear content is mapped
       * write-combined, word by word reads of it were slower than the
       * unpacking) */
      if (native)
         memcpy(row, line + cx0 * bytes, (cx1 - cx0) * bytes);
      else if (f16)
         unpack_row_f16(p->format, line + cx0 * bytes, cx1 - cx0, row);
      else
         unpack_row(p->format, line + cx0 * bytes, cx1 - cx0, row);
      switch (out) {
      case 1:
         for (unsigned x = x0; x < x1; x++)
            dst[twiddle_at(t, x, y)] = ((const uint8_t *)row)[SX(x)];
         break;
      case 2:
         for (unsigned x = x0; x < x1; x++)
            ((uint16_t *)dst)[twiddle_at(t, x, y)] = ((const uint16_t *)row)[SX(x)];
         break;
      case 8:
         for (unsigned x = x0; x < x1; x++)
            ((uint64_t *)dst)[twiddle_at(t, x, y)] = ((const uint64_t *)row)[SX(x)];
         break;
      default:
         for (unsigned x = x0; x < x1; x++)
            ((uint32_t *)dst)[twiddle_at(t, x, y)] = row[SX(x)];
         break;
      }
#undef SX
   }
}

static bool zs_to_linear(struct sgx_resource *res, unsigned level, unsigned layer);

static bool
copy_texture(struct sgx_context *ctx, struct sgx_resource *res, unsigned first)
{
   struct sgx_screen *screen = sgx_screen(res->base.screen);
   struct pipe_resource *p = &res->base;
   unsigned w = u_minify(p->width0, first), h = u_minify(p->height0, first);
   unsigned tw = util_next_power_of_two(w), th = util_next_power_of_two(h);
   unsigned faces = p->target == PIPE_TEXTURE_CUBE ? 6 : 1;
   unsigned levels = p->last_level + 1 - first;
   unsigned b = copy_bytes(res);
   bool scale = faces > 1 && (tw != w || th != h);
   bool same = res->tw && res->tw_w == tw && res->tw_h == th && res->tw_levels == levels &&
               res->tw_first == first;
   /* boxes of level 0 alone written since the copy was made */
   bool boxed = same && faces == 1 && levels == 1 && !first &&
                !util_format_is_compressed(p->format) && res->tw_writes &&
                res->tw_writes == res->seq - res->tw_seq;
   uint32_t size = 0, face = 0, lo = 0, hi, *row;
   unsigned x0 = 0, y0 = 0, x1 = tw, y1 = th;
   uint8_t *out;
   struct twiddle t;
   bool fresh, partial;

   for (unsigned l = 0; l < levels; l++)
      size += MAX2(tw >> l, 1) * MAX2(th >> l, 1) * b;
   for (unsigned l = 0; faces > 1 && (tw >> l || th >> l); l++)
      face += MAX2(tw >> l, 1) * MAX2(th >> l, 1) * b;
   /* (M43: a face 2 KiB aligned when level 0's is 512 bytes or more --
    * 16 x 16 of four-byte texels, 8 x 8 of eight, 32 x 32 of one: dEQP's
    * random_teximage2d.cube_7, an 8 x 8 cube map of half floats) */
   if (faces > 1 && tw * th * b >= 512)
      face = align(face, 2048);
   if (faces > 1)
      size = 5 * face + size;
   hi = size;
   if (!same || (!boxed && res->tw_cpu)) {
      FREE(res->tw_cpu);
      res->tw_cpu = NULL;
   }
   /* the first box written: from now on a shadow of the copy */
   partial = boxed && res->tw_cpu;
   if (boxed && !res->tw_cpu)
      res->tw_cpu = MALLOC(size);
   if (partial) {
      /* (the padding of the box's last column and row too) */
      x0 = res->tw_x0;
      y0 = res->tw_y0;
      x1 = res->tw_x1 >= w ? tw : res->tw_x1;
      y1 = res->tw_y1 >= h ? th : res->tw_y1;
   }
   /* a new copy, or the old one again if no render reads it -- or if a
    * small box of a big copy changed: then the renders that read it go
    * first and are waited for, cheaper than copying all of it over (64
    * boxes of 16 x 16 into 1024 x 1024: 1.2 s the other way, glupload) */
   if (partial && same && size >= (512u << 10) && res->tw_cpu) {
      unsigned bw = (x1 - x0) * (y1 - y0) * b * 4;

      if (bw <= size / 8) {
         if (sgx_batch_reads(ctx, res->tw))
            sgx_batch_flush(ctx);
         if (!sgx_bo_idle(res->tw))
            sgx_bo_wait(res->tw, -1);
      }
   }
   fresh = !same || sgx_batch_reads(ctx, res->tw) || !sgx_bo_idle(res->tw);
   if (fresh) {
      struct sgx_bo *bo = sgx_bo_cache_get(&screen->dev, size);

      if (!bo)
         return false;
      sgx_bo_destroy(res->tw);
      res->tw = bo;
      res->tw_w = tw;
      res->tw_h = th;
      res->tw_levels = levels;
      res->tw_first = first;
   }
   out = res->tw_cpu ? res->tw_cpu : MALLOC(size);
   row = MALLOC(MAX2(tw, 1) * 8);
   if (!out || !row || !sgx_bo_map(res->bo)) {
      if (out != res->tw_cpu)
         FREE(out);
      FREE(row);
      return false;
   }
   /* what the GPU wrote into the texture, done first -- a depth buffer's
    * from the ISP's tiles (M37: a shadow map) */
   if (res->gpu_written || res->external)
      sgx_bo_wait(res->bo, -1);
   for (unsigned i = 0; util_format_is_depth_or_stencil(p->format) && i < res->nzls; i++)
      if (res->zls[i].valid || res->zls[i].cleared)
         zs_to_linear(res, i / p->array_size, i % p->array_size);
   for (unsigned f = 0; f < faces; f++) {
      uint8_t *dst = out + f * face;

      for (unsigned l = 0; l < levels; l++) {
         unsigned dw = MAX2(tw >> l, 1), dh = MAX2(th >> l, 1);

         if (!twiddle_init(&t, dw, dh)) {
            if (out != res->tw_cpu)
               FREE(out);
            FREE(row);
            return false;
         }
         if (partial) {
            copy_level(res, first + l, f, dst, dw, dh, scale, x0, y0, x1, y1, row, &t);
            lo = twiddle_at(&t, x0, y0) * b;
            hi = (twiddle_at(&t, x1 - 1, y1 - 1) + 1) * b;
         } else {
            copy_level(res, first + l, f, dst, dw, dh, scale, 0, 0, dw, dh, row, &t);
         }
         FREE(t.mx);
         dst += dw * dh * b;
      }
   }
   FREE(row);
   /* a new buffer takes all of it; the old one the range the box is in */
   if (fresh)
      lo = 0, hi = size;
   memcpy((uint8_t *)res->tw->map + lo, out + lo, hi - lo);
   if (out != res->tw_cpu)
      FREE(out);
   /* SGX_TEX_PROBE=1: a cube map's copy holds each texel's own index
    * instead (finding the layout: glmip --probe) */
   if (faces > 1 && b == 4 && getenv("SGX_TEX_PROBE"))
      for (uint32_t i = 0; i < size / 4; i++)
         ((uint32_t *)res->tw->map)[i] = 0xff000000u | i;
   /* SGX_DEBUG=copies: each copy made, what of it and why */
   {
      static int on = -1;

      if (on < 0)
         on = getenv("SGX_DEBUG") && strstr(getenv("SGX_DEBUG"), "copies");
      if (on)
         mesa_logi("sgx: copy %p %s %ux%u l%u: %s, %s buffer, %u bytes (%u writes, seq +%u%s%s)",
                   (void *)res, util_format_short_name(p->format), w, h, levels,
                   partial ? "a box" : "all", fresh ? "a new" : "the same", hi - lo,
                   res->tw_writes, res->seq - res->tw_seq, res->gpu_written ? ", GPU-written" : "",
                   same ? "" : ", new size");
   }
   res->tw_seq = res->seq;
   res->tw_writes = 0;
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
sgx_resource_linear(const struct sgx_resource *res, const struct pipe_sampler_state *ss)
{
   const struct pipe_resource *p = &res->base;
   static int force = -1;
   uint32_t fmt, swizzle;

   if (force < 0)
      force = getenv("SGX_LINEAR_TEX") ? atoi(getenv("SGX_LINEAR_TEX")) : 2;
   /* SGX_LINEAR_TEX=0: never, 1: whenever it can */
   if (force == 0 || !native_format(p, &fmt, &swizzle) || p->last_level ||
       p->array_size > 1 || (res->stride[0] & 15) || res->stride[0] < 32 ||
       ((res->offset[0] + res->bo->va) & 15) ||
       (p->target != PIPE_TEXTURE_2D && p->target != PIPE_TEXTURE_RECT))
      return false;
   /* (a size not a power of two is twiddled too, M28: the minification
    * filter is lost here) -- and, M37, one sampled with the nearest texel
    * whether minified or magnified, clamped both ways: nothing is lost
    * then (dEQP's nearest_linear_clamp_*_npot, piglit's texwrap 2d with
    * repeat and mirror went wrong linear), and no copy is made (glamor's
    * masks, a new texture of the window's size at every step of a window
    * dragged, were copied once each) */
   return force == 1 || res->external || res->gpu_written ||
          (ss && ss->min_img_filter == PIPE_TEX_FILTER_NEAREST &&
           ss->mag_img_filter == PIPE_TEX_FILTER_NEAREST &&
           wrap_bits(ss->wrap_s) == 2 && wrap_bits(ss->wrap_t) == 2);
}

/* The levels a lookup through view with ss may take (M35): from *first
 * to *last.  The sampler's own are from its copy's level 0 to the last
 * level in word 0, so GL's base and maximum levels and its LOD clamps are
 * the copy's: it starts at the base level -- or, with a minimum LOD m, m
 * levels further (the sampler's LOD, worked out from that level's size, is
 * then GL's less m, and its level 0 GL's level m: max(lambda, m)) -- and
 * the last level is the maximum level's or the maximum LOD's.  Exact for
 * whole numbers. */
static void
sampled_levels(const struct sgx_resource *res, const struct pipe_sampler_view *view,
               const struct pipe_sampler_state *ss, unsigned *first, unsigned *last)
{
   unsigned base = view ? MIN2(view->u.tex.first_level, res->base.last_level) : 0;
   unsigned top = view ? CLAMP(view->u.tex.last_level, base, res->base.last_level) :
                         res->base.last_level;

   *first = *last = base;
   if (!ss || ss->min_mip_filter == PIPE_TEX_MIPFILTER_NONE)
      return;
   if (ss->max_lod < (float)(top - base))
      top = base + (unsigned)MAX2(ss->max_lod, 0.0f);
   *last = top;
   if (ss->min_lod > 0)
      *first = MIN2(base + (unsigned)ceilf(ss->min_lod), top);
}

bool
sgx_resource_texture(struct sgx_context *ctx, struct sgx_resource *res,
                     const struct pipe_sampler_view *view,
                     const struct pipe_sampler_state *ss, uint32_t words[4])
{
   struct pipe_resource *p = &res->base;

   if ((p->target != PIPE_TEXTURE_1D && p->target != PIPE_TEXTURE_2D &&
        p->target != PIPE_TEXTURE_RECT && p->target != PIPE_TEXTURE_CUBE) ||
       p->array_size != (p->target == PIPE_TEXTURE_CUBE ? 6 : 1) || !p->width0 || !p->height0)
      return false;
   if (sgx_resource_linear(res, ss)) {
      uint32_t fmt, swizzle;

      /* the 2D engine's way (apple_sgx_hw.c, blt_tex: iOS's for an
       * IOSurface): the content itself, its stride in 16 bytes less 2;
       * bits 11:9 all set, or the rows come out skewed -- every other value
       * of the three was tried (gltex), and the minification filter they
       * are for twiddled textures is lost: point sampling.  Type 6 (GXM's
       * linear strided), the format and its swizzle as a copy's (M37: the
       * sampler reads B G R A by swizzle 1, ARGB, not by itself) */
      native_format(p, &fmt, &swizzle);
      words[0] = (res->stride[0] / 16 - 2) << 16 | (sampler_bits(ss) & ~0xe00u) | 7u << 9;
      words[1] = 6u << 29 | fmt << 24 | (p->width0 - 1) << 12 | (p->height0 - 1);
      words[2] = res->bo->va + res->offset[0];
      words[3] = swizzle << 28;
      res->sampled = res->bo;
   } else {
      bool cube = p->target == PIPE_TEXTURE_CUBE;
      unsigned first, last, w, h;
      uint32_t fmt, swizzle;
      int bias;

      sampled_levels(res, view, ss, &first, &last);
      if ((!res->tw || res->tw_seq != res->seq || res->tw_first != first) &&
          !copy_texture(ctx, res, first))
         return false;
      w = u_minify(p->width0, first);
      h = u_minify(p->height0, first);
      /* bits 20:17 the last level the sampler goes down to (iOS's 4 x 4
       * cube map, one level, has 0; glmip): with no mip filter, level 0's
       * alone; bits 26:21 the LOD bias in eighths, 31 none (iOS's corpus:
       * 39 for 1.0, 43 for 1.5 -- Vita3K's SceGxmTexture, the same words) */
      bias = 31 + (ss ? (int)lroundf(ss->lod_bias * 8) : 0);
      words[0] = (uint32_t)CLAMP(bias, 0, 63) << 21 |
                 MIN2(last - first, res->tw_levels - 1) << 17 | sampler_bits(ss);
      /* word 1: the type in bits 31:29 (0 twiddled, 2 a cube map -- iOS's
       * t04, 0x4c020002 -- 5 twiddled of any size, 6 strided: GXM's
       * SceGxmTextureType), the format in 28:24, the size: log2 w in 19:16
       * and log2 h in 3:0, or w - 1 in 23:12 and h - 1 in 11:0 */
      native_format(p, &fmt, &swizzle);
      /* (M43) a copy of half floats, R G B A: swizzle ABGR */
      if (sgx_resource_copy_f16(p)) {
         fmt = 0x1b;
         swizzle = 0;
      }
      /* depth: F32 (0x12), its one channel raw; nearest -- the sampler
       * filters no F32 (iOS has no OES_texture_float_linear) */
      if (util_format_is_depth_or_stencil(p->format)) {
         fmt = 0x12;
         swizzle = 0;
         words[0] &= ~(1u << 9 | 3u << 10 | 3u << 12);
      }
      if (cube || (util_is_power_of_two_nonzero(w) && util_is_power_of_two_nonzero(h)))
         words[1] = (cube ? 2u << 29 : 0) | fmt << 24 |
                    util_logbase2(res->tw_w) << 16 | util_logbase2(res->tw_h);
      else
         words[1] = 5u << 29 | fmt << 24 | (w - 1) << 12 | (h - 1);
      words[2] = res->tw->va;
      words[3] = swizzle << 28;
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

/* ---- a depth/stencil buffer's content for the CPU (M35) -------------------
 *
 * Depth and stencil live in the render's tiles, and in memory only as the
 * ISP stores them (M24, res->zls): 32 x 32 pixels a tile, row by row, 4
 * bytes a pixel -- F32 depth alone, or 24-bit depth with the stencil in the
 * top byte -- tile (x, y) at (x + y * S) * 4 KiB, S two tiles for every 64
 * pixels of the width (the level's).  F32 depth with stencil (M43,
 * SGX_ZLS_F32S8): 5 bytes a pixel, 5 KiB a tile, each row of 32 a stream
 * of 160 bytes -- for each pixel the stencil, then the depth's bits as a
 * key that sorts as an integer (a positive float's sign bit flipped, a
 * negative one's every bit), most significant byte first -- each 16 bytes
 * of which are stored reversed (found from a pattern of known depths and
 * stencils, every pixel of a 256 x 256 buffer).  A map of a level's layer
 * brings the resource's own linear copy of it, in its format, up to date
 * from them (or from a clear no render has done yet); an unmap that wrote
 * puts it back, for the next render to load. */
struct sgx_zls *
sgx_resource_zls(struct sgx_resource *res, unsigned level, unsigned layer)
{
   unsigned n = (res->base.last_level + 1) * res->base.array_size;

   if (level > res->base.last_level || layer >= res->base.array_size)
      return NULL;
   if (!res->zls) {
      if (!(res->zls = CALLOC(n, sizeof(*res->zls))))
         return NULL;
      res->nzls = n;
   }
   return &res->zls[level * res->base.array_size + layer];
}

static uint32_t *
zls_texel(const struct sgx_zls *zls, unsigned w, unsigned x, unsigned y)
{
   unsigned s = 2 * DIV_ROUND_UP(w, 64);

   return (uint32_t *)((uint8_t *)zls->bo->map + (x / 32 + y / 32 * s) * 4096) +
          (y % 32) * 32 + x % 32;
}

/* (M43) an SGX_ZLS_F32S8 pixel's byte i (0 the stencil, 1..4 the key) */
static uint8_t *
zls_f32s8_byte(const struct sgx_zls *zls, unsigned w, unsigned x, unsigned y, unsigned i)
{
   unsigned s = 2 * DIV_ROUND_UP(w, 64), at = 5 * (x % 32) + i;

   return zls->bo->map + (x / 32 + y / 32 * s) * 5120 + (y % 32) * 160 + (at & ~15u) +
          15 - at % 16;
}

/* pixel (x, y)'s depth and stencil as the ISP stored them in format f */
static void
zls_get(const struct sgx_zls *zls, enum sgx_zls_format f, unsigned w, unsigned x, unsigned y,
        float *z, uint8_t *st)
{
   uint32_t wd;

   if (f == SGX_ZLS_F32S8) {
      uint32_t key = 0;

      for (unsigned i = 1; i <= 4; i++)
         key = key << 8 | *zls_f32s8_byte(zls, w, x, y, i);
      wd = key & 0x80000000u ? key ^ 0x80000000u : ~key;
      memcpy(z, &wd, 4);
      *st = *zls_f32s8_byte(zls, w, x, y, 0);
      return;
   }
   wd = *zls_texel(zls, w, x, y);
   if (f == SGX_ZLS_D24S8) {
      *z = (wd & 0xffffff) / 16777215.0f;
      *st = wd >> 24;
   } else {
      memcpy(z, &wd, 4);
      *st = 0;
   }
}

static void
zls_put(const struct sgx_zls *zls, enum sgx_zls_format f, unsigned w, unsigned x, unsigned y,
        float z, uint8_t st)
{
   uint32_t wd;

   if (f == SGX_ZLS_F32S8) {
      uint32_t key;

      memcpy(&wd, &z, 4);
      key = wd & 0x80000000u ? ~wd : wd ^ 0x80000000u;
      *zls_f32s8_byte(zls, w, x, y, 0) = st;
      for (unsigned i = 1; i <= 4; i++)
         *zls_f32s8_byte(zls, w, x, y, i) = key >> (32 - 8 * i);
      return;
   }
   if (f == SGX_ZLS_D24S8) {
      /* (in doubles: 1.0 in floats came to 2^24, a carry into the
       * stencil) */
      wd = (uint32_t)st << 24 | (uint32_t)(CLAMP(z, 0.0f, 1.0f) * 16777215.0 + 0.5);
   } else {
      memcpy(&wd, &z, 4);
   }
   *zls_texel(zls, w, x, y) = wd;
}

static bool
zs_to_linear(struct sgx_resource *res, unsigned level, unsigned layer)
{
   struct pipe_resource *p = &res->base;
   struct sgx_zls *zls = sgx_resource_zls(res, level, layer);
   unsigned w = u_minify(p->width0, level), h = u_minify(p->height0, level);
   bool stencil = util_format_has_stencil(util_format_description(p->format));
   bool depth = util_format_has_depth(util_format_description(p->format));
   bool tiles = zls && zls->bo && zls->valid && sgx_bo_map(zls->bo);
   enum sgx_zls_format f = sgx_zls_format(p->format);
   uint8_t *map = sgx_bo_map(res->bo);
   float *z = MALLOC(w * sizeof(float));
   uint8_t *st = MALLOC(w);

   if (!map || !z || !st || !zls) {
      FREE(z);
      FREE(st);
      return false;
   }
   /* (never stored nor cleared: the copy is what there is) */
   if (!tiles && !zls->cleared) {
      FREE(z);
      FREE(st);
      return true;
   }
   if (tiles)
      sgx_bo_wait(zls->bo, -1);
   for (unsigned y = 0; y < h; y++) {
      uint8_t *row = map + res->offset[level] + layer * res->layer_size[level] +
                     y * res->stride[level];

      for (unsigned x = 0; x < w; x++) {
         z[x] = 0;
         st[x] = 0;
         if (tiles)
            zls_get(zls, f, w, x, y, &z[x], &st[x]);
         if (zls->cleared)
            z[x] = zls->clear;
      }
      if (depth)
         util_format_pack_z_float(p->format, row, z, w);
      if (stencil)
         util_format_pack_s_8uint(p->format, row, st, w);
   }
   FREE(z);
   FREE(st);
   return true;
}

static void
linear_to_zs(struct sgx_screen *screen, struct sgx_resource *res, unsigned level,
             unsigned layer)
{
   struct pipe_resource *p = &res->base;
   struct sgx_zls *zls = sgx_resource_zls(res, level, layer);
   unsigned lw = u_minify(p->width0, level), lh = u_minify(p->height0, level);
   bool stencil = util_format_has_stencil(util_format_description(p->format));
   bool depth = util_format_has_depth(util_format_description(p->format));
   enum sgx_zls_format f = sgx_zls_format(p->format);
   uint8_t *map = sgx_bo_map(res->bo);
   float *z = MALLOC(lw * sizeof(float));
   uint8_t *st = MALLOC(lw);

   if (zls && !zls->bo) {
      zls->bo = sgx_bo_create(&screen->dev, sgx_frame_zls_size(lw, lh, f), 0, 0);
      zls->valid = false;
   }
   if (map && z && st && zls && zls->bo && sgx_bo_map(zls->bo)) {
      sgx_bo_wait(zls->bo, -1);
      for (unsigned y = 0; y < lh; y++) {
         const uint8_t *row = map + res->offset[level] + layer * res->layer_size[level] +
                              y * res->stride[level];

         if (depth)
            util_format_unpack_z_float(p->format, z, row, lw);
         if (stencil)
            util_format_unpack_s_8uint(p->format, st, row, lw);
         for (unsigned x = 0; x < lw; x++)
            zls_put(zls, f, lw, x, y, depth ? z[x] : 0, stencil ? st[x] : 0);
      }
      zls->valid = true;
      zls->cleared = false;
   }
   FREE(z);
   FREE(st);
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
    * (M26: sgx_buffer_bo).  Nor for a texture whose linear content no
    * render reads -- sampled through its copy -- or writes (M37): it is the
    * CPU's alone (glamor's glyphs went one render and a wait each). */
   if (!(usage & PIPE_MAP_UNSYNCHRONIZED) && prsc->target != PIPE_BUFFER) {
      bool gpu = res->gpu_written || res->external ||
                 util_format_is_depth_or_stencil(prsc->format) ||
                 ctx->batch.rt == prsc || ctx->batch.zs == prsc ||
                 sgx_batch_reads(ctx, res->bo) || !sgx_bo_idle(res->bo);

      if (gpu && sgx_batch_uses(ctx, prsc))
         sgx_batch_flush(ctx);
      if (gpu && !sgx_bo_wait(res->bo, -1))
         return NULL;
   }
   /* a depth clear at the start of a render that has no draws yet: the
    * buffer's own from now on (what the CPU reads, or writes over) */
   if (ctx->batch.zs_cleared && !ctx->batch.ndraws && ctx->fb.zsbuf.texture == prsc) {
      struct sgx_zls *zls = sgx_resource_zls(res, ctx->fb.zsbuf.level, ctx->fb.zsbuf.first_layer);

      if (zls) {
         zls->cleared = true;
         zls->clear = ctx->batch.depth_clear;
      }
      ctx->batch.zs_cleared = false;
   }
   /* depth and stencil from the ISP's tiles, unless all of it is to be
    * written (the level's layers the box takes) */
   if (util_format_is_depth_or_stencil(prsc->format) && prsc->target != PIPE_BUFFER &&
       !(usage & PIPE_MAP_DISCARD_WHOLE_RESOURCE) &&
       !((usage & PIPE_MAP_DISCARD_RANGE) && box->width == (int)u_minify(prsc->width0, level) &&
         box->height == (int)u_minify(prsc->height0, level))) {
      for (int z = box->z; z < box->z + MAX2(box->depth, 1); z++)
         if (!zs_to_linear(res, level, z))
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
      const struct pipe_box *box = &ptrans->box;

      res->seq++;
      /* a box of level 0 of a 2D texture of one level: the copy made again
       * for it alone (M37: copy_texture) */
      if (ptrans->resource->target != PIPE_BUFFER && !ptrans->level &&
          !ptrans->resource->last_level && ptrans->resource->array_size == 1 &&
          (ptrans->resource->target == PIPE_TEXTURE_2D ||
           ptrans->resource->target == PIPE_TEXTURE_RECT) &&
          !box->z && box->depth == 1 && box->width > 0 && box->height > 0) {
         if (!res->tw_writes) {
            res->tw_x0 = box->x;
            res->tw_y0 = box->y;
            res->tw_x1 = box->x + box->width;
            res->tw_y1 = box->y + box->height;
         } else {
            res->tw_x0 = MIN2(res->tw_x0, (unsigned)box->x);
            res->tw_y0 = MIN2(res->tw_y0, (unsigned)box->y);
            res->tw_x1 = MAX2(res->tw_x1, (unsigned)(box->x + box->width));
            res->tw_y1 = MAX2(res->tw_y1, (unsigned)(box->y + box->height));
         }
         res->tw_writes++;
      }
      if (ptrans->resource->target == PIPE_BUFFER && !(ptrans->usage & PIPE_MAP_FLUSH_EXPLICIT))
         buffer_written(res, ptrans->box.x, ptrans->box.x + ptrans->box.width, ptrans->usage);
      if (util_format_is_depth_or_stencil(ptrans->resource->format) &&
          ptrans->resource->target != PIPE_BUFFER) {
         for (int z = box->z; z < box->z + MAX2(box->depth, 1); z++)
            linear_to_zs(sgx_screen(pctx->screen), res, ptrans->level, z);
      }
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
