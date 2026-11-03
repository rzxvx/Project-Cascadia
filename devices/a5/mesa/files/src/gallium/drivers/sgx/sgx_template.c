/*
 * Copyright 2026 Project Cascadia
 * SPDX-License-Identifier: MIT
 *
 * The template frame, built at given addresses (docs/research/p105-mesa.md,
 * M17): tools/sgx/frame.py's gl_window(), vdm_stream() and payload(),
 * pds.py's program shapes, programs.py's code (as programs.py assembles it
 * with usse.py), rgen.py's ta_cmd(), and the GL driver's words of the 3D
 * register block as 0x80bf5eec copies them out of the render payload.
 * Words whose meaning is not known keep the values GL gives them, as in
 * frame.py.  mesa/host/tmpl-test.py builds all of it at the pack's
 * addresses and compares it with the pack.
 */
#include "sgx_template.h"

#include <string.h>

#define TA_BASE  0x87800000u

/* ---- PDS programs, by shape (pds.py) ------------------------------------ */

#define END            0xaf000000u
#define DOUTU_ONLY     0x185u        /* the program's only DOUT */
#define DOUTU_AFTER    0x1a5u        /* after DMA or attribute DOUTs */
#define DOUTU_TEX      0x1b5u        /* before the iterate and texture DOUTs that feed it */
#define DOUTU_VERTEX   0x1f5u        /* vertex programs (predicate 3) */
#define DMA_ROW0       0x07018113u   /* DMA: address and control word from data row 0 */
#define ITERATE_W3     0x07040c12u   /* iterate the coordinates, control: word 3 */
#define ITERATE_BG     0x07000c02u   /* the background object's iteration */
#define TEXTURE_ROW1   0x07041004u   /* texture fetch: the four state words of row 1 */
#define ATTR_ROW0      0x070181a6u   /* the state header's attribute DOUTs */
#define ATTR_ROW1      0x070581a6u
#define FETCH_INDEX    0x67800072u   /* vertex fetch: the index */

static uint32_t
pds_doutu(unsigned row, uint32_t kind, unsigned pred)
{
   return pred << 24 | row << 18 | kind;
}

/* a program: the data words padded to rows of four, then the code */
static unsigned
program(uint32_t *out, const uint32_t *data, unsigned ndata, const uint32_t *code,
        unsigned ncode)
{
   unsigned rows = (ndata + 3) & ~3u;

   memset(out, 0, rows * 4);
   memcpy(out, data, ndata * 4);
   memcpy(out + rows, code, ncode * 4);
   return rows + ncode;
}

/* DMA words from src into the USSE's inputs (ctl: their count less one and
 * where), then the program: state and constant loaders */
static void
dma_then_usse(uint32_t *out, uint32_t src, uint32_t ctl, uint32_t doutu, uint32_t temps)
{
   const uint32_t data[6] = { src, ctl, 0, 0, doutu, temps };
   const uint32_t code[3] = { DMA_ROW0, pds_doutu(1, DOUTU_AFTER, 7), END };

   program(out, data, 6, code, 3);
}

/* a pixel program's PDS: start it, iterate the texture coordinates, fetch
 * the texture (four state words) */
static void
textured(uint32_t *out, uint32_t doutu, uint32_t temps, uint32_t iterate, const uint32_t tex[4])
{
   const uint32_t data[8] = { doutu, temps, 0, iterate, tex[0], tex[1], tex[2], tex[3] };
   const uint32_t code[4] = { pds_doutu(0, DOUTU_TEX, 7), ITERATE_W3, TEXTURE_ROW1, END };

   program(out, data, 8, code, 4);
}

/* ---- USSE programs (programs.py) ---------------------------------------- */

#define PHAS        0xfa44070000000000ull   /* a program's start, single phase */
#define PHAS_NEXT   0xfa44000000000000ull   /* | the next phase's code index */

/* the PDS has loaded the state words into pa0..: the dummy load every
 * program that reads them starts with, the moves to o0.., out to the tiler */
