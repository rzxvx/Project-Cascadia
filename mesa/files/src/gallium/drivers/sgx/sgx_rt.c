/*
 * Copyright 2026 Project Cascadia
 * SPDX-License-Identifier: MIT
 *
 * Render target data (docs/research/p105-mesa.md, M10).  What the kext
 * does for a render target, rewritten from its behaviour: 0x80bf7aac (the
 * init: tiles, the "big" flag), 0x80bf6e78 and 0x80bf6ed8 (the context
 * areas), 0x80bf6fd8 (macrotiles and the buffers' sizes), 0x80bf74f4 (the
 * render details and the state buffer) and 0x80bf5eec (the submit's
 * payload words and 3D register block).  mesa/host/rt-test.py checks every
 * byte against tools/iosgpu/rtemu.py, which runs the kext's own code.
 *
 * The screen is cut into 32 x 32 tiles and the tiles into 2 x 2
 * macrotiles, whose sides are a multiple of 4 tiles; the TA keeps a region
 * array per pixel pipe (12 bytes a tile) and tail pointers, and the CPU
 * writes the render details -- where all of it is, for the microkernel --
 * and the state buffer: a full-screen object the clear draws (vertices in
 * 12.4 fixed point, biased by 0x4000), a tile-sized one, and a stream entry
 * per region pointing at the latter.  Everything is kept twice ("copies"),
 * for a TA and a 3D pass in flight at once; renders here use copy 0.
 *
 * One sample only: the kext's multi-sample path (samples > 1: a tile of
 * 16 x 16 pixels, a "big" limit of 1024) and its 4 x 4 macrotiles (an
 * RT field the init sets to -1, never 2) are not here.
 */
#include "sgx_rt.h"

#include <assert.h>
#include <string.h>

#define ONE        0x3f800000u   /* 1.0f */
#define COPY       0x84          /* the render details: copy 1 at +0x84 */
#define FIRST      0x140         /* then each copy's first regions */

static uint32_t
align(uint32_t v, uint32_t a)
{
   return (v + a - 1) & ~(a - 1);
}

static uint32_t
min(uint32_t a, uint32_t b)
{
   return a < b ? a : b;
}

/* the power of two at or above v, as the kext finds it (from bit 12 down:
 * v below 0x2000) */
static uint32_t
npot(uint32_t v)
{
   uint32_t h = 0x1000;

   assert(v < 0x2000);
   while (h > 1 && !(v & h))
      h >>= 1;
   return h << !!((h - 1) & v);
}

bool
sgx_rt_layout(struct sgx_rt *rt, unsigned w, unsigned h, unsigned ncores)
{
   uint32_t size = 0, p;

   if (!w || !h || w > SGX_RT_MAX_SIZE || h > SGX_RT_MAX_SIZE || !ncores ||
       ncores > SGX_RT_MAX_CORES)
      return false;
   memset(rt, 0, sizeof(*rt));
   rt->w = w;
   rt->h = h;
   rt->ncores = ncores;
   rt->tiles_x = (w + 31) >> 5;
   rt->tiles_y = (h + 31) >> 5;
   rt->big = w > 2048 || h > 2048;

   /* 0x80bf6e78: the context areas, the same for every target */
   for (unsigned k = 0; k < 2; k++) {
      rt->context[k] = size;
      size = (size + 0x6f7) & ~0xfu;
   }
   for (unsigned c = 0; c < ncores; c++) {
      rt->context[2 + c] = size;
      size = (size + 0x17f) & ~0xffu;
      rt->context[2 + SGX_RT_MAX_CORES + c] = size;
      size = (size + 0xaf) & ~0x1fu;
   }
   rt->size[SGX_RT_CONTEXT] = size;

   /* 0x80bf6fd8: 2 x 2 macrotiles of half the tiles each way, rounded up
    * to 4 */
   rt->mt_across = rt->mt_down = 2;
   rt->mt_w = align((rt->tiles_x + 1) >> 1, 4);
   rt->mt_h = align((rt->tiles_y + 1) >> 1, 4);
   rt->regions = rt->mt_w * rt->mt_across * rt->mt_h * rt->mt_down;
   rt->region_size = align(align(rt->regions * 12, 64), 4096);
   p = npot(rt->mt_w * rt->mt_across);
   if (npot(rt->mt_h * rt->mt_across) > p)
      p = npot(rt->mt_h * rt->mt_across);
   rt->tail_size = align(align(p * p * 8, 64), 4096);
   rt->size[SGX_RT_REGIONS0] = rt->size[SGX_RT_REGIONS1] = rt->region_size * ncores;
   rt->size[SGX_RT_TAILS] = rt->tail_size * ncores;

   size = FIRST;
   for (unsigned k = 0; k < 2; k++) {
      rt->first[k] = size;
      size = align(size + rt->mt_across * rt->mt_down * ncores * 4, 32);
   }
   rt->size[SGX_RT_DETAILS] = size;

   rt->state_clear = 0;
   rt->state_tile = 0x80;
   rt->state_regions = (rt->state_tile + 0xbf) & 0x380;
   rt->size[SGX_RT_STATE] = rt->state_regions + ncores * rt->regions * 64 + 0x100;
   return true;
}

