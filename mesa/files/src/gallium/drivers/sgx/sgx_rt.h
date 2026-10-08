/*
 * Copyright 2026 Project Cascadia
 * SPDX-License-Identifier: MIT
 *
 * Render target data (docs/research/p105-mesa.md, M10): what the kext
 * computes for a render target of a size, in C -- the buffers the TA and
 * the 3D pass keep their state in, what the CPU writes into them, and the
 * words of the 3D register block and the TA command that point at them.
 * No Mesa headers: the host test (mesa/host/rt-test.py) builds it alone.
 */
#ifndef SGX_RT_H
#define SGX_RT_H

#include <stdbool.h>
#include <stdint.h>

/* the TA's requests are addressed from here: the state buffer has to be
 * within 256 MiB above it (the 3D block's +0x88) */
#define SGX_RT_TA_BASE   0x87800000u
#define SGX_RT_MAX_CORES 4
#define SGX_RT_MAX_SIZE  4096

/* the buffers, in the kext's order of allocation */
enum sgx_rt_buf {
   SGX_RT_CONTEXT,    /* areas the microkernel keeps, two per copy and two per core */
   SGX_RT_REGIONS0,   /* the region arrays, one per core, for copy 0 */
   SGX_RT_REGIONS1,   /* ... and for copy 1 */
   SGX_RT_TAILS,      /* the tail pointers, per core */
   SGX_RT_DETAILS,    /* the render details: two copies, then each one's first regions */
   SGX_RT_STATE,      /* the state buffer: the clear's objects, a stream entry per region */
   SGX_RT_NBUF,
};

struct sgx_rt {
   uint32_t w, h, ncores;
   uint32_t tiles_x, tiles_y;        /* 32 x 32 pixels */
   bool big;                         /* a side over 2048 */
   uint32_t mt_w, mt_h;              /* a macrotile's tiles, multiples of 4 */
   uint32_t mt_across, mt_down;      /* macrotiles: 2 x 2 */
   uint32_t regions;                 /* the macrotiles' tiles, padded */
   uint32_t region_size, tail_size;  /* bytes per core */
   uint32_t context[2 + 2 * SGX_RT_MAX_CORES];   /* offsets in SGX_RT_CONTEXT */
   uint32_t first[2];                /* each copy's first regions in the details */
   uint32_t state_clear, state_tile, state_regions;   /* offsets in SGX_RT_STATE */
   uint32_t size[SGX_RT_NBUF];       /* bytes, as the kext asks for them */
};

/* the layout for a W x H target, one sample, NCORES pixel pipes */
bool sgx_rt_layout(struct sgx_rt *rt, unsigned w, unsigned h, unsigned ncores);

/* what the CPU writes: the render details and the state buffer, whole
 * (SGX_RT_DETAILS and SGX_RT_STATE bytes), for the buffers at VA; the
 * others start as zeros */
void sgx_rt_fill(const struct sgx_rt *rt, const uint32_t va[SGX_RT_NBUF], uint32_t *details,
                 uint32_t *state);

/* the render target's words in a render's 3D register block (the rest are
 * the GL driver's) */
void sgx_rt_block3d(const struct sgx_rt *rt, const uint32_t va[SGX_RT_NBUF], uint32_t *blk);

/* the words of iOS's render payload the kext fills in (w[n] is word n),
 * with the 3D block at BLK_VA */
void sgx_rt_payload(const struct sgx_rt *rt, const uint32_t va[SGX_RT_NBUF], uint32_t blk_va,
                    uint32_t *w);

/* and the TA command's words that come from them */
void sgx_rt_ta_cmd(const struct sgx_rt *rt, const uint32_t va[SGX_RT_NBUF], uint32_t blk_va,
                   uint32_t *cmd);

#endif