static const uint64_t state_2[] = {
   PHAS, 0x488b0281a00c0000ull, 0xe9a30084a0000000ull, 0xf920000000000000ull,
   0x50811009a0000000ull, 0xfb274000a0200000ull,
};
static const uint64_t state_4[] = {
   PHAS, 0x488b0281a00c0000ull, 0xe9a30084a0000000ull, 0xf920000000000000ull,
   0x50813009a0000000ull, 0xfb274000a0200000ull,
};
static const uint64_t state_21[] = {
   PHAS, 0x488b0281a00c0000ull, 0xe9a30084a0000000ull, 0xf920000000000000ull,
   0x5081f009a0000000ull, 0x50814009a2000800ull, 0xfb274000a0200000ull,
};
static const uint64_t bg_reload[] = { PHAS, 0x50850009a0000000ull };   /* o0 = pa0, end */
static const uint64_t empty[] = { PHAS, 0xf804014000000000ull };       /* nop, end */
static const uint64_t done[] = { PHAS, 0xf834800000000000ull };        /* emit the event */
static const uint64_t eor[] = {                                         /* r0 = r1 = 0, emit */
   PHAS, 0xfca0000000000000ull, 0xfca0000000200000ull, 0xfb26000081200000ull,
};
/* two phases: the position completed (z 0, w 1.0 in pa10, pa11), then
 * position and attributes moved to the outputs and emitted */
static const uint64_t vertex[] = {
   PHAS_NEXT, 0x488b0281a00c0000ull, 0xe9a30084a0000000ull, 0xf920000000000000ull,
   0xfca0f1c201600000ull, 0xfca4000201400000ull,
   PHAS, 0xfa10000201010e01ull, 0x3880252183000080ull, 0xfa10000001010101ull,
   0x38801521830c0000ull, 0xfb275000a0200000ull,
};
#define VERTEX_PHASE2 6

unsigned
sgx_tmpl_program(enum sgx_tmpl_prog p, uint32_t at, uint64_t *code)
{
   static const struct { const uint64_t *code; unsigned n; } progs[SGX_TP_N] = {
#define P(name, a) [name] = { a, sizeof(a) / sizeof(a[0]) }
      P(SGX_TP_BG_RELOAD, bg_reload), P(SGX_TP_EMPTY_A, empty), P(SGX_TP_EMPTY_B, empty),
      P(SGX_TP_EMPTY_C, empty), P(SGX_TP_EMPTY_D, empty), P(SGX_TP_STATE_2, state_2),
      P(SGX_TP_STATE_4, state_4), P(SGX_TP_STATE_21, state_21), P(SGX_TP_DONE, done),
      P(SGX_TP_EOR, eor), P(SGX_TP_VERTEX, vertex),
#undef P
   };

   memcpy(code, progs[p].code, progs[p].n * 8);
   if (p == SGX_TP_VERTEX)
      code[0] |= at + VERTEX_PHASE2;
   return progs[p].n;
}

uint32_t
sgx_tmpl_doutu(const struct sgx_tmpl *t, uint32_t va)
{
   return ((va - t->code_base) / 8) << 4 | 3;
}

/* ---- the frame (frame.py) ----------------------------------------------- */

static uint32_t
tag5(uint32_t va)            /* a PDS data pointer: bit 31 implied */
{
   return 0x10000000 | ((va >> 4) & 0x07ffffff);
}

static uint32_t
vdm4(uint32_t tag, uint32_t va)
{
   return tag << 28 | ((va >> 4) & 0x0fffffff);
}

static uint32_t
f32(float x)
{
   uint32_t u;

   memcpy(&u, &x, 4);
   return u;
}

static uint32_t
last_tile(unsigned n)
{
   return (n + 31) / 32 - 1;
}

/* the texture state of a pixel program block: iterate control, then
 * {format, 0x0c << 24 | log2 w << 16 | log2 h, address, 0} */
#define TEX_ITERATE     0x1fc01900u
#define TEX_FORMAT      0x03fe0000u
#define BG_ITERATE      0xf800u
#define TEMPS_TEXTURED  0xe
#define TEMPS_BG        0xa