/* the macrotiles' bounds, in tiles, as three fields of 10 bits (with 2 x 2
 * macrotiles all three are the first one's) */
static uint32_t
bounds(uint32_t t)
{
   return t << 22 | t << 12 | t;
}

void
sgx_rt_fill(const struct sgx_rt *rt, const uint32_t va[SGX_RT_NBUF], uint32_t *details,
            uint32_t *state)
{
   uint32_t xmax = rt->tiles_x - 1, ymax = rt->tiles_y - 1, mt_w = rt->mt_w, mt_h = rt->mt_h;
   uint32_t *s, *t, *r;

   memset(details, 0, rt->size[SGX_RT_DETAILS]);
   memset(state, 0, rt->size[SGX_RT_STATE]);

   /* the render details, each copy; copy 0's tail pointers run into copy
    * 1's first words, as the kext has them */
   details[0x08 / 4] = mt_w * rt->mt_across;
   details[0x1c / 4] = 2;
   for (unsigned k = 0; k < 2; k++) {
      uint32_t *e = details + k * COPY / 4;

      e[0x2c / 4] = mt_h * mt_w >> 4;
      e[0x30 / 4] = mt_h * mt_w * 12;
      e[0x34 / 4] = va[SGX_RT_DETAILS] + rt->first[k];
      e[0x58 / 4] = va[SGX_RT_STATE] + rt->state_clear;
      e[0x60 / 4] = va[SGX_RT_STATE] + rt->size[SGX_RT_STATE] - 0x100;
      e[0x5c / 4] = va[SGX_RT_CONTEXT] + rt->context[k];
      for (unsigned c = 0; c < rt->ncores; c++) {
         e[0x64 / 4 + c] = va[SGX_RT_CONTEXT] + rt->context[2 + c];
         e[0x74 / 4 + c] = va[SGX_RT_CONTEXT] + rt->context[2 + SGX_RT_MAX_CORES + c];
         e[0x84 / 4 + c] = va[SGX_RT_TAILS] + c * rt->tail_size;
      }
      e[0x94 / 4] = rt->tail_size;
      e[0x98 / 4] = va[SGX_RT_DETAILS] + k * COPY + 0x9c;
      e[0x9c / 4] = 0x10;
   }

   /* each macrotile's last region, as an offset into a region array (bit
    * 0 set: the macrotile is on the screen); the pipes' arrays and where
    * the last one ends; the last macrotile on the screen */
   for (unsigned k = 0; k < 2; k++) {
      uint32_t *e = details + k * COPY / 4, *first = details + rt->first[k] / 4, last = 0;

      for (uint32_t i = 0; i < rt->mt_across; i++) {
         uint32_t x = min(i * mt_w + mt_w - 1, xmax), xm = x % mt_w;

         for (uint32_t j = 0; j < rt->mt_down; j++) {
            uint32_t y = min(j * mt_h + mt_h - 1, ymax), ym = y % mt_h;
            uint32_t n = (y - (ym & 3)) * mt_w + ((ym & 3) << 2 | (xm & 3) | (xm & ~3u) << 2) +
                         mt_h * rt->mt_across * (x - xm);
            bool off = j * mt_h > ymax || i * mt_w > xmax;

            last = (n * 12 | off) ^ 1;
            first[i * rt->mt_down + j] = last;
         }
      }
      for (unsigned c = 0; c < rt->ncores; c++) {
         e[0x38 / 4 + c] = va[SGX_RT_REGIONS0 + k] + c * rt->region_size;
         e[0x48 / 4 + c] = va[SGX_RT_REGIONS0 + k] + c * rt->region_size + last;
      }
      for (uint32_t n = 0; n < rt->mt_across * rt->mt_down; n++)
         if (first[n] & 1)
            e[0x28 / 4] = n;
   }
   details[0x18 / 4] = rt->regions;
   details[0x14 / 4] = va[SGX_RT_STATE] + rt->state_regions;

   /* the state buffer: the clear's object (a triangle (0, 0), (w, 0),
    * (0, h) at depth 1, at half scale on big targets) */
   s = state + rt->state_clear / 4;
   s[0x0c / 4] = 0xffff0000;
   s[0x18 / 4] = 0xff00ff00;
   s[0x1c / 4] = ONE;
   s[0x24 / 4] = 0xff0000ff;
   s[0x2c / 4] = ONE;
   s[0x30 / 4] = 3;
   s[0x34 / 4] = 0x40000000;
   s[0x38 / 4] = 0x40004000;
   s[0x3c / 4] = ONE;
   s[0x40 / 4] = ((rt->big ? rt->w << 19 : rt->w << 20) + 0x40000000u) | 0x4000;
   s[0x44 / 4] = ONE;
   s[0x48 / 4] = ((rt->big ? rt->h << 3 : rt->h << 4) + 0x4000u) | 0x40000000;
   s[0x4c / 4] = ONE;

   /* a tile's object */
   t = state + rt->state_tile / 4;
   t[0x04 / 4] = 0x400000;
   t[0x14 / 4] = 0x820;
   t[0x20 / 4] = 0x3ff03ff0;
   t[0x24 / 4] = ONE;
   t[0x28 / 4] = 0x3fe03ff0;
   t[0x2c / 4] = ONE;
   t[0x30 / 4] = 0x3ff03fe0;
   t[0x34 / 4] = ONE;

   /* a stream entry for each region of each pipe, 64 bytes apart */
   r = state + rt->state_regions / 4;
   for (uint32_t n = 0; n < rt->regions * rt->ncores; n++, r += 16) {
      r[1] = 0x44400001;
      r[2] = ((0x00800010u + rt->state_tile + va[SGX_RT_STATE]) >> 2 & 0x1ffffff) | 0x4000000;
   }
}

