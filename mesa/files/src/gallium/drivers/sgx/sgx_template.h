/*
 * Copyright 2026 Project Cascadia
 * SPDX-License-Identifier: MIT
 *
 * The template frame, built (docs/research/p105-mesa.md, M17): what
 * tools/sgx/frame.py, pds.py and programs.py put in sgx2d's pack, at
 * addresses given -- the kernel's picks, so that every process has its
 * own.  No Mesa headers: the host test (mesa/host/tmpl-test.py) builds it
 * at the pack's addresses and compares it with the pack, byte for byte.
 */
#ifndef SGX_TEMPLATE_H
#define SGX_TEMPLATE_H

#include <stdint.h>

/* the USSE programs the frame runs (programs.py) */
enum sgx_tmpl_prog {
   SGX_TP_BG_RELOAD,   /* the background object: the tile as the target holds it */
   SGX_TP_EMPTY_A,     /* nothing, where a PDS program needs one: the stream's tail */
   SGX_TP_EMPTY_B,     /* ... the constants' loader */
   SGX_TP_EMPTY_C,     /* ... the pixel program's secondary loader */
   SGX_TP_EMPTY_D,     /* ... the later draws' constants */
   SGX_TP_STATE_2,     /* N state words to the tiler */
   SGX_TP_STATE_4,
   SGX_TP_STATE_21,
   SGX_TP_DONE,        /* the 3D pass's events */
   SGX_TP_EOR,
   SGX_TP_VERTEX,      /* the vertex shader of the frame's own vertices: r g b a u v x y */
   SGX_TP_N,
};
#define SGX_TMPL_PROG_MAX   12        /* instructions */
#define SGX_TMPL_EVENT_WORDS 42      /* the event program: 5 data rows, 22 instructions */

/* the PDS block (+0 event program, +0x100 nothing, +0x120 background, +0x160
 * and +0x200 pixel programs, +0x1c0 their secondary loader) and the state
 * area (+0 the stream's tail state, +0xa0 constants, +0xe0 draw 0's state,
 * +0x140 its program, +0x180 vertex fetch, +0x3c0 later draws' state) */
#define SGX_TMPL_PDS_SIZE    0x240
#define SGX_TMPL_STATE_SIZE  0x420
#define SGX_TMPL_IDX_COUNT   8192
#define SGX_TMPL_CMD_SIZE    0x120
#define SGX_TMPL_BLOCK_SIZE  0x160

struct sgx_tmpl {
   uint32_t code_base;              /* USE code base 3 */
   uint32_t prog[SGX_TP_N];         /* where each program is */
   uint32_t eot;                    /* the end-of-tile program the event program names */
   uint32_t pixel;                  /* the pixel program of the frame's pixel blocks */
   uint32_t pds, state, idx;        /* the PDS block, the state area, the index buffer */
   uint32_t tex0, tex1;             /* the pixel programs' textures */
   uint32_t fb, fb_w, fb_h, fb_stride;   /* the background's first target, pixels */
   unsigned w, h;                   /* the screen: the stream's tail covers its tiles */
};

/* a program's instructions (len returned), for it at code index 'at' (its
 * address less the code base, / 8): the vertex program names its second
 * phase */
unsigned sgx_tmpl_program(enum sgx_tmpl_prog p, uint32_t at, uint64_t *code);

/* the DOUTU word for code at va */
uint32_t sgx_tmpl_doutu(const struct sgx_tmpl *t, uint32_t va);

/* the PDS block, SGX_TMPL_PDS_SIZE bytes; event: the GL driver's event
 * program (build/firmware/gl-event.pds, from the IPSW) */
void sgx_tmpl_pds(const struct sgx_tmpl *t, const uint32_t event[SGX_TMPL_EVENT_WORDS],
                  uint32_t *out);
/* the state area, SGX_TMPL_STATE_SIZE bytes */
void sgx_tmpl_state(const struct sgx_tmpl *t, uint32_t *out);
/* the VDM stream's end: the tail state, then the terminate word */
void sgx_tmpl_tail(const struct sgx_tmpl *t, uint32_t tail[5]);
/* the TA command, SGX_TMPL_CMD_SIZE bytes, the render target's words left
 * for sgx_rt_ta_cmd() */
void sgx_tmpl_ta_cmd(const struct sgx_tmpl *t, uint32_t vdm, uint32_t pb, uint32_t *cmd);
/* the 3D register block's GL words, SGX_TMPL_BLOCK_SIZE bytes, the render
 * target's left for sgx_rt_block3d() */
void sgx_tmpl_block3d(const struct sgx_tmpl *t, uint32_t *blk);

#endif