void
sgx_tmpl_pds(const struct sgx_tmpl *t, const uint32_t event[SGX_TMPL_EVENT_WORDS],
             uint32_t *out)
{
   const uint32_t fb[4] = {
      /* the target, linear, as the 2D engine reads it */
      (t->fb_stride / 4 - 2) << 16 | 0x0e90, 0xcc000000 | (t->fb_w - 1) << 12 | (t->fb_h - 1),
      t->fb, 0x10000000,
   };
   const uint32_t tex0[4] = { TEX_FORMAT, 0x0c030003, t->tex0, 0 };
   const uint32_t tex1[4] = { TEX_FORMAT, 0x0c020002, t->tex1, 0 };
   const uint32_t bg[8] = {
      sgx_tmpl_doutu(t, t->prog[SGX_TP_BG_RELOAD]), TEMPS_BG, 0, BG_ITERATE,
      fb[0], fb[1], fb[2], fb[3],
   };
   const uint32_t bg_code[4] = { pds_doutu(0, DOUTU_ONLY, 7), ITERATE_BG, TEXTURE_ROW1, END };
   const uint32_t nothing = END;

   memset(out, 0, SGX_TMPL_PDS_SIZE);
   /* the 3D pass: the GL driver's event program, its three programs --
    * end of render, end of tile, the rest -- and word 16 filled in */
   memcpy(out, event, SGX_TMPL_EVENT_WORDS * 4);
   out[0] = sgx_tmpl_doutu(t, t->prog[SGX_TP_EOR]);
   out[2] = sgx_tmpl_doutu(t, t->eot);
   out[4] = sgx_tmpl_doutu(t, t->prog[SGX_TP_DONE]);
   out[16] = 0x30000;
   program(out + 0x100 / 4, NULL, 0, &nothing, 1);
   program(out + 0x120 / 4, bg, 8, bg_code, 4);
   textured(out + 0x160 / 4, sgx_tmpl_doutu(t, t->pixel), TEMPS_TEXTURED, TEX_ITERATE, tex0);
   dma_then_usse(out + 0x1c0 / 4, t->pds + 0x1a4, 4,
                 sgx_tmpl_doutu(t, t->prog[SGX_TP_EMPTY_C]), 2);
   textured(out + 0x200 / 4, sgx_tmpl_doutu(t, t->pixel), TEMPS_TEXTURED, TEX_ITERATE, tex1);
}

void
sgx_tmpl_state(const struct sgx_tmpl *t, uint32_t *out)
{
   /* the normalisation the constants' loaders DMA in: 2/255, 1/255,
    * 2/65535, 1/65535 */
   const uint32_t norm[4] = {
      f32(2.0f / 255), f32(1.0f / 255), f32(2.0f / 65535), f32(1.0f / 65535),
   };
   const uint32_t tiles = last_tile(t->w) << 16 | last_tile(t->h);
   const uint32_t header[10] = {
      0x2000, 0, 0, 0, tiles, 0x100, 0, 0, sgx_tmpl_doutu(t, t->prog[SGX_TP_STATE_2]), 0,
   };
   const uint32_t header_code[6] = {
      ATTR_ROW0, ATTR_ROW1, ATTR_ROW1, ATTR_ROW1, pds_doutu(2, DOUTU_AFTER, 7), END,
   };
   const uint32_t tail[4] = { sgx_tmpl_doutu(t, t->prog[SGX_TP_EMPTY_A]), 2, 0, 0 };
   const uint32_t tail_code[2] = { pds_doutu(0, DOUTU_ONLY, 7), END };
   /* draw 0's whole state, 21 words: ISP state A and B (compare ALWAYS, no
    * depth write, blending), the pixel program's loader, info and block,
    * the tile clip, the viewport, the varyings' words */
   const uint32_t full[21] = {
      0x0000dfc7, 0x03d00300, 0x0000f000, 0x0e000000, tag5(t->pds + 0x1c0), 0x0803e000,
      tag5(t->pds + 0x160), 0x80000000 | last_tile(t->w), last_tile(t->h),
      f32(t->w / 2.0f), f32(t->w / 2.0f), f32(t->h / 2.0f), f32(t->h / 2.0f),
      f32(0.5f), f32(0.5f), 0, 0x0a001000, f32(1e-5f), 0x00088000, 0x00000039, 0x00000003,
   };
   /* a later draw's: mask 0x40 (the pixel program only), words 4..6 */
   const uint32_t delta[4] = {
      0x40, tag5(t->pds + 0x1c0), 0x0803e000, tag5(t->pds + 0x200),
   };
   /* the vertex fetch: r g b a, u v, x y (f32), 32 bytes, at +0x1e0;
    * control = first primary attribute << 8 | words - 1 */
   const uint32_t vb = t->state + 0x1e0;
   const uint32_t fetch[16] = {
      vb, 0x003, 32, 0, vb + 0x10, 0x401, 0, 0, vb + 0x18, 0x801, 0, 0,
      sgx_tmpl_doutu(t, t->prog[SGX_TP_VERTEX]), 0, 0, 0,
   };
   const uint32_t fetch_code[6] = {
      FETCH_INDEX, 0x2f0091a3 | 1 << 16, 0x2f0091a3 | 5 << 16, 0x2f0091a3 | 9 << 16,
      pds_doutu(3, DOUTU_VERTEX, 3), END,
   };

   memset(out, 0, SGX_TMPL_STATE_SIZE);
   program(out, header, 10, header_code, 6);
   program(out + 0x60 / 4, tail, 4, tail_code, 2);
   memcpy(out + 0x78 / 4, norm, sizeof(norm));
   dma_then_usse(out + 0xa0 / 4, t->state + 0x78, 6,
                 sgx_tmpl_doutu(t, t->prog[SGX_TP_EMPTY_B]), 2);
   memcpy(out + 0xe0 / 4, full, sizeof(full));
   dma_then_usse(out + 0x140 / 4, t->state + 0xe0, 20,
                 sgx_tmpl_doutu(t, t->prog[SGX_TP_STATE_21]), 0);
   program(out + 0x180 / 4, fetch, 16, fetch_code, 6);
   memcpy(out + 0x360 / 4, norm, sizeof(norm));
   dma_then_usse(out + 0x380 / 4, t->state + 0x360, 6,
                 sgx_tmpl_doutu(t, t->prog[SGX_TP_EMPTY_D]), 2);
   memcpy(out + 0x3c0 / 4, delta, sizeof(delta));
   dma_then_usse(out + 0x3e0 / 4, t->state + 0x3c0, 3,
                 sgx_tmpl_doutu(t, t->prog[SGX_TP_STATE_4]), 0);
}