void
sgx_rt_block3d(const struct sgx_rt *rt, const uint32_t va[SGX_RT_NBUF], uint32_t *blk)
{
   assert(va[SGX_RT_STATE] - SGX_RT_TA_BASE < (1u << 28));
   blk[0x28 / 4] = rt->big;
   for (unsigned c = 0; c < rt->ncores; c++)
      blk[0x2c / 4 + c] = va[SGX_RT_REGIONS0] + c * rt->region_size;
   blk[0x3c / 4] = rt->region_size;
   blk[0x40 / 4] = rt->mt_w * rt->mt_h;
   blk[0x44 / 4] = bounds(rt->mt_w);
   blk[0x48 / 4] = bounds(rt->mt_h);
   /* the state buffer from the TA's base, in 16 bytes; bits 29:24 are the
    * GL driver's */
   blk[0x88 / 4] = (blk[0x88 / 4] & 0x3f000000) | (va[SGX_RT_STATE] - SGX_RT_TA_BASE) >> 4;
   blk[0x10c / 4] = va[SGX_RT_DETAILS];
   blk[0x110 / 4] = va[SGX_RT_DETAILS] + 0x20;
}

void
sgx_rt_payload(const struct sgx_rt *rt, const uint32_t va[SGX_RT_NBUF], uint32_t blk_va,
               uint32_t *w)
{
   w[4] = bounds(rt->mt_w);
   w[5] = bounds(rt->mt_h);
   w[6] = (rt->tiles_y - 1) << 12 | (rt->tiles_x - 1);
   w[7] = rt->mt_w * rt->mt_h;
   w[8] = va[SGX_RT_TAILS];
   w[9] = rt->tail_size;
   w[10] = rt->tail_size * rt->ncores;
   w[11] = (0xfffff000u + (rt->h << 12)) | (rt->w - 1);
   w[21] = rt->w;
   w[22] = rt->h;
   w[27] = va[SGX_RT_REGIONS0];
   w[28] = rt->region_size;
   w[29] = rt->region_size * rt->ncores;
   w[51] = blk_va;
   w[52] = va[SGX_RT_DETAILS];
   w[53] = va[SGX_RT_DETAILS] + 0x20;
}

/* as 0x80bfca20 builds it (tools/sgx/rgen.py's ta_cmd()) */
void
sgx_rt_ta_cmd(const struct sgx_rt *rt, const uint32_t va[SGX_RT_NBUF], uint32_t blk_va,
              uint32_t *cmd)
{
   uint32_t w[54];

   sgx_rt_payload(rt, va, blk_va, w);
   cmd[0x50 / 4] = w[51];
   cmd[0x54 / 4] = w[52];
   cmd[0x58 / 4] = w[53];
   for (unsigned i = 0; i < 4; i++)
      cmd[0xc0 / 4 + i] = w[4 + i];
   for (unsigned c = 0; c < rt->ncores; c++) {
      cmd[0xdc / 4 + c] = w[27] + c * w[28];
      cmd[0xec / 4 + c] = w[8] + c * w[9];
   }
   cmd[0x104 / 4] = w[11];
   cmd[0x114 / 4] = rt->big;
}