void
sgx_tmpl_tail(const struct sgx_tmpl *t, uint32_t tail[5])
{
   tail[0] = vdm4(4, t->state + 0x60);
   tail[1] = 0x0800e100;
   tail[2] = vdm4(6, t->state);
   tail[3] = 0x1a022201;
   tail[4] = 0xc0000000;
}

/* rgen.py's ta_cmd(): one TA, first and last, from frame.py's payload --
 * w18..w20 the background object, w37 and w38 GL's */
void
sgx_tmpl_ta_cmd(const struct sgx_tmpl *t, uint32_t vdm, uint32_t pb, uint32_t *c)
{
   memset(c, 0, SGX_TMPL_CMD_SIZE);
   c[0x00 / 4] = SGX_TMPL_CMD_SIZE;
   c[0x04 / 4] = 0x73;
   c[0x08 / 4] = (t->pds + 0x100 - 0x80000000u) >> 4;   /* its PDS program */
   c[0x0c / 4] = 0x0801e000;                           /* its info word */
   c[0x10 / 4] = tag5(t->pds + 0x120);                 /* its pixel program */
   c[0x14 / 4] = 0x0c000000;
   c[0x20 / 4] = 1;
   c[0x28 / 4] = pb;
   c[0x64 / 4] = 1;
   c[0xd0 / 4] = 0x400000;
   c[0xd4 / 4] = vdm;
   c[0xd8 / 4] = 0x1a2;
   c[0xfc / 4] = 0x1e3ce508;
   c[0x100 / 4] = 0x1e3ce508;
   c[0x10c / 4] = 0x7fffffff;
   c[0x110 / 4] = TA_BASE;
   c[0x118 / 4] = 0x88;
}

/* what 0x80bf5eec writes into the 3D register block from the payload GL
 * hands the kernel (w12..w17 the 3D pass's PDS program, w23, w24, w36 GL's;
 * w30..w34, w39, w42, w43 zero), and its constants */
void
sgx_tmpl_block3d(const struct sgx_tmpl *t, uint32_t *b)
{
   memset(b, 0, SGX_TMPL_BLOCK_SIZE);
   b[0x10 / 4] = TA_BASE;
   b[0x18 / 4] = 0x80;
   b[0x1c / 4] = 0xf0000;            /* 0xf0100 when w12 and w13 differ */
   b[0x24 / 4] = 0xee;
   b[0x50 / 4] = 1;
   b[0x54 / 4] = 0x322bcc77;         /* 1e-8 */
   b[0x58 / 4] = 0x322bcc77;
   b[0x80 / 4] = 0x3f800000;         /* the depth tiles start at (register 0x4b8) */
   b[0x84 / 4] = 0x200 | 0x100;      /* w23 | 0x100 */
   b[0x88 / 4] = 3 << 24;            /* w24 << 24; the state buffer is the render target's */
   b[0x8c / 4] = 0x88;               /* w36 */
   b[0x94 / 4] = 1;
   b[0x98 / 4] = t->pds;             /* w12: the 3D pass's pixel PDS program */
   b[0x9c / 4] = 5;                  /* w14: its data, in rows */
   b[0xa0 / 4] = 0x4000;             /* w16: its info */
   b[0xcc / 4] = 0x2000000;
   b[0xd4 / 4] = 0x2000000;
   b[0xd8 / 4] = 0x200;
   b[0xfc / 4] = t->pds;             /* w13: the event program */
   b[0x100 / 4] = 5;                 /* w15 */
   b[0x104 / 4] = 0x4000;            /* w17 */
   b[0x144 / 4] = 0x1800000;
}
