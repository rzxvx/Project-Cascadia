/*
 * Copyright 2026 Project Cascadia
 * SPDX-License-Identifier: MIT
 *
 * One full-screen quad through sgx2d's template frame.  How the pieces were
 * found is docs/research/p105-gpu.md (M4-M8); the frame is sgx2d's
 * (tools/sgx/lib/sgx2d.c), cut down to one draw, with two programs of our
 * own:
 *
 *  - the end-of-tile program, which loads the pixel back end's six state
 *    words and emits: in the pack it writes the framebuffer, here one is
 *    written for each render target (format word 0x00110000 = linear,
 *    B8G8R8A8 from the shader's RGBA; the address; the line stride in
 *    pixels / 2 - 1; 0; 0; the size);
 *  - a pixel program that replaces the tile's colour instead of blending
 *    with it: the pack's texel x colour (SOP2M, into pa0), then o0 = pa0,
 *    which is the pack's background program's own instruction.
 *
 * Draws have a vertex side of our own (M13b): a vertex fetch PDS program
 * and a vertex program for each number of varyings, iOS's shape (the
 * corpus's v* cases), and the TA state words that describe the varyings.
 *
 * They go in the pack's code page, in the 1 KiB below its first program,
 * where the pack's own programs are known to run.  The 3D pass's event
 * program names the end-of-tile program in its data (word 2), and the
 * background object reloads every tile from a linear descriptor (the 3D
 * PDS block +0x130): both are pointed at the render target before each
 * render.
 *
 * Render targets of any size (M10): the render target data -- what the
 * kext computes for a size, sgx_rt.c -- are ours, one set for each size a
 * render goes to (rt_set()), and the words that hold the size are written
 * for each render: the TA command's, the state's tile bounds and viewport,
 * the stream end's tiles.  The pack's own set, for the screen's size, is
 * left unused (SGX_FRAME=packrt takes it).
 *
 * SGX_FRAME swaps pieces for the pack's, or moves them, to find on the
 * device which one is wrong when a clear is (sgx_frame.h).
 */
#include "sgx_frame.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "drm-uapi/apple_sgx_drm.h"
#include "util/log.h"
#include "util/os_time.h"
#include "util/u_math.h"
#include "util/u_memory.h"
#include "util/vma.h"

#include "sgx_compiler.h"
#include "sgx_device.h"
#include "sgx_resource.h"
#include "sgx_rt.h"
#include "sgx_template.h"

/* the EXT window, as sgx2d uses it */
#define EXT_TEX_END     0x280000
#define EXT_VB          0x280000
#define EXT_FRAME       0x3c0000
#define PACK_VTX_FLOATS 8       /* the pack's vertex: r g b a u v x y */

#define MAX_PACK_BOS    48

/* our programs: the replace program, the iterated-colour pixel programs
 * (F16 and F32), the vertex programs (one for each number of varyings,
 * 0..8: 52 instructions), then one end-of-tile program (11) a slot */
#define ITER_PROG       0x20
#define ITER_PROG_F32   0x30
#define VERTEX_PROGS    0x40
#define PROGS_SIZE      0x1e0
#define EOT_SLOT        0x58
#define MAX_EOT         256
#define CODE_BO_SIZE    (PROGS_SIZE + MAX_EOT * EOT_SLOT)
/* in the EXT window after the white texture's block: the iterated-colour
 * pixel program's PDS, one for each varying (F16, F32), and the vertex
 * fetch, one for each number of varyings */
#define EXT_ITER_PDS    0x100
#define ITER_PDS_SIZE   0x20
#define EXT_FETCH_PDS   0x400
#define FETCH_PDS_SIZE  0x100
/* our pixel programs' PDS programs (M13c), one slot each, never reused */
#define EXT_PROG_PDS    0x1000
#define EXT_PROG_PDS_END 0x80000
#define PROG_PDS_SLOT   0x80
/* per render, in the frame's part of the EXT window: the secondary
 * attributes' loader and the words it loads (uniforms, texture states) */
#define FRAME_UNI_PDS   0xf00
#define FRAME_UNIFORMS  0x1000
/* a draw's whole state: the words the state program DMAs to the tiler
 * (its control word says 21 -- the template's block is 20 long, and the
 * 21st, which varyings are F16, came from whatever followed: zeros in a
 * buffer of its own, M27) */
#define STATE_WORDS     21
/* a gathered render's draws (sgx_frame_render), in its own memory (M27):
 * the whole state, then its state program */
#define DRAW_PROG       0x80
/* the uniforms' loader after the words */
#define UNIFORM_LOADER_AT(nsa) align(4 * (nsa), 16)
/* M27: a render's own memory comes in chunks of at least this */
#define ARENA_CHUNK     (256u << 10)
#define ARENA_MAX       128
/* M27: the gathered renders queued at most, the oldest waited for before
 * another goes (the CPU ahead of the GPU by that much, no more) */
#define MAX_QUEUED      3
/* the code of our programs: a buffer of its own in the code zone, a
 * program's place free again once the renders that ran it are done */
#define HEAP_SIZE       (256 << 10)

/* A retired program's places, free once fence (the last render when it
 * was retired) is done */
struct retired {
   uint32_t code_va, code_size, pds_va, pds_size;
   struct sgx_fence *fence;
};

/* the pack's code page is at code base + 0x1000 (rpack.py), its first
 * program at +0x400 (programs.py) */
#define PAGE_OFFSET     0x1000
#define PAGE_FREE       0x400

/* A DOUTU (and a PHAS) names a program by its index from the code base:
 * 20 bits of 8-byte instructions, 8 MiB.  Kernels before 2026-10-04 made
 * the code zone 16 MiB and gave out its top first, out of reach; with
 * SGX_FRAME=codebo the buffer goes at an address of our own choosing, just
 * above the pack's. */
#define CODE_REACH      (8u << 20)

/* the GL window's 3D PDS block, where frame.py puts it */
#define PDS_DEFAULT     0x98956000u

/* The render target data for each size a render goes to (M10, sgx_rt.c):
 * a buffer each, in a slot of the TA's heap -- the state buffer has to be
 * within 256 MiB above its base, and the kernel picks addresses far above
 * -- clear of the pack's windows; the least recently used is dropped when
 * the slots run out.  4 MiB holds a 4096 x 4096 target's. */
#define RT_SLOT_VA      0x89000000u
#define RT_SLOT_SIZE    (4u << 20)
#define MAX_RT_SETS     8
/* the pack's 3D register block, copied for each size */
#define BLOCK_SIZE      0x1000

enum { T_FULL, T_FULLPROG, T_DELTA, T_DELTAPROG, T_FETCH, T_TEX, T_NUM };
static const char *tmpl_names[T_NUM] = {
   "tmpl_full", "tmpl_fullprog", "tmpl_delta", "tmpl_deltaprog", "tmpl_fetch", "tmpl_tex",
};

/* USSE words, from tools/sgx/usse.py (assembled byte-identical to iOS's) */
#define USSE_PHAS          0xfa44070000000000ull
#define USSE_EMIT_PIXEL    0xfb24000003200082ull
#define USSE_SOP2M_MOD     0x90807982a0000140ull   /* r = texel * vertex colour */
#define USSE_SOP2_REPLACE  0x8184080190000000ull   /* o0 = pa0 * (1 - 0) + o0 * 0, end */
#define USSE_MOV_O0_PA0    0x50850009a0000000ull   /* o0 = pa0, end (the pack's bg_reload) */
#define USSE_PCK_O0_PA0    0x40850a3da01d8000ull   /* pck.u8.f16 o0, pa0 scale, end (iOS's) */
#define USSE_PCK_O0_PA0_F32 0x40840c3da01d8002ull  /* pck.u8.f32 o0, pa0, pa2 scale, end */
#define USSE_SMLSI_INC1    0xfa10000001010101ull   /* repeats: every register + 1 */
#define USSE_EMIT_VERTEX   0xfb275000a0200000ull   /* o0.. to the tiler, end */

/* The pixel side iOS's GL driver builds for gl_FragColor = v, a mediump
 * vec4 varying (docs/research/p105-mesa.md, M11): a PDS program that
 * starts the pixel program (doutu row 0) after iterating the varying into
 * pa0..pa1 as F16 (07040c12: two registers, control word 3), and the
 * control word: F16 (29:28 = 2), the last iterate (25), four components
 * (23:22 = 3), varying 1 (15:12) -- the pack's vertex program writes
 * (u, v) as varying 0, the colour as varying 1. */
#define PDS_DOUTU_ROW0_ITER  0x070001b5u
#define PDS_ITERATE_2REG_W3  0x07040c12u
#define PDS_ITERATE_4REG_W3  0x07040c32u
#define PDS_END              0xaf000000u
#define ITERATE_F16_VEC4     0x2fc0000fu   /* | varying << 12 */
#define ITERATE_F32_VEC4     0x0fc0000fu
#define ITERATE_F32_VEC4_MORE 0x0dc0000fu  /* not the last iterate (bit 25 clear) */
#define PDS_DMA_ROW0         0x07018113u   /* DMA: address and control in row 0 */
#define PDS_DOUTU_ROW1_AFTER 0x070401a5u   /* then the program in row 1 */

/* The iterate DOUT for the control word at data word d, into size (0x02,
 * 0x12, 0x32: one, two, four registers) -- the formula fits every one in
 * the corpus (v05's eight: 07040c12 ... 070c2b12); the registers follow
 * one another from pa0. */
static uint32_t
pds_iterate(unsigned d, unsigned size)
{
   return 0x07000000u | ((d + 2) >> 2) << 18 | d << 10 | ((d + 1) & 3) << 8 | size;
}
#define ITER_TEMPS           6
/* the pack's colour, for draws through its vertex side */
#define PACK_COLOUR_VARYING  1

/* The vertex fetch (pds.py's vertex_fetch(), iOS's for the v* cases): the
 * index, then attribute n from data row n -- the address, the first primary
 * attribute << 8 | words - 1, the stride (row 0's) -- then the vertex
 * program, its DOUTU in the row after (predicate 3). */
#define PDS_FETCH_INDEX      0x67800072u

static uint32_t
pds_fetch_attr(unsigned row)
{
   return 0x2f0091a3u | (4 * row + 1) << 16;
}

static uint32_t
pds_doutu_vertex(unsigned row)
{
   return 3u << 24 | row << 18 | 0x1f5;
}

/* The VDM word after the vertex fetch's address, as iOS's captures have
 * it: 0x03800202 for one vec4 attribute (logs/ios/depth), 0x05800403 for
 * two (tmpl/vcolor), 0x07800604 for three (mod, the pack's) -- read as the
 * attributes (bits 26:25), the primary attributes the vertex program gets
 * (11:7) and the fetch's data rows (6:0).  M13b; SGX_FETCH=word
 * overrides it. */
static uint32_t
vdm_fetch_word(unsigned nvaryings)
{
   static int64_t forced = -2;

   if (forced == -2)
      forced = getenv("SGX_FETCH") ? (int64_t)strtoul(getenv("SGX_FETCH"), NULL, 0) : -1;
   if (forced >= 0)
      return forced;
   return (1 + nvaryings) << 25 | 0x01800000u | (4 + 4 * nvaryings) << 7 | (nvaryings + 2);
}

/* The bits 31:28 of the VDM word with the vertex fetch's address: the
 * vertices a vertex task takes, less one -- as many as their outputs (the
 * position and nvaryings vec4s) fit in 128 words of the output buffer, 16
 * at most.  iOS has 15 for two vec4s, 9 for three, 2 for nine (the
 * corpus's v07, v04, v05); with more the vertices' outputs overlap, and
 * in some tiles whole triangles take other vertices' varyings (M30).
 * SGX_FETCH_TAG=n overrides it. */
static unsigned
vdm_fetch_tag(unsigned nvaryings)
{
   static int forced = -2;

   if (forced == -2)
      forced = getenv("SGX_FETCH_TAG") ? (int)strtoul(getenv("SGX_FETCH_TAG"), NULL, 0) : -1;
   return forced >= 0 ? forced : MIN2(16, 128 / (4 + 4 * nvaryings)) - 1;
}
static const uint64_t usse_dummy_load[3] = {
   0x488b0281a00c0000ull, 0xe9a30084a0000000ull, 0xf920000000000000ull,
};

/* VMOV.f32 o[dst..] <- pa[src..], two words a repeat (registers counted
 * in pairs: 3 is o6), up to four repeats (usse.py's vmov_f32) */
static uint64_t
usse_vmov_f32(unsigned dst, unsigned src, unsigned repeat)
{
   return 0x3880052183000000ull | (uint64_t)(repeat - 1) << 44 | (uint64_t)dst << 18 |
          (uint64_t)src << 6;
}

/* LIMM rN <- imm (bank 0, the temporaries) */
static uint64_t
usse_limm_r(unsigned reg, uint32_t imm)
{
   return 0xfca0000000000000ull | (uint64_t)(reg & 0x7f) << 21 |
          (uint64_t)((imm >> 26) & 0x3f) << 44 | (uint64_t)((imm >> 21) & 0x1f) << 36 |
          (imm & 0x1fffff);
}

struct sgx_eot {
   uint32_t va, w, h, stride;   /* the render target it writes */
};

struct rt_set {
   struct sgx_rt rt;
   struct sgx_bo *bo;           /* the buffers, then the 3D block */
   uint32_t va[SGX_RT_NBUF], blk_va;
   uint64_t used;
   struct sgx_fence *fence;     /* the last render that used it */
};

/* M27: a render's own memory -- its stream, the draws' state, uniforms,
 * fetches, vertices and indices, its copies of the frame's words a render
 * sets -- in chunks from the device's cache of buffer objects, which hands
 * them out again once the render is done (sgx_bo_set_busy; the kernel keeps
 * them for the render) */
struct arena {
   struct sgx_bo *bo[ARENA_MAX];
   unsigned n;
   uint32_t at;                 /* in the last chunk */
};

struct sgx_frame {
   struct sgx_device *dev;
   struct sgx_bo *bo[MAX_PACK_BOS];
   unsigned nbo;
   uint32_t handles[MAX_PACK_BOS + 3];

   uint32_t kick[3];            /* PB descriptor, render details, TA command */
   uint32_t w, h;
   uint32_t consts0, idx, idx_count, vdm, vdm_size, ext, ext_size, heap, heap_size, pds;
   unsigned max_draws;          /* in a gathered render */
   uint32_t fetch_tag, fetch_word;
   uint32_t tail[8];
   unsigned ntail;
   uint32_t term;               /* the tiles' bounds in the stream's terminate PDS data */
   struct rt_set rts[MAX_RT_SETS];
   uint64_t rt_clock;

   /* M17: the frame built here (sgx_template.c), in buffers where the
    * kernel puts them, rendered with the kernel's parameter buffer -- or the
    * pack's, at its addresses (SGX_FRAME=pack, kernels before UAPI 3) */
   bool built;
   uint32_t pack_pb;                              /* built, with the pack's parameter buffer */
   uint32_t cmd_tmpl[SGX_TMPL_CMD_SIZE / 4];      /* the TA command, render target aside */
   uint32_t blk_tmpl[SGX_TMPL_BLOCK_SIZE / 4];    /* the 3D block's GL words */
   uint8_t *tmpl;
   int toff[T_NUM], tsize[T_NUM];
   uint64_t code_base_expected;

   /* ours: the replace program at code_va, end-of-tile programs after it */
   struct sgx_bo *code;         /* their own buffer: SGX_FRAME=codebo only */
   uint32_t code_va;
   uint8_t *code_map;
   unsigned nhandles;           /* the pack's buffers (and ours); then the target */
   struct sgx_eot eot[MAX_EOT];
   struct sgx_fence *eot_fence[MAX_EOT];   /* the last render each slot's program ran in */
   unsigned neot, max_eot, eot_slot;       /* eot_slot: the render's */
   struct arena arena;
   struct sgx_fence *queued[MAX_QUEUED];   /* the last gathered renders, round in turn */
   unsigned nqueued;
   uint32_t texblock;           /* the white texture's block, replace program */
   uint32_t iter_pds;           /* the iterated-colour pixel program's PDS, per varying */
   uint32_t fetch_pds;          /* the vertex fetch, per number of varyings */
   struct sgx_bo *code_heap;    /* our programs' code */
   uint32_t empty_prog;
   /* where our programs' code (in code_heap) and PDS programs (in the EXT
    * window) go; a retired program's places, with the last render then,
    * are free again when it is done (struct retired) */
   struct util_vma_heap code_vma, pds_vma;
   struct retired *retired;
   unsigned nretired, maxretired;
   uint32_t vdmbuf[10 * SGX_FRAME_MAX_DRAWS + 32];   /* a gathered render's stream */
   uint32_t vertex_prog[SGX_FRAME_MAX_VARYINGS + 1];
   struct sgx_fence *last;      /* the last render through the frame */
   char *dir;                   /* the pack: loaded again after a render hangs */
   uint64_t timeouts;           /* the kernel's count of renders that did */
   bool vma_ready;
   bool debug;                  /* SGX_DEBUG=frame: say what each render is */
   bool debug_state;            /* SGX_DEBUG=state: each draw's state words */
   unsigned opts;               /* SGX_FRAME */
};

unsigned
sgx_frame_options(void)
{
   static const struct { const char *name; unsigned bit; } names[] = {
      { "fb", SGX_FRAME_FB }, { "blend", SGX_FRAME_BLEND },
      { "screen", SGX_FRAME_SCREEN }, { "codebo", SGX_FRAME_CODEBO },
      { "sop2", SGX_FRAME_SOP2 }, { "align", SGX_FRAME_ALIGN },
      { "packpixel", SGX_FRAME_PACKPIX }, { "packvertex", SGX_FRAME_PACKVTX },
      { "packrt", SGX_FRAME_PACKRT }, { "pack", SGX_FRAME_PACK }, { "built", SGX_FRAME_BUILT },
   };
   static int opts = -1;
   const char *env = getenv("SGX_FRAME");
   unsigned o = 0;

   if (opts >= 0)
      return opts;
   while (env && *env) {
      size_t n = strcspn(env, ",");
      unsigned i;

      for (i = 0; i < ARRAY_SIZE(names); i++)
         if (n == strlen(names[i].name) && !strncmp(env, names[i].name, n)) {
            o |= names[i].bit;
            break;
         }
      if (n && i == ARRAY_SIZE(names))
         mesa_logw("sgx: SGX_FRAME: no such switch \"%.*s\"", (int)n, env);
      env += n + (env[n] == ',');
   }
   opts = o;
   return o;
}

/* M27: a gathered render's own copies of the frame's words a render sets
 * -- the 3D pass's PDS block (the end of tile's DOUTU at +8, the
 * background's descriptor at +0x130) and the stream's end (the tiles at
 * +0x10) -- so it need not wait for the last render; the TA command, the
 * 3D block (a copy too) and the stream's tail point at them.
 * With the built frame only, and not for SGX_FRAME=fb, packrt (the
 * pack's). */
struct own {
   uint32_t pds, term;
};

/* n bytes of the render's own memory, at *va, aligned to al (a power of
 * two); NULL if there is none */
static uint8_t *
arena_alloc(struct sgx_frame *f, uint32_t n, uint32_t al, uint32_t *va)
{
   struct arena *a = &f->arena;
   struct sgx_bo *bo = a->n ? a->bo[a->n - 1] : NULL;
   uint32_t at = align(a->at, al);

   if (!bo || at + n > bo->size) {
      if (a->n == ARENA_MAX || !(bo = sgx_bo_cache_get(f->dev, MAX2(n, ARENA_CHUNK))))
         return NULL;
      a->bo[a->n++] = bo;
      at = 0;
   }
   a->at = at + n;
   *va = bo->va + at;
   return bo->map + at;
}

/* n bytes from p into the render's own memory; its address (0: none) */
static uint32_t
arena_put(struct sgx_frame *f, const void *p, uint32_t n, uint32_t al)
{
   uint32_t va;
   uint8_t *dst = arena_alloc(f, n, al, &va);

   if (!dst)
      return 0;
   memcpy(dst, p, n);
   return va;
}

/* the render's memory given back: the cache takes it again once the
 * render is done */
static void
arena_release(struct sgx_frame *f)
{
   for (unsigned i = 0; i < f->arena.n; i++)
      sgx_bo_destroy(f->arena.bo[i]);
   f->arena.n = f->arena.at = 0;
}

static uint32_t
p27(uint32_t a)                 /* a PDS data pointer */
{
   return 0x10000000 | ((a >> 4) & 0x07ffffff);
}

static uint32_t
vdm4(uint32_t tag, uint32_t a)
{
   return tag << 28 | a >> 4;
}


static uint32_t
doutu(struct sgx_frame *f, uint32_t va)  /* USE code base 3 */
{
   return ((va - f->dev->code_base) / 8) << 4 | 3;
}

/* State word 5's secondary attributes: their count - 1 in bits 21:13, a
 * multiple of 32 -- 31 in all of iOS's states (0x3e000); an sa past 32
 * read nothing with that, and 32 itself (33 registers) hangs the GPU
 * (M21).  (A vertex program's are in its VDM word, in fours.) */
static uint32_t
sa_field(unsigned nsa)
{
   return (align(MAX2(nsa, 1), 32) - 1) << 13;
}

/* State word 5 for a pixel program: the registers a pixel takes, primary
 * attributes and temporaries, in fours (bits 31:27); bits 26:23 as iOS has
 * them for 2, 3 and 4 fours, 12 / fours (M13b), but 1 past 12 -- 0 there
 * and the program reads 0 for its inputs (M30); its secondary attributes
 * (sa_field) */
static uint32_t
pixel_word5(const struct sgx_pixel_program *pix)
{
   unsigned fours = DIV_ROUND_UP(4 * pix->ninputs + pix->ntemps, 4);

   return fours << 27 | (fours > 1 ? MAX2(12 / fours, 1) : 0) << 23 | sa_field(pix->nsa);
}

/* A loader of n words (128 at most) at va into sa0..: a DMA, data row
 * {address, words - 1, 0, 0}, then the empty program -- iOS's form.  Its
 * words into out, its data rows returned.  (A pixel program has as many sa
 * as its state word 5 says: sa_field().) */
#define UNIFORM_LOADER_MAX 12

static unsigned
uniform_loader(struct sgx_frame *f, uint32_t va, unsigned n, uint32_t *out)
{
   const uint32_t loader[UNIFORM_LOADER_MAX] = {
      va, n - 1, 0, 0, doutu(f, f->empty_prog), 2, 0, 0,
      PDS_DMA_ROW0, PDS_DOUTU_ROW1_AFTER, PDS_END, 0,
   };

   memcpy(out, loader, sizeof(loader));
   return 2;
}

/* the stream's terminate PDS program (the tail's tag 6 word, in the
 * pack's buffers) holds the last tile across (31:16) and down (15:0) in
 * its data's +0x10 */
static uint32_t
term_tiles(unsigned w, unsigned h)
{
   return (DIV_ROUND_UP(w, 32) - 1) << 16 | (DIV_ROUND_UP(h, 32) - 1);
}

static uint8_t *
cpu_at(struct sgx_frame *f, uint32_t va, uint32_t n, unsigned *which)
{
   for (unsigned i = 0; i < f->nbo; i++) {
      struct sgx_bo *bo = f->bo[i];

      if (va >= bo->va && va - bo->va + n <= bo->size) {
         if (which)
            *which = i;
         return (uint8_t *)bo->map + (va - bo->va);
      }
   }
   return NULL;
}

static bool
put(struct sgx_frame *f, uint32_t va, const void *p, uint32_t n)
{
   uint8_t *dst = cpu_at(f, va, n, NULL);

   if (!dst)
      return false;
   memcpy(dst, p, n);
   return true;
}

static bool
load_file(const char *dir, const char *name, uint8_t **buf, size_t *len)
{
   char path[512];
   FILE *fp;
   long n;

   *buf = NULL;
   *len = 0;
   snprintf(path, sizeof(path), "%s/%s", dir, name);
   if (!(fp = fopen(path, "rb")))
      return false;
   fseek(fp, 0, SEEK_END);
   n = ftell(fp);
   fseek(fp, 0, SEEK_SET);
   *buf = malloc(n > 0 ? n : 1);
   *len = *buf ? fread(*buf, 1, n, fp) : 0;
   fclose(fp);
   return *buf && *len == (size_t)n;
}

static unsigned
parse_words(const char *p, uint32_t *out, unsigned max)
{
   unsigned n = 0;

   while (n < max && *p) {
      char *e;
      uint32_t v = strtoul(p, &e, 0);

      if (e == p)
         break;
      out[n++] = v;
      p = e;
   }
   return n;
}

static bool
map_window(struct sgx_frame *f, uint32_t va, uint32_t size)
{
   struct sgx_bo *bo;

   if (f->nbo == MAX_PACK_BOS)
      return false;
   bo = sgx_bo_create(f->dev, size, APPLE_SGX_BO_FIXED_VA, va);
   if (!bo && errno == EEXIST)
      mesa_logw("sgx: GPU 0x%08x is taken (another client of the template "
                "frame, or memory debugfs mapped there)", va);
   if (!bo || !sgx_bo_map(bo)) {
      sgx_bo_destroy(bo);
      return false;
   }
   f->handles[f->nbo] = bo->handle;
   f->bo[f->nbo++] = bo;
   return true;
}

/* the pack's contents ("img" lines) into its buffers: when the frame is
 * made, and again after a render hung (it leaves the parameter buffer and
 * the render target data half-used) */
static bool
load_images(struct sgx_frame *f)
{
   char path[512], line[512], key[64], a[64], b[64];
   bool ok = true;
   size_t len;
   FILE *fp;

   /* a built frame has nothing a render changes but the parameter buffer,
    * and that is the kernel's (which writes it again after a restart) --
    * or the pack's, SGX_FRAME=built on an older kernel */
   if (f->built && !f->pack_pb)
      return true;

   snprintf(path, sizeof(path), "%s/pack.txt", f->dir);
   if (!(fp = fopen(path, "r")))
      return false;
   while (ok && fgets(line, sizeof(line), fp)) {
      uint8_t *img = NULL;

      if (sscanf(line, "%63s %63s %63s", key, a, b) < 3 || strcmp(key, "img") ||
          (f->built && strtoul(a, 0, 0) != f->pack_pb))
         continue;
      ok = load_file(f->dir, b, &img, &len) && put(f, strtoul(a, 0, 0), img, len);
      free(img);
      if (!ok)
         mesa_logw("sgx: %s/%s is missing or larger than its buffer", f->dir, b);
   }
   fclose(fp);
   return ok;
}

/* pack.txt, the way sgx2d_open() reads it (two passes there: buffers before
 * the microkernel's boot, contents after; the render node needs no boot) */
static bool
load_pack(struct sgx_frame *f, const char *dir)
{
   char path[512], line[512], key[64], a[64], b[64];
   uint32_t poke_value = 0;
   bool ok = true;
   size_t len;
   FILE *fp;

   snprintf(path, sizeof(path), "%s/pack.txt", dir);
   if (!(fp = fopen(path, "r"))) {
      mesa_logw("sgx: no template frame at %s (./cascadia gpu installs it); "
                "clears go through the CPU", path);
      return false;
   }
   f->pds = PDS_DEFAULT;
   while (ok && fgets(line, sizeof(line), fp)) {
      int n = sscanf(line, "%63s %63s %63s", key, a, b);

      if (n < 2)
         continue;
      if (!strcmp(key, "map"))
         ok = map_window(f, strtoul(a, 0, 0), strtoul(b, 0, 0));
      else if (!strcmp(key, "kick"))
         parse_words(line + 4, f->kick, 3);
      else if (!strcmp(key, "screen")) {
         f->w = atoi(a);
         f->h = atoi(b);
      } else if (!strcmp(key, "consts0"))
         f->consts0 = strtoul(a, 0, 0);
      else if (!strcmp(key, "idx")) {
         f->idx = strtoul(a, 0, 0);
         f->idx_count = n == 3 ? strtoul(b, 0, 0) : 6;   /* indices 0, 1, ... */
      }
      else if (!strcmp(key, "vdm")) {
         f->vdm = strtoul(a, 0, 0);
         f->vdm_size = strtoul(b, 0, 0);
      } else if (!strcmp(key, "ext")) {
         f->ext = strtoul(a, 0, 0);
         f->ext_size = strtoul(b, 0, 0);
         f->max_draws = SGX_FRAME_MAX_DRAWS;
      } else if (!strcmp(key, "texheap")) {
         f->heap = strtoul(a, 0, 0);
         f->heap_size = strtoul(b, 0, 0);
      } else if (!strcmp(key, "pds"))
         f->pds = strtoul(a, 0, 0);
      else if (!strcmp(key, "fetch")) {
         f->fetch_tag = strtoul(a, 0, 0);
         f->fetch_word = strtoul(b, 0, 0);
      } else if (!strcmp(key, "tail"))
         f->ntail = parse_words(line + 4, f->tail, 8);
      else if (!strcmp(key, "poke") && n == 3)
         poke_value = strtoul(b, 0, 0);
      else
         for (unsigned i = 0; i < T_NUM; i++)
            if (!strcmp(key, tmpl_names[i])) {
               f->toff[i] = atoi(a);
               f->tsize[i] = atoi(b);
            }
   }
   if (ok && poke_value != f->dev->code_base >> 6) {
      mesa_logw("sgx: %s was built for code base 0x%x, the kernel's is 0x%x -- "
                "./cascadia gpu rebuilds it", dir, poke_value << 6, f->dev->code_base);
      ok = false;
   }
   fclose(fp);
   ok = ok && load_images(f);
   if (ok && !load_file(dir, "tmpl.bin", &f->tmpl, &len))
      ok = false;
   if (ok && (!f->w || !f->h || !f->vdm || !f->ext || !f->heap || !f->kick[2] ||
              !f->tsize[T_FULL] || !f->tsize[T_FETCH] || !f->tsize[T_TEX] ||
              f->tsize[T_TEX] > 64 || !cpu_at(f, f->pds, 0x140, NULL))) {
      mesa_logw("sgx: %s/pack.txt lacks pieces of the frame", dir);
      ok = false;
   }
   return ok;
}

/* ---- M17: the frame built here ----------------------------------------- */

/* the frame's own buffers, where the kernel puts them */
#define BUILT_PROG      0x80        /* a program a slot, in its code buffer */
#define BUILT_PDS       0x0         /* in the GL buffer: the PDS block, */
#define BUILT_STATE     0x400       /* the state area, */
#define BUILT_WHITE     0xc00       /* the white texture, */
#define BUILT_IDX       0x1000      /* the index buffer */
#define BUILT_GL_SIZE   (BUILT_IDX + SGX_TMPL_IDX_COUNT * 2)
#define BUILT_VDM_SIZE  0x1000      /* the template's own draws' stream */
#define BUILT_EXT_SIZE  0x400000    /* the pack's 4 MiB */

static struct sgx_bo *
new_bo(struct sgx_frame *f, uint32_t size, uint32_t flags)
{
   struct sgx_bo *bo;

   if (f->nbo == MAX_PACK_BOS)
      return NULL;
   bo = sgx_bo_create(f->dev, size, flags, 0);
   if (!bo || !sgx_bo_map(bo)) {
      sgx_bo_destroy(bo);
      return NULL;
   }
   memset(bo->map, 0, size);
   f->handles[f->nbo] = bo->handle;
   f->bo[f->nbo++] = bo;
   return bo;
}

/* The one piece from elsewhere: the GL driver's event program for the 3D
 * pass (pds.py's event_program(), from the IPSW), as the pack has it at the
 * start of its PDS block -- three programs and a word of it are ours. */
static bool
event_template(const char *dir, uint32_t ev[SGX_TMPL_EVENT_WORDS])
{
   char path[512], line[512], key[64], a[64], b[64];
   uint32_t pds = PDS_DEFAULT;
   bool ok = false;
   uint8_t *img;
   size_t len;
   FILE *fp;

   snprintf(path, sizeof(path), "%s/pack.txt", dir);
   if (!(fp = fopen(path, "r")))
      return false;
   while (fgets(line, sizeof(line), fp))
      if (sscanf(line, "%63s %63s", key, a) == 2 && !strcmp(key, "pds"))
         pds = strtoul(a, 0, 0);
   rewind(fp);
   while (!ok && fgets(line, sizeof(line), fp)) {
      if (sscanf(line, "%63s %63s %63s", key, a, b) < 3 || strcmp(key, "img") ||
          strtoul(a, 0, 0) != pds || !load_file(dir, b, &img, &len))
         continue;
      if (len >= SGX_TMPL_EVENT_WORDS * 4) {
         memcpy(ev, img, SGX_TMPL_EVENT_WORDS * 4);
         ok = true;
      }
      free(img);
   }
   fclose(fp);
   return ok;
}

/* SGX_FRAME=built on a kernel without a parameter buffer of its own: the
 * pack's, at its address (the first of pack.txt's kick words) */
static bool
pack_pb(struct sgx_frame *f, const char *dir)
{
   char path[512], line[512], key[64], a[64], b[64];
   uint32_t kick[3] = { 0 }, size = 0;
   FILE *fp;

   snprintf(path, sizeof(path), "%s/pack.txt", dir);
   if (!(fp = fopen(path, "r")))
      return false;
   while (fgets(line, sizeof(line), fp))
      if (!strncmp(line, "kick ", 5))
         parse_words(line + 4, kick, 3);
   rewind(fp);
   while (fgets(line, sizeof(line), fp))
      if (sscanf(line, "%63s %63s %63s", key, a, b) == 3 && !strcmp(key, "map") &&
          strtoul(a, 0, 0) == kick[0])
         size = strtoul(b, 0, 0);
   fclose(fp);
   if (!kick[0] || !size || !map_window(f, kick[0], size))
      return false;
   f->pack_pb = f->kick[0] = f->cmd_tmpl[0x28 / 4] = kick[0];
   return load_images(f);
}

static bool
build_frame(struct sgx_frame *f, const char *dir)
{
   struct sgx_device *dev = f->dev;
   uint32_t ev[SGX_TMPL_EVENT_WORDS], *pds, *state;
   struct sgx_bo *prog, *gl, *vdm, *ext;
   struct sgx_tmpl t = { 0 };
   static const struct { int which, off, size; } blocks[T_NUM] = {
      [T_FULL] = { 1, 0xe0, 0x50 }, [T_FULLPROG] = { 1, 0x140, 0x2c },
      [T_DELTA] = { 1, 0x3c0, 0x20 }, [T_DELTAPROG] = { 1, 0x3e0, 0x2c },
      [T_FETCH] = { 1, 0x180, 0x58 }, [T_TEX] = { 0, 0x160, 0x30 },
   };
   unsigned at = 0;

   f->built = true;
   if (!event_template(dir, ev)) {
      mesa_logw("sgx: no 3D event program in %s (./cascadia gpu installs it)", dir);
      return false;
   }
   if (!(prog = new_bo(f, 0x1000, APPLE_SGX_BO_USSE_CODE)) ||
       !(gl = new_bo(f, BUILT_GL_SIZE, 0)) || !(vdm = new_bo(f, BUILT_VDM_SIZE, 0)) ||
       !(ext = new_bo(f, BUILT_EXT_SIZE, 0)))
      return false;

   /* the programs, a slot each */
   t.code_base = dev->code_base;
   for (unsigned p = 0; p < SGX_TP_N; p++) {
      uint64_t code[SGX_TMPL_PROG_MAX];
      unsigned n;

      t.prog[p] = prog->va + p * BUILT_PROG;
      n = sgx_tmpl_program(p, (t.prog[p] - dev->code_base) / 8, code);
      memcpy(prog->map + p * BUILT_PROG, code, n * 8);
   }
   /* until a render says, the end of tile and the frame's pixel blocks do
    * nothing; the background reads the screen */
   t.eot = t.pixel = t.prog[SGX_TP_EMPTY_A];
   t.pds = gl->va + BUILT_PDS;
   t.state = gl->va + BUILT_STATE;
   t.idx = gl->va + BUILT_IDX;
   t.tex0 = t.tex1 = gl->va + BUILT_WHITE;
   t.fb = dev->fb_va ? dev->fb_va : 0x90000000u;
   t.fb_w = dev->fb_width ? dev->fb_width : 768;
   t.fb_h = dev->fb_height ? dev->fb_height : 1024;
   t.fb_stride = dev->fb_stride ? dev->fb_stride / 4 : t.fb_w;
   t.w = t.fb_w;
   t.h = t.fb_h;

   pds = (uint32_t *)(gl->map + BUILT_PDS);
   state = (uint32_t *)(gl->map + BUILT_STATE);
   sgx_tmpl_pds(&t, ev, pds);
   sgx_tmpl_state(&t, state);
   for (unsigned i = 0; i < SGX_TMPL_IDX_COUNT; i++)
      ((uint16_t *)(gl->map + BUILT_IDX))[i] = i;
   sgx_tmpl_tail(&t, f->tail);
   f->ntail = 5;
   sgx_tmpl_ta_cmd(&t, vdm->va, 0, f->cmd_tmpl);
   f->cmd_tmpl[0x28 / 4] = sgx_device_param(dev, APPLE_SGX_PARAM_PB_VA);
   /* SGX_PACK_PB=1: the pack's on any kernel (to tell the kernel's apart) */
   if ((dev->uapi < 3 || getenv("SGX_PACK_PB")) && !pack_pb(f, dir))
      return false;
   sgx_tmpl_block3d(&t, f->blk_tmpl);

   /* the blocks draws are made from */
   if (!(f->tmpl = CALLOC(T_NUM, 0x80)))
      return false;
   for (unsigned i = 0; i < T_NUM; i++) {
      f->toff[i] = at;
      f->tsize[i] = blocks[i].size;
      memcpy(f->tmpl + at, (blocks[i].which ? state : pds) + blocks[i].off / 4,
             blocks[i].size);
      at += 0x80;
   }

   f->w = t.w;
   f->h = t.h;
   f->kick[0] = f->pack_pb;
   f->kick[1] = f->kick[2] = 0;
   f->consts0 = t.state + 0xa0;
   f->idx = t.idx;
   f->idx_count = SGX_TMPL_IDX_COUNT;
   f->vdm = vdm->va;
   f->vdm_size = BUILT_VDM_SIZE;
   f->max_draws = SGX_FRAME_MAX_DRAWS;
   f->ext = ext->va;
   f->ext_size = BUILT_EXT_SIZE;
   f->heap = gl->va + BUILT_WHITE;
   f->heap_size = 0x400;
   f->pds = t.pds;
   f->fetch_tag = 9;
   f->fetch_word = 0x07800604;
   return true;
}

/* The buffer for our programs: the lowest free 64 KiB step above the
 * pack's windows in the code zone, within a DOUTU's reach */
static struct sgx_bo *
code_bo(struct sgx_frame *f, uint32_t size)
{
   struct sgx_device *dev = f->dev;
   uint32_t lo = MAX2(dev->code_va_start, dev->code_base);
   uint32_t hi = MIN2(dev->code_va_end, dev->code_base + CODE_REACH);

   /* a built frame takes the kernel's pick: the code zone is within reach */
   if (f->built) {
      struct sgx_bo *bo = sgx_bo_create(dev, size, APPLE_SGX_BO_USSE_CODE, 0);

      if (bo && sgx_bo_map(bo))
         return bo;
      sgx_bo_destroy(bo);
      return NULL;
   }
   for (uint32_t va = align(lo, 0x10000); va + size <= hi; va += 0x10000) {
      struct sgx_bo *bo;
      bool clear = true;

      /* a page of space around the pack's windows, as the kernel leaves */
      for (unsigned i = 0; i < f->nbo && clear; i++)
         clear = va + size + 4096 <= f->bo[i]->va ||
                 va >= f->bo[i]->va + f->bo[i]->size + 4096;
      if (!clear)
         continue;
      bo = sgx_bo_create(dev, size, APPLE_SGX_BO_USSE_CODE | APPLE_SGX_BO_FIXED_VA, va);
      if (bo && sgx_bo_map(bo))
         return bo;
      sgx_bo_destroy(bo);
      if (bo || errno != EEXIST)
         return NULL;
   }
   mesa_logw("sgx: no room for programs within 8 MiB of the code base 0x%08x",
             dev->code_base);
   return NULL;
}

/* The vertex program for n varyings: the position and the varyings, as
 * the vertex fetch put them in pa0.., to the outputs in the same order --
 * iOS's for gl_Position = p; v = a (the corpus's v00_vec4, there with the
 * attributes the other way round) -- then to the tiler */
static unsigned
vertex_program(uint64_t *p, unsigned nvaryings)
{
   unsigned n = 0, pairs = 2 * (1 + nvaryings);

   p[n++] = USSE_PHAS;
   p[n++] = USSE_SMLSI_INC1;
   for (unsigned i = 0; i < pairs; i += 4)
      p[n++] = usse_vmov_f32(i, i, MIN2(4, pairs - i));
   p[n++] = USSE_EMIT_VERTEX;
   return n;
}

/* the vertex fetch for n varyings: one attribute a vec4, from the vertices
 * at vb */
static unsigned
fetch_program(struct sgx_frame *f, uint32_t *p, unsigned nvaryings, uint32_t vb)
{
   unsigned n = 0, rows = 1 + nvaryings;

   for (unsigned i = 0; i < rows; i++) {
      p[n++] = vb + 16 * i;
      p[n++] = 4 * i << 8 | 3;
      p[n++] = i ? 0 : 16 * rows;
      p[n++] = 0;
   }
   p[n++] = doutu(f, f->vertex_prog[nvaryings]);
   p[n++] = 0;
   p[n++] = 0;
   p[n++] = 0;
   p[n++] = PDS_FETCH_INDEX;
   for (unsigned i = 0; i < rows; i++)
      p[n++] = pds_fetch_attr(i);
   p[n++] = pds_doutu_vertex(rows);
   p[n++] = PDS_END;
   return n;
}

static uint32_t
iter_pds_at(struct sgx_frame *f, unsigned varying, bool f32)
{
   return f->iter_pds + (2 * varying + f32) * ITER_PDS_SIZE;
}

/* Our pieces into the pack's buffers (again after load_images): the
 * replace program, a 4x4 white texture at the start of the texel heap, and
 * its block (word 0 the pixel program, 5 the size, log2, 6 the address);
 * the iterated-colour pixel programs and their PDS; the vertex programs
 * and their fetches.  End-of-tile programs are written as targets come. */
static bool
put_ours(struct sgx_frame *f)
{
   const uint64_t replace[3] = {
      USSE_PHAS, USSE_SOP2M_MOD,
      f->opts & SGX_FRAME_SOP2 ? USSE_SOP2_REPLACE : USSE_MOV_O0_PA0,
   };
   const uint64_t iterated[2] = { USSE_PHAS, USSE_PCK_O0_PA0 };
   const uint64_t iterated_f32[2] = { USSE_PHAS, USSE_PCK_O0_PA0_F32 };
   uint32_t white[16], blk[16], pds[FETCH_PDS_SIZE / 4];
   unsigned at = VERTEX_PROGS;
   bool ok;

   memcpy(f->code_map, replace, sizeof(replace));
   memcpy(f->code_map + ITER_PROG, iterated, sizeof(iterated));
   memcpy(f->code_map + ITER_PROG_F32, iterated_f32, sizeof(iterated_f32));
   for (unsigned n = 0; n <= SGX_FRAME_MAX_VARYINGS; n++) {
      uint64_t prog[8];
      unsigned len = vertex_program(prog, n);

      f->vertex_prog[n] = f->code_va + at;
      memcpy(f->code_map + at, prog, len * 8);
      at += len * 8;
   }
   assert(at <= PROGS_SIZE);
   f->neot = 0;
   memset(white, 0xff, sizeof(white));
   memcpy(blk, f->tmpl + f->toff[T_TEX], f->tsize[T_TEX]);
   if (!(f->opts & SGX_FRAME_BLEND))
      blk[0] = doutu(f, f->code_va);
   blk[5] = 0x0c000000 | 2 << 16 | 2;
   blk[6] = f->heap;
   f->texblock = f->ext;
   ok = put(f, f->heap, white, sizeof(white)) && put(f, f->texblock, blk, f->tsize[T_TEX]);

   f->iter_pds = f->ext + EXT_ITER_PDS;
   for (unsigned v = 0; v < SGX_FRAME_MAX_VARYINGS; v++) {
      for (unsigned f32 = 0; f32 < 2; f32++) {
         const uint32_t p[8] = {
            doutu(f, f->code_va + (f32 ? ITER_PROG_F32 : ITER_PROG)), ITER_TEMPS, 0,
            (f32 ? ITERATE_F32_VEC4 : ITERATE_F16_VEC4) | v << 12,
            PDS_DOUTU_ROW0_ITER, f32 ? PDS_ITERATE_4REG_W3 : PDS_ITERATE_2REG_W3, PDS_END, 0,
         };

         ok = ok && put(f, iter_pds_at(f, v, f32), p, sizeof(p));
      }
   }
   f->fetch_pds = f->ext + EXT_FETCH_PDS;
   for (unsigned n = 0; n <= SGX_FRAME_MAX_VARYINGS; n++) {
      unsigned len = fetch_program(f, pds, n, f->ext + EXT_VB);

      assert(len * 4 <= FETCH_PDS_SIZE);
      ok = ok && put(f, f->fetch_pds + n * FETCH_PDS_SIZE, pds, len * 4);
   }
   return ok;
}

struct sgx_frame *
sgx_frame_create(struct sgx_device *dev, const char *dir)
{
   struct sgx_frame *f = CALLOC_STRUCT(sgx_frame);
   uint32_t page;
   uint8_t *at;
   bool pack;

   if (!f)
      return NULL;
   f->dev = dev;
   f->debug = getenv("SGX_DEBUG") && strstr(getenv("SGX_DEBUG"), "frame");
   f->debug_state = getenv("SGX_DEBUG") && strstr(getenv("SGX_DEBUG"), "state");
   f->opts = sgx_frame_options();
   /* the frame built here, unless the kernel is too old for it (no
    * parameter buffer of its own, UAPI 3) or the pack's pieces are asked
    * for */
   pack = (dev->uapi < 3 && !(f->opts & SGX_FRAME_BUILT)) ||
          (f->opts & (SGX_FRAME_PACK | SGX_FRAME_FB | SGX_FRAME_BLEND | SGX_FRAME_PACKPIX |
                      SGX_FRAME_PACKRT));
   if (!(f->dir = strdup(dir)) || !(pack ? load_pack(f, dir) : build_frame(f, dir)))
      goto fail;
   f->timeouts = sgx_device_param(dev, APPLE_SGX_PARAM_RENDERS_TIMED_OUT);
   f->nhandles = f->nbo;

   /* where the stream's end holds the pack's tiles, for other sizes' */
   for (unsigned i = 0; i < f->ntail; i++) {
      uint32_t va = (f->tail[i] & 0x0fffffff) << 4;
      uint32_t *w = (uint32_t *)cpu_at(f, va + 0x10, 4, NULL);

      if (f->tail[i] >> 28 == 6 && w && *w == term_tiles(f->w, f->h))
         f->term = va + 0x10;
   }
   if (!f->term)
      mesa_logw("sgx: the pack's stream end has not got its tiles where they were "
                "found; renders at its size only");

   /* where our programs go: the free start of the pack's code page, or a
    * buffer of their own */
   page = dev->code_base + PAGE_OFFSET;
   at = f->built ? NULL : cpu_at(f, page, PAGE_FREE, NULL);
   if (!(f->opts & SGX_FRAME_CODEBO) && at) {
      for (unsigned i = 0; i < PAGE_FREE && at; i++)
         if (at[i])
            at = NULL;
      if (!at)
         mesa_logw("sgx: the pack's code page has something below its first "
                   "program; ours go in a buffer of their own");
   }
   if (!(f->opts & SGX_FRAME_CODEBO) && at) {
      f->code_va = page;
      f->code_map = at;
      f->max_eot = (PAGE_FREE - PROGS_SIZE) / EOT_SLOT;
   } else {
      if (!(f->code = code_bo(f, align(CODE_BO_SIZE, 4096))))
         goto fail;
      f->code_va = f->code->va;
      f->code_map = f->code->map;
      f->max_eot = MAX_EOT;
      f->handles[f->nhandles++] = f->code->handle;
   }
   if (!put_ours(f))
      goto fail;

   /* the code of our pixel programs, after an empty program (the uniform
    * loader's) */
   if (!(f->code_heap = code_bo(f, HEAP_SIZE)))
      goto fail;
   ((uint64_t *)f->code_heap->map)[0] = USSE_PHAS;
   ((uint64_t *)f->code_heap->map)[1] = 0xf804014000000000ull;     /* nop, end */
   f->empty_prog = f->code_heap->va;
   util_vma_heap_init(&f->code_vma, f->code_heap->va + 0x40, HEAP_SIZE - 0x40);
   f->code_vma.alloc_high = false;
   util_vma_heap_init(&f->pds_vma, f->ext + EXT_PROG_PDS, EXT_PROG_PDS_END - EXT_PROG_PDS);
   f->pds_vma.alloc_high = false;
   f->vma_ready = true;
   f->handles[f->nhandles++] = f->code_heap->handle;
   if (f->built)
      mesa_logi("sgx: template frame built at 0x%08x (PDS); the parameter buffer "
                "%s", f->pds, f->pack_pb ? "the pack's" : "the kernel's");
   else
      mesa_logi("sgx: template frame %ux%u from %s", f->w, f->h, dir);
   return f;

fail:
   sgx_frame_destroy(f);
   return NULL;
}

static void
drop_rt_sets(struct sgx_frame *f)
{
   for (unsigned i = 0; i < MAX_RT_SETS; i++) {
      sgx_fence_reference(&f->rts[i].fence, NULL);
      sgx_bo_destroy(f->rts[i].bo);
      f->rts[i].bo = NULL;
   }
}

void
sgx_frame_finish(struct sgx_frame *f)
{
   if (f && f->last)
      sgx_fence_wait(f->last, OS_TIMEOUT_INFINITE);
}

void
sgx_frame_destroy(struct sgx_frame *f)
{
   if (!f)
      return;
   if (f->last) {
      sgx_fence_wait(f->last, OS_TIMEOUT_INFINITE);
      sgx_fence_reference(&f->last, NULL);
   }
   for (unsigned i = 0; i < f->nretired; i++)
      sgx_fence_reference(&f->retired[i].fence, NULL);
   free(f->retired);
   if (f->vma_ready) {
      util_vma_heap_finish(&f->code_vma);
      util_vma_heap_finish(&f->pds_vma);
   }
   sgx_bo_destroy(f->code);
   sgx_bo_destroy(f->code_heap);
   drop_rt_sets(f);
   arena_release(f);
   for (unsigned i = 0; i < MAX_EOT; i++)
      sgx_fence_reference(&f->eot_fence[i], NULL);
   for (unsigned i = 0; i < MAX_QUEUED; i++)
      sgx_fence_reference(&f->queued[i], NULL);
   for (unsigned i = 0; i < f->nbo; i++)
      sgx_bo_destroy(f->bo[i]);
   free(f->tmpl);
   free(f->dir);
   FREE(f);
}

/* where a target's pixels are: its level's and layer's */
static struct sgx_eot
target_eot(const struct sgx_frame_target *t)
{
   const struct sgx_resource *r = t->res;

   return (struct sgx_eot){
      .va = r->bo->va + r->offset[t->level] + t->layer * r->layer_size[t->level],
      .w = u_minify(r->base.width0, t->level), .h = u_minify(r->base.height0, t->level),
      .stride = r->stride[t->level],
   };
}

bool
sgx_frame_can_render(struct sgx_frame *f, const struct sgx_frame_target *t)
{
   const struct pipe_resource *p = &t->res->base;
   struct sgx_eot to;

   if (!f || !t->res->bo || t->level > p->last_level || t->layer >= p->array_size ||
       (p->target != PIPE_TEXTURE_2D && p->target != PIPE_TEXTURE_CUBE) ||
       (p->format != PIPE_FORMAT_B8G8R8A8_UNORM && p->format != PIPE_FORMAT_B8G8R8X8_UNORM) ||
       p->nr_samples > 1)
      return false;
   to = target_eot(t);
   return to.w <= SGX_RT_MAX_SIZE && to.h <= SGX_RT_MAX_SIZE &&
          ((to.w == f->w && to.h == f->h) || (f->term && !(f->opts & SGX_FRAME_PACKRT))) &&
          !(to.va & 63) && !(to.stride & 15);
}

/* The render target data for a w x h target: made the first time a render
 * goes to that size -- the buffers as sgx_rt.c fills them, the pack's 3D
 * block with the size's words -- and kept.  A slot taken over: its buffers
 * stay with the renders that use them (the kernel's reference), but at a
 * fixed address (kernels before UAPI 3) the new ones go where they are --
 * once those renders are done. */
static struct rt_set *
rt_set(struct sgx_frame *f, unsigned w, unsigned h)
{
   struct rt_set *s = NULL;
   const uint32_t *cmd = f->built ? f->cmd_tmpl :
                         (const uint32_t *)cpu_at(f, f->kick[2], APPLE_SGX_TA_CMD_MIN, NULL);
   const uint8_t *blk = f->built ? (const uint8_t *)f->blk_tmpl :
                        cmd ? cpu_at(f, cmd[0x50 / 4], BLOCK_SIZE, NULL) : NULL;
   uint32_t va, size = 0;
   unsigned slot;

   for (unsigned i = 0; i < MAX_RT_SETS; i++) {
      struct rt_set *t = &f->rts[i];

      if (t->bo && t->rt.w == w && t->rt.h == h) {
         t->used = ++f->rt_clock;
         return t;
      }
      if (!s || (s->bo && (!t->bo || t->used < s->used)))
         s = t;
   }
   slot = s - f->rts;
   if (s->fence && f->dev->uapi < 3)
      sgx_fence_wait(s->fence, OS_TIMEOUT_INFINITE);
   sgx_fence_reference(&s->fence, NULL);
   sgx_bo_destroy(s->bo);
   s->bo = NULL;
   if (!blk || !sgx_rt_layout(&s->rt, w, h, MAX2(f->dev->num_cores, 1)))
      return NULL;
   for (unsigned i = 0; i < SGX_RT_NBUF; i++)
      size += align(s->rt.size[i], 4096);
   size += BLOCK_SIZE;
   if (size > RT_SLOT_SIZE)
      return NULL;
   /* a slot of our own in the TA's heap; the kernel's pick on a kernel
    * that has one (UAPI 3, M17) */
   va = RT_SLOT_VA + slot * RT_SLOT_SIZE;
   s->bo = f->dev->uapi >= 3 ?
           sgx_bo_create(f->dev, size, APPLE_SGX_BO_TA_HEAP, 0) :
           sgx_bo_create(f->dev, size, APPLE_SGX_BO_FIXED_VA, va);
   if (s->bo)
      va = s->bo->va;
   size = 0;
   for (unsigned i = 0; i < SGX_RT_NBUF; i++) {
      s->va[i] = va + size;
      size += align(s->rt.size[i], 4096);
   }
   s->blk_va = va + size;
   size += BLOCK_SIZE;
   if (!s->bo || !sgx_bo_map(s->bo)) {
      mesa_logw("sgx: no render target data for %ux%u at 0x%08x (%u bytes)", w, h, va, size);
      sgx_bo_destroy(s->bo);
      s->bo = NULL;
      return NULL;
   }
   sgx_rt_fill(&s->rt, s->va, (uint32_t *)(s->bo->map + (s->va[SGX_RT_DETAILS] - va)),
               (uint32_t *)(s->bo->map + (s->va[SGX_RT_STATE] - va)));
   memset(s->bo->map + (s->blk_va - va), 0, BLOCK_SIZE);
   memcpy(s->bo->map + (s->blk_va - va), blk, f->built ? SGX_TMPL_BLOCK_SIZE : BLOCK_SIZE);
   sgx_rt_block3d(&s->rt, s->va, (uint32_t *)(s->bo->map + (s->blk_va - va)));
   s->used = ++f->rt_clock;
   if (f->debug)
      mesa_logi("sgx: render target data for %ux%u at 0x%08x: details 0x%08x, 3D block "
                "0x%08x, %u bytes", w, h, va, s->va[SGX_RT_DETAILS], s->blk_va, size);
   return s;
}

/* The state words that hold the target's size: the tiles the TA bins into
 * (words 7 and 8: the last tile across and down) and the viewport (9..14:
 * translate and scale for x, y and z -- half the width twice, half the
 * height twice for the whole target, as iOS's captures of other sizes have
 * them, logs/ios/size; the order from glcull, M18) */
static void
state_size(uint32_t *full, unsigned w, unsigned h)
{
   float hw = w / 2.0f, hh = h / 2.0f;

   full[7] = (full[7] & ~0xfffu) | (DIV_ROUND_UP(w, 32) - 1);
   full[8] = (full[8] & ~0xfffu) | (DIV_ROUND_UP(h, 32) - 1);
   memcpy(&full[9], &hw, 4);
   memcpy(&full[10], &hw, 4);
   memcpy(&full[11], &hh, 4);
   memcpy(&full[12], &hh, 4);
}

uint32_t
sgx_frame_zls_size(unsigned w, unsigned h)
{
   return 2 * DIV_ROUND_UP(w, 64) * DIV_ROUND_UP(h, 32) * 4096;
}

/* ZLSCTL (register 0x480), from iOS's depth capture (0x0015100c, a store)
 * and flipping its bits (M23, M24): bit 2 stores the tiles' depth, 17 their
 * stencil (16 with it); 14 loads depth (1 with it), 13 stencil; the format
 * stored in bits 25:24, loaded in 22:21 -- 0 F32, 1 24-bit with stencil in
 * the top byte, 2 16-bit; bits 10:4 the rows of tiles in twos less 1 (tile
 * x, y at (x + y * 2 * (v + 1)) * 4 KiB); 12 and 18 needed.  Tiles a render
 * draws nothing in are neither loaded nor stored.  SGX_ZLS_CTL=value:
 * another (the extent put in). */
static uint32_t
zls_ctl(const struct sgx_frame_zls *zls)
{
   const char *e = getenv("SGX_ZLS_CTL");
   uint32_t ctl = 0x00151000;

   if (zls->stencil)
      ctl |= 0x01200000;
   if (zls->store)
      ctl |= 0xc | (zls->stencil ? 0x20000 : 0);
   if (zls->load)
      ctl |= 0x4002 | (zls->stencil ? 0x2000 : 0);
   if (e)
      ctl = strtoul(e, NULL, 0);
   return (ctl & ~(0x7fu << 4)) | (DIV_ROUND_UP(zls->w, 64) - 1) << 4;
}

/* The render: the pack's TA command with the target's render target data
 * (or the pack's own, SGX_FRAME=packrt), its parameter buffer, and the
 * buffers -- the frame's, the target, the render target data, what the
 * draws read besides */
static int
kick(struct sgx_frame *f, const struct sgx_frame_target *t, float depth,
     const struct sgx_frame_zls *zls, const uint32_t *handles, unsigned nhandles, uint32_t vdm,
     const struct own *o, struct sgx_fence *done)
{
   struct sgx_eot to = target_eot(t);
   uint32_t hs[MAX_PACK_BOS + 3 + 2 + 1 + SGX_FRAME_MAX_HANDLES + ARENA_MAX];
   uint32_t cmd[APPLE_SGX_TA_CMD_MAX / 4];
   const uint32_t *pack = f->built ? f->cmd_tmpl :
                          (const uint32_t *)cpu_at(f, f->kick[2], APPLE_SGX_TA_CMD_MIN, NULL);
   unsigned n = f->nhandles, det_bo;
   uint32_t det_handle, det_offset;
   struct rt_set *s = NULL;
   uint8_t *blk;
   int ret;

   if (!pack || pack[0] > sizeof(cmd) ||
       (!f->built && !cpu_at(f, f->kick[2], pack[0], NULL)) ||
       n + 3 + nhandles + f->arena.n > ARRAY_SIZE(hs))
      return -EINVAL;
   memcpy(cmd, pack, pack[0]);
   memcpy(hs, f->handles, n * sizeof(uint32_t));
   hs[n++] = t->res->bo->handle;
   if (f->opts & SGX_FRAME_PACKRT) {
      if (!cpu_at(f, f->kick[1], 0xa8, &det_bo))
         return -EFAULT;
      det_handle = f->bo[det_bo]->handle;
      det_offset = f->kick[1] - f->bo[det_bo]->va;
      blk = cpu_at(f, cmd[0x50 / 4] + 0x80, 4, NULL);
   } else {
      uint32_t blk_va;

      if (!(s = rt_set(f, to.w, to.h)))
         return -ENOMEM;
      blk_va = s->blk_va;
      blk = s->bo->map + (s->blk_va - s->bo->va);
      /* a gathered render's own copy of the 3D block, its event and pixel
       * PDS programs the render's (M27); the TA command's background
       * program and stream too */
      if (o) {
         uint8_t *b = arena_alloc(f, SGX_TMPL_BLOCK_SIZE, 64, &blk_va);

         if (!b)
            return -ENOMEM;
         memcpy(b, blk, SGX_TMPL_BLOCK_SIZE);
         blk = b;
         ((uint32_t *)blk)[0x98 / 4] = o->pds;
         ((uint32_t *)blk)[0xfc / 4] = o->pds;
      }
      sgx_rt_ta_cmd(&s->rt, s->va, blk_va, cmd);
      if (o) {
         cmd[0x08 / 4] = (o->pds + 0x100 - 0x80000000u) >> 4;
         cmd[0x10 / 4] = p27(o->pds + 0x120);
      }
      det_handle = hs[n++] = s->bo->handle;
      det_offset = s->va[SGX_RT_DETAILS] - s->bo->va;
      blk += 0x80;
   }
   /* the depth the tiles start at: register 0x4b8, the 3D block's +0x80
    * (1.0 as the kext sets it, M4) */
   if (!blk)
      return -EFAULT;
   memcpy(blk, &depth, 4);
   /* the depth buffer (M24), where the kext puts a GL depth attachment's
    * payload words (logs/ios/depth, rtemu.py): the ZLS base at +0x14 (1 MiB
    * aligned, BIF_ZLS_REQ_BASE), ZLSCTL at +0x6c, the load and store
    * offsets from the base at +0x70, +0x74 */
   {
      uint32_t *b = (uint32_t *)(blk - 0x80), base = zls ? zls->bo->va & ~0xfffffu : 0;

      b[0x14 / 4] = base;
      b[0x6c / 4] = zls ? zls_ctl(zls) : 0;
      b[0x70 / 4] = zls ? zls->bo->va - base : 0;
      b[0x74 / 4] = zls ? zls->bo->va - base : 0;
      if (zls)
         hs[n++] = zls->bo->handle;
   }
   /* the stream: the render's own (M27), or the frame's */
   if (vdm)
      cmd[0xd4 / 4] = vdm;
   /* SGX_CMD=off:xor[,...]: the TA command's words flipped (finding them) */
   {
      static uint32_t xo[APPLE_SGX_TA_CMD_MAX / 4];
      static int parsed;

      if (!parsed) {
         const char *e = getenv("SGX_CMD");

         parsed = 1;
         while (e && *e) {
            char *t;
            unsigned o = strtoul(e, &t, 0);

            if (*t != ':' || o >= APPLE_SGX_TA_CMD_MAX)
               break;
            xo[o / 4] ^= strtoul(t + 1, &t, 0);
            e = *t == ',' ? t + 1 : t;
         }
      }
      for (unsigned i = 0; i < cmd[0] / 4; i++)
         cmd[i] ^= xo[i];
   }
   /* SGX_BLK=off:xor[,...]: the 3D block's words flipped (finding them) */
   {
      static uint32_t xo[0x160 / 4];
      static int parsed;

      if (!parsed) {
         const char *e = getenv("SGX_BLK");

         parsed = 1;
         while (e && *e) {
            char *t;
            unsigned o = strtoul(e, &t, 0);

            if (*t != ':' || o >= 0x160)
               break;
            xo[o / 4] ^= strtoul(t + 1, &t, 0);
            e = *t == ',' ? t + 1 : t;
         }
      }
      for (unsigned i = 0; i < 0x160 / 4; i++)
         ((uint32_t *)(blk - 0x80))[i] ^= xo[i];
   }
   if (getenv("SGX_DEBUG") && strstr(getenv("SGX_DEBUG"), "cmd")) {
      for (unsigned i = 0; i < cmd[0] / 4; i++)
         fprintf(stderr, "%s%03x:%08x", i % 8 ? " " : "\nsgx: cmd ", 4 * i, cmd[i]);
      for (unsigned i = 0; i < 0x160 / 4; i++)
         fprintf(stderr, "%s%03x:%08x", i % 8 ? " " : "\nsgx: blk ", 4 * i,
                 ((uint32_t *)(blk - 0x80))[i]);
      fprintf(stderr, "\n");
   }
   memcpy(hs + n, handles, nhandles * sizeof(uint32_t));
   n += nhandles;
   for (unsigned i = 0; i < f->arena.n; i++)
      hs[n++] = f->arena.bo[i]->handle;
   if (f->debug)
      mesa_logi("sgx:   kick: PB 0x%08x, details 0x%08x (%s), 3D block 0x%08x, TA command "
                "%u bytes, %u buffers", f->kick[0], cmd[0x54 / 4],
                s ? "ours" : "the pack's", cmd[0x50 / 4], cmd[0], n);
   ret = sgx_submit(f->dev, cmd, f->kick[0], det_handle, det_offset, hs, n, done);
   if (!ret) {
      sgx_fence_reference(&f->last, done);
      sgx_fence_reference(&f->eot_fence[f->eot_slot], done);
      if (s)
         sgx_fence_reference(&s->fence, done);
   }
   return ret;
}

/* The end-of-tile program for a target: written once per target, in a
 * slot of its own (f->eot_slot says which, for the render's fence).  When
 * the slots run out (seven in the pack's page) the oldest is written over,
 * once the last render that ran it is done -- the USSE may still have the
 * old program in its cache. */
static uint32_t
eot_program(struct sgx_frame *f, const struct sgx_eot *want)
{
   const uint32_t pbe[6] = {
      0x00110000, want->va, want->stride / 4 / 2 - 1, 0, 0,
      (want->h - 1) << 12 | (want->w - 1),
   };
   uint64_t prog[11];
   unsigned i, slot;

   for (i = 0; i < MIN2(f->neot, f->max_eot); i++)
      if (!memcmp(&f->eot[i], want, sizeof(*want))) {
         f->eot_slot = i;
         return f->code_va + PROGS_SIZE + i * EOT_SLOT;
      }
   slot = f->neot++ % f->max_eot;
   if (f->neot == f->max_eot + 1)
      mesa_logw("sgx: more render targets than end-of-tile slots (%u); old ones are "
                "written over", f->max_eot);
   if (f->eot_fence[slot])
      sgx_fence_wait(f->eot_fence[slot], OS_TIMEOUT_INFINITE);
   f->eot[slot] = *want;
   f->eot_slot = slot;

   prog[0] = USSE_PHAS;
   memcpy(&prog[1], usse_dummy_load, sizeof(usse_dummy_load));
   for (i = 0; i < 6; i++)
      prog[4 + i] = usse_limm_r(i, pbe[i]);
   prog[10] = USSE_EMIT_PIXEL;
   memcpy(f->code_map + PROGS_SIZE + slot * EOT_SLOT, prog, sizeof(prog));
   return f->code_va + PROGS_SIZE + slot * EOT_SLOT;
}

/* two triangles over the whole target, the pack's vertices */
static void
quad(float *v, const float rgba[4])
{
   static const float corner[4][2] = { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 1 } };
   static const int order[6] = { 0, 1, 2, 0, 2, 3 };

   for (unsigned i = 0; i < 6; i++, v += PACK_VTX_FLOATS) {
      const float *c = corner[order[i]];

      memcpy(v, rgba, 4 * sizeof(float));
      v[4] = c[0];
      v[5] = c[1];
      v[6] = c[0] * 2 - 1;
      v[7] = c[1] * 2 - 1;
   }
}

static unsigned
max_vertices(struct sgx_frame *f, unsigned stride)
{
   unsigned n = MIN2(f->idx_count, (EXT_FRAME - EXT_VB) / stride);

   return n - n % 3;
}

unsigned
sgx_frame_max_vertices(struct sgx_frame *f, const struct sgx_frame_layout *l)
{
   /* the pack's vertices (SGX_FRAME=packvertex) are never larger */
   return max_vertices(f, sgx_frame_vertex_floats(l) * sizeof(float));
}

/* the retired programs' places whose renders are done (all of them:
 * after waiting for the last render) back into the heaps */
static void
reclaim(struct sgx_frame *f, bool all)
{
   unsigned n = 0;

   for (unsigned i = 0; i < f->nretired; i++) {
      struct retired *r = &f->retired[i];

      if (all || !r->fence || sgx_fence_wait(r->fence, 0)) {
         util_vma_heap_free(&f->code_vma, r->code_va, r->code_size);
         if (r->pds_size)
            util_vma_heap_free(&f->pds_vma, r->pds_va, r->pds_size);
         sgx_fence_reference(&r->fence, NULL);
      } else {
         f->retired[n++] = *r;
      }
   }
   f->nretired = n;
}

/* a program's code and PDS places, the retired ones' taken back first if
 * need be (waiting for the last render, at worst); false: no room */
static bool
place_program(struct sgx_frame *f, uint32_t code_size, uint32_t pds_size,
              uint32_t *code_va, uint32_t *pds_va)
{
   for (unsigned attempt = 0; attempt < 3; attempt++) {
      if (attempt == 1)
         reclaim(f, false);
      if (attempt == 2) {
         if (!f->nretired)
            break;
         if (f->last)
            sgx_fence_wait(f->last, OS_TIMEOUT_INFINITE);
         reclaim(f, true);
      }
      *code_va = util_vma_heap_alloc(&f->code_vma, code_size, 64);
      if (!*code_va)
         continue;
      *pds_va = pds_size ? util_vma_heap_alloc(&f->pds_vma, pds_size, PROG_PDS_SLOT) : 0;
      if (*pds_va || !pds_size)
         return true;
      util_vma_heap_free(&f->code_vma, *code_va, code_size);
   }
   return false;
}

static void
retire(struct sgx_frame *f, uint32_t code_va, uint32_t code_size, uint32_t pds_va,
       uint32_t pds_size)
{
   struct retired *r;

   if (f->nretired == f->maxretired) {
      unsigned max = MAX2(16, 2 * f->maxretired);

      if (!(r = realloc(f->retired, max * sizeof(*r)))) {
         /* nowhere to keep it: the places stay taken */
         mesa_logw_once("sgx: out of memory for a retired program: its place is lost");
         return;
      }
      f->retired = r;
      f->maxretired = max;
   }
   r = &f->retired[f->nretired++];
   *r = (struct retired){ code_va, code_size, pds_va, pds_size, NULL };
   sgx_fence_reference(&r->fence, f->last);
}

void
sgx_frame_retire(struct sgx_frame *f, struct sgx_pixel_program *p)
{
   if (!p->code_va)
      return;
   retire(f, p->code_va, align(p->ncode * 8, 64), p->pds_va, PROG_PDS_SLOT);
   p->code_va = p->pds_va = 0;
}

void
sgx_frame_retire_vs(struct sgx_frame *f, struct sgx_vs *vs)
{
   if (!vs->code_va)
      return;
   retire(f, vs->code_va, align(vs->ncode * 8, 64), 0, 0);
   vs->code_va = 0;
}

/* A pixel program into GPU memory, at its first draw: the code into the
 * heap, and its PDS program -- start the program, then iterate each input
 * (F32, four registers; iOS's shape, the corpus's v04 and v05) -- into the
 * EXT window, until it is retired (sgx_frame_retire). */
static int
upload(struct sgx_frame *f, struct sgx_pixel_program *p)
{
   uint32_t pds[PROG_PDS_SLOT / 4], size = p->ncode * 8, code_va, pds_va;
   unsigned n = 0, data;

   if (p->code_va)
      return 0;
   if (p->ninputs > SGX_FRAME_MAX_VARYINGS ||
       !place_program(f, align(size, 64), PROG_PDS_SLOT, &code_va, &pds_va)) {
      mesa_logw("sgx: no room for another pixel program");
      return -ENOSPC;
   }
   memcpy((uint8_t *)f->code_heap->map + (code_va - f->code_heap->va), p->code, size);
   p->code_va = code_va;

   pds[n++] = doutu(f, p->code_va);
   /* (bit 0 for a program that branches: iOS's c04_loop_break has 3 where
    * the rest have 2) */
   pds[n++] = (p->ninputs ? ITER_TEMPS : 2) | p->branches;
   pds[n++] = 0;
   for (unsigned i = 0; i < p->ninputs; i++)
      pds[n++] = (i == p->ninputs - 1 ? ITERATE_F32_VEC4 : ITERATE_F32_VEC4_MORE) |
                 p->iter_src[i] << 12;
   while (n % 4)
      pds[n++] = 0;
   data = n;
   pds[n++] = PDS_DOUTU_ROW0_ITER;
   for (unsigned i = 0; i < p->ninputs; i++)
      pds[n++] = pds_iterate(3 + i, 0x32);
   pds[n++] = PDS_END;
   p->pds_va = pds_va;
   p->pds_rows = data / 4;
   return put(f, p->pds_va, pds, n * 4) ? 0 : -EFAULT;
}

/* A vertex shader's code into GPU memory, at its first draw (its fetch is
 * each draw's: vs_fetch()) */
int
sgx_frame_upload_vs(struct sgx_frame *f, struct sgx_vs *vs)
{
   uint32_t size = vs->ncode * 8, code_va, pds_va;

   if (vs->code_va)
      return 0;
   if (vs->nattrs > SGX_VS_MAX_ATTRIBS ||
       !place_program(f, align(size, 64), 0, &code_va, &pds_va)) {
      mesa_logw("sgx: no room for another vertex program");
      return -ENOSPC;
   }
   memcpy((uint8_t *)f->code_heap->map + (code_va - f->code_heap->va), vs->code, size);
   vs->code_va = code_va;
   return 0;
}

/* A draw's vertex fetch (M26), into words[]: a DMA row for each attribute
 * the shader takes from memory -- {address of vertex 0's, first pa << 8 |
 * words - 1, the stride (row 0's only: one for the whole fetch), 0} --
 * then the program (DOUTU; bit 0 of its second word for a program that
 * branches: without it, a loop whose count differs between vertices came
 * out wrong, M22); then the index fetch, each row's, the program started
 * (iOS's shape, the corpus's x cases).  The DOUTU's temporaries are none:
 * the program's scratch is primary attributes after the vertex's, which the
 * VDM's fetch word counts in (vs_fetch_word()).  Returns its data rows. */
static unsigned
vs_fetch(struct sgx_frame *f, const struct sgx_frame_draw *d, uint32_t vb, uint32_t *words,
         unsigned *nwords)
{
   const struct sgx_vs *vs = d->vs;
   unsigned n = 0, rows = 0;

   for (unsigned a = 0; a < vs->nattrs; a++) {
      if (SGX_ATTR_KIND(vs->attr[a]) == SGX_ATTR_CONST)
         continue;
      words[n++] = vb + d->base[a];
      words[n++] = 4 * a << 8 | (sgx_attr_words(vs->attr[a]) - 1);
      words[n++] = rows ? 0 : d->stride;
      words[n++] = 0;
      rows++;
   }
   /* (no attribute from memory: a word read into pa0, which is attribute
    * 0's and never read) */
   if (!rows) {
      words[n++] = f->ext;
      words[n++] = 0;
      words[n++] = 0;
      words[n++] = 0;
      rows++;
   }
   words[n++] = doutu(f, vs->code_va);
   words[n++] = vs->branches;
   words[n++] = 0;
   words[n++] = 0;
   words[n++] = PDS_FETCH_INDEX;
   for (unsigned r = 0; r < rows; r++)
      words[n++] = pds_fetch_attr(r);
   words[n++] = pds_doutu_vertex(rows);
   words[n++] = PDS_END;
   *nwords = n;
   return rows + 1;
}

/* The VDM's word after a vertex shader's fetch: the vec4s the vertex hands
 * the tiler, the position and the varyings (bits 25 and up -- what M13b
 * read as attributes: with its copying programs they were as many), the
 * registers the program takes -- the attributes' four words each, then its
 * temporaries -- and the fetch's data rows */
static uint32_t
vs_fetch_word(const struct sgx_vs *vs, unsigned rows)
{
   unsigned regs = align(4 * vs->nattrs + vs->ntemps, 4);

   return (1 + vs->nvaryings) << 25 | 0x01800000u | regs << 7 | rows;
}

static int render(struct sgx_frame *f, struct sgx_resource *rt,
                  const struct sgx_frame_layout *l, const float *verts, unsigned nverts,
                  bool iterated, struct sgx_pixel_program *prog, const uint32_t *sa,
                  const uint32_t *handles, unsigned nhandles,
                  const struct sgx_frame_state *st, struct sgx_fence *done);

/* a clear: the pack's vertex and pixel sides, as M12 proved them */
int
sgx_frame_clear(struct sgx_frame *f, struct sgx_resource *rt, const float rgba[4],
                struct sgx_fence *done)
{
   float verts[6 * PACK_VTX_FLOATS];

   quad(verts, rgba);
   return render(f, rt, NULL, verts, 6, false, NULL, NULL, NULL, 0, NULL, done);
}

int
sgx_frame_draw(struct sgx_frame *f, struct sgx_resource *rt, const struct sgx_frame_layout *l,
               const float *verts, unsigned nverts, struct sgx_pixel_program *prog,
               const uint32_t *sa, const uint32_t *handles, unsigned nhandles,
               const struct sgx_frame_state *st, struct sgx_fence *done)
{
   unsigned vf = sgx_frame_vertex_floats(l);
   float *pack;
   int ret;

   if (l->nvaryings > SGX_FRAME_MAX_VARYINGS ||
       (prog ? l->nvaryings != prog->nvaryings : l->colour >= l->nvaryings))
      return -EINVAL;
   if (prog)
      return render(f, rt, l, verts, nverts, true, prog, sa, handles, nhandles, st, done);
   if (!(f->opts & (SGX_FRAME_PACKVTX | SGX_FRAME_PACKPIX)))
      return render(f, rt, l, verts, nverts, true, NULL, NULL, NULL, 0, st, done);

   /* through the pack's vertex side: r g b a (the colour) u v x y */
   if (!(pack = malloc(nverts * PACK_VTX_FLOATS * sizeof(float))))
      return -ENOMEM;
   for (unsigned i = 0; i < nverts; i++) {
      const float *in = verts + i * vf;
      float *out = pack + i * PACK_VTX_FLOATS;

      memcpy(out, in + 4 + 4 * l->colour, 4 * sizeof(float));
      out[4] = out[5] = 0;
      out[6] = in[0];
      out[7] = in[1];
   }
   ret = render(f, rt, NULL, pack, nverts, !(f->opts & SGX_FRAME_PACKPIX), NULL, NULL, NULL,
                0, st, done);
   free(pack);
   return ret;
}

static bool
own_copies(const struct sgx_frame *f)
{
   return f->built && f->term && !(f->opts & (SGX_FRAME_FB | SGX_FRAME_PACKRT));
}

/* What every render starts with: the pack again after a hang, and the end
 * of tile and background aimed at the target (or, with SGX_FRAME=screen,
 * the framebuffer) -- into the render's own copies (o), or into the
 * frame's words once the last render is done (the frame's buffers are its
 * until then). */
static int
begin_render(struct sgx_frame *f, const struct sgx_frame_target *t, struct sgx_eot *out_to,
             uint32_t *out_eot, uint32_t bg[4], struct own *o)
{
   struct sgx_eot to = target_eot(t);
   uint32_t eot, d0, tiles = term_tiles(to.w, to.h);
   uint64_t timeouts;

   if (!o) {
      sgx_trace_mark("begin_render wait");
      if (f->last && !sgx_fence_wait(f->last, 5ull * 1000 * 1000 * 1000))
         mesa_logw("sgx: the last render through the template frame is still running");
      sgx_trace_mark("begin_render waited");
   }

   /* a render that hung (this process's or another's) leaves the parameter
    * buffer and the render target data half-used: the pack again */
   timeouts = sgx_device_param(f->dev, APPLE_SGX_PARAM_RENDERS_TIMED_OUT);
   if (timeouts != f->timeouts) {
      mesa_logw("sgx: a render timed out (%llu so far); the template frame is "
                "loaded again", (unsigned long long)timeouts);
      f->timeouts = timeouts;
      if (f->last)
         sgx_fence_wait(f->last, 5ull * 1000 * 1000 * 1000);
      if (!load_images(f) || !put_ours(f))
         return -EFAULT;
      drop_rt_sets(f);
   }

   /* where tiles go, and what they start as: the render target (or, with
    * SGX_FRAME=screen, the framebuffer) */
   if (f->opts & SGX_FRAME_SCREEN) {
      /* where the pack's own end of tile writes, if the kernel does not say */
      to.va = f->dev->fb_va ? f->dev->fb_va : 0x90000000u;
      to.stride = f->dev->fb_stride ? f->dev->fb_stride : f->w * 4;
   }
   eot = eot_program(f, &to);
   bg[0] = (to.stride / 4 / 4 - 2) << 16 | 0x0e90;
   bg[1] = 0xcc000000 | (to.w - 1) << 12 | (to.h - 1);
   bg[2] = to.va;
   bg[3] = 0x10000000;
   d0 = doutu(f, eot);

   if (o) {
      const uint8_t *pds = cpu_at(f, f->pds, SGX_TMPL_PDS_SIZE, NULL);
      const uint8_t *term = cpu_at(f, f->term - 0x10, 0x60, NULL);
      uint32_t *p, *t;

      if (!pds || !term || !(p = (uint32_t *)arena_alloc(f, SGX_TMPL_PDS_SIZE, 64, &o->pds)) ||
          !(t = (uint32_t *)arena_alloc(f, 0x60, 64, &o->term)))
         return -ENOMEM;
      memcpy(p, pds, SGX_TMPL_PDS_SIZE);
      p[8 / 4] = d0;
      memcpy(p + 0x130 / 4, bg, 4 * sizeof(uint32_t));
      memcpy(t, term, 0x60);
      t[0x10 / 4] = tiles;
   } else {
      /* the tiles the stream's end covers: the target's */
      if (f->term && !put(f, f->term, &tiles, 4))
         return -EFAULT;
      if (!(f->opts & SGX_FRAME_FB) &&
          (!put(f, f->pds + 8, &d0, 4) || !put(f, f->pds + 0x130, bg, 4 * sizeof(uint32_t))))
         return -EFAULT;
   }

   *out_to = to;
   *out_eot = eot;
   return 0;
}

/* A render through the frame: with l, our vertex side (M13b) and verts
 * laid out as it says; without, the pack's, verts r g b a u v x y.  The
 * pixel side is the iterated colour (l's colour varying, or the pack's) or
 * the pack's texel x colour. */
static int
render(struct sgx_frame *f, struct sgx_resource *rt, const struct sgx_frame_layout *l,
       const float *verts, unsigned nverts, bool iterated, struct sgx_pixel_program *pix,
       const uint32_t *sa, const uint32_t *handles, unsigned nhandles,
       const struct sgx_frame_state *st, struct sgx_fence *done)
{
   const struct sgx_frame_target t = { rt, 0, 0 };
   uint32_t vdm[32] = { 0 }, full[32], prog[16], fetch[32], bg[4], *v = vdm;
   uint32_t frame = f->ext + EXT_FRAME, vb = f->ext + EXT_VB, d0, p0, fb, eot, fetch_word;
   unsigned stride = l ? sgx_frame_vertex_floats(l) * sizeof(float) :
                     PACK_VTX_FLOATS * sizeof(float);
   unsigned colour = l ? l->colour : PACK_COLOUR_VARYING;
   bool f32 = l && (l->f32 >> l->colour & 1);
   uint32_t uni_pds = f->ext + EXT_FRAME + FRAME_UNI_PDS;
   uint32_t uni = f->ext + EXT_FRAME + FRAME_UNIFORMS;
   struct sgx_eot to;
   unsigned i;
   int ret;

   if (!sgx_frame_can_render(f, &t) || f->tsize[T_FULL] > STATE_WORDS * 4 ||
       f->tsize[T_FULLPROG] > (int)sizeof(prog) || f->tsize[T_FETCH] > (int)sizeof(fetch) ||
       !nverts || nverts % 3 || nverts > max_vertices(f, stride))
      return -EINVAL;

   if ((ret = begin_render(f, &t, &to, &eot, bg, NULL)))
      return ret;

   /* draw 0: the whole state, the white texture with the replace program */
   memset(full, 0, sizeof(full));
   memcpy(full, f->tmpl + f->toff[T_FULL], f->tsize[T_FULL]);
   state_size(full, to.w, to.h);
   /* the pixel program's PDS: tag (bits 31:27) its data size in rows */
   full[6] = iterated ? 1u << 27 | (iter_pds_at(f, colour, f32) >> 4 & 0x07ffffff) :
             p27(f->texblock);
   if (st) {
      /* ISP state B: the depth compare in bits 24:22, bit 20 set when depth
       * is not written (iOS's depth capture, M5) */
      full[1] = (full[1] & ~(7u << 22 | 1u << 20)) | (uint32_t)(st->depth_func & 7) << 22 |
                (st->depth_write ? 0 : 1u << 20);
   }
   if (pix) {
      /* our pixel program: its PDS (word 6), how many registers a pixel
       * takes (word 5) and its secondary attributes in sa0.. (word 4: a
       * DMA, then the empty program) */
      /* (uniforms in memory only through draw_state(), M32) */
      if (pix->nubuf)
         return -EINVAL;
      if ((ret = upload(f, pix)))
         return ret;
      full[6] = pix->pds_rows << 27 | (pix->pds_va >> 4 & 0x07ffffff);
      full[5] = pixel_word5(pix);
      if (pix->nsa) {
         uint32_t loader[UNIFORM_LOADER_MAX];
         unsigned rows = uniform_loader(f, uni, pix->nsa, loader);

         if (pix->nsa > 128 || !put(f, uni, sa, pix->nsa * 4) ||
             !put(f, uni_pds, loader, sizeof(loader)))
            return -EFAULT;
         full[4] = rows << 27 | (uni_pds >> 4 & 0x07ffffff);
      }
   }
   if (l) {
      /* the vertex's size in words (31:24); three bits a varying, 111 for
       * four components; a bit a varying kept as F16 */
      full[16] = (4 + 4 * l->nvaryings) << 24 | (full[16] & 0x00ffffff);
      full[19] = 0;
      for (i = 0; i < l->nvaryings; i++)
         full[19] |= 7u << 3 * i;
      full[20] = ~l->f32 & ((1u << l->nvaryings) - 1);
   }
   d0 = frame;
   memcpy(prog, f->tmpl + f->toff[T_FULLPROG], f->tsize[T_FULLPROG]);
   prog[0] = d0;
   p0 = (frame + STATE_WORDS * 4 + 0x3f) & ~0x3fu;
   if (!put(f, d0, full, STATE_WORDS * 4) || !put(f, p0, prog, f->tsize[T_FULLPROG]) ||
       !put(f, vb, verts, nverts * stride))
      return -EFAULT;
   if (l) {
      fb = f->fetch_pds + l->nvaryings * FETCH_PDS_SIZE;
      fetch_word = vdm_fetch_word(l->nvaryings);
   } else {
      memcpy(fetch, f->tmpl + f->toff[T_FETCH], f->tsize[T_FETCH]);
      fetch[0] = vb;            /* r g b a */
      fetch[4] = vb + 16;       /* u v */
      fetch[8] = vb + 24;       /* x y */
      fb = (p0 + f->tsize[T_FULLPROG] + 0x3f) & ~0x3fu;
      fetch_word = f->fetch_word;
      if (!put(f, fb, fetch, f->tsize[T_FETCH]))
         return -EFAULT;
   }

   /* the draw: index count, the index buffer (0, 1, 2, ...) */
   *v++ = vdm4(4, f->consts0); *v++ = 0x1000e102;
   *v++ = vdm4(4, p0);         *v++ = 0x12022206;
   *v++ = 0x81c00000 | nverts; *v++ = f->idx; *v++ = 0x70000000; *v++ = 0x003fffff;
   *v++ = vdm4(l ? vdm_fetch_tag(l->nvaryings) : f->fetch_tag, fb); *v++ = fetch_word;
   for (i = 0; i < f->ntail; i++)
      *v++ = f->tail[i];
   if (!put(f, f->vdm, vdm, (v - vdm) * 4))
      return -EFAULT;

   if (f->debug) {
      const float *c = l ? verts + 4 + 4 * l->colour : verts, *pos = l ? verts : verts + 6;

      mesa_logi("sgx: %u vertices into %ux%u at 0x%08x (stride %u); the first's colour "
                "%.3f %.3f %.3f %.3f, position %.3f %.3f", nverts, to.w, to.h, rt->bo->va,
                rt->stride[0], c[0], c[1], c[2], c[3], pos[0], pos[1]);
      if (l)
         mesa_logi("sgx:   vertex side: ours, %u varyings (F32 mask 0x%x), the colour "
                   "varying %u; program 0x%08x, state words 16 %08x 19 %08x 20 %08x, "
                   "fetch word %08x", l->nvaryings, l->f32, l->colour,
                   f->vertex_prog[l->nvaryings], full[16], full[19], full[20], fetch_word);
      else
         mesa_logi("sgx:   vertex side: the pack's");
      mesa_logi("sgx:   SGX_FRAME=%s; our programs at 0x%08x (%s)",
                getenv("SGX_FRAME") ? getenv("SGX_FRAME") : "", f->code_va,
                f->code ? "a buffer of their own" : "the pack's code page");
      if (f->opts & SGX_FRAME_FB)
         mesa_logi("sgx:   the pack's end of tile and background: to the screen");
      else
         mesa_logi("sgx:   end of tile at 0x%08x (DOUTU 0x%08x) to 0x%08x, background "
                   "%08x %08x %08x %08x", eot, doutu(f, eot), to.va, bg[0], bg[1], bg[2], bg[3]);
      if (pix)
         mesa_logi("sgx:   pixel program: compiled, %u instructions at 0x%08x, %u temps, "
                   "%u inputs, %u sa words; its PDS at 0x%08x (%u rows), state words 4 "
                   "%08x 5 %08x 6 %08x", pix->ncode, pix->code_va, pix->ntemps,
                   pix->ninputs, pix->nsa, pix->pds_va, pix->pds_rows, full[4],
                   full[5], full[6]);
      else if (iterated)
         mesa_logi("sgx:   pixel program: varying %u iterated as %s and packed (iOS's); "
                   "its PDS at 0x%08x (state word 6 0x%08x), the program at 0x%08x",
                   colour, f32 ? "F32" : "F16", iter_pds_at(f, colour, f32), full[6],
                   f->code_va + (f32 ? ITER_PROG_F32 : ITER_PROG));
      else
         mesa_logi("sgx:   pixel program: %s (texture block 0x%08x word 0 = 0x%08x)",
                   f->opts & SGX_FRAME_BLEND ? "the pack's" :
                   f->opts & SGX_FRAME_SOP2 ? "replace by SOP2" : "replace by MOV",
                   f->texblock, *(uint32_t *)cpu_at(f, f->texblock, 4, NULL));
      mesa_logi("sgx:   state 0x%08x, its program 0x%08x, vertex fetch 0x%08x, vertices 0x%08x",
                d0, p0, fb, vb);
      for (i = 0; i < (unsigned)(v - vdm); i += 5)
         mesa_logi("sgx:   VDM +%02x: %08x %08x %08x %08x %08x", i * 4, vdm[i], vdm[i + 1],
                   vdm[i + 2], vdm[i + 3], vdm[i + 4]);
   }
   return kick(f, &t, 1.0f, NULL, handles, nhandles, 0, NULL, done);
}

int
sgx_frame_upload(struct sgx_frame *f, struct sgx_pixel_program *p)
{
   return upload(f, p);
}

unsigned
sgx_frame_list_max(const struct sgx_frame *f)
{
   return f->idx_count - f->idx_count % 3;
}

unsigned
sgx_frame_max_draws(const struct sgx_frame *f)
{
   return f->max_draws;
}

/* SGX_STATE=word:mask[,word:mask...] flips bits of every draw's state
 * words, to find what they do */
static void
state_xor(uint32_t *full)
{
   static uint32_t mask[21];
   static int parsed;

   if (!parsed) {
      const char *s = getenv("SGX_STATE");

      parsed = 1;
      while (s && *s) {
         char *e;
         unsigned w = strtoul(s, &e, 0);

         if (*e != ':' || w >= 21)
            break;
         mask[w] ^= strtoul(e + 1, &e, 0);
         s = *e == ',' ? e + 1 : e;
      }
   }
   for (unsigned i = 0; i < 21; i++)
      full[i] ^= mask[i];
}

/* n words of secondary attributes into the render's memory, the nmem
 * words of uniforms in memory after them (M32; sa word slot their address
 * less 4: VLDST adds 4), and their loader after that: the loader's address
 * (*rows its data rows), 0 if there is no room */
static uint32_t
arena_uniforms(struct sgx_frame *f, const uint32_t *words, unsigned n, unsigned nmem,
               unsigned slot, unsigned *rows)
{
   uint32_t loader[UNIFORM_LOADER_MAX], va, at = UNIFORM_LOADER_AT(n + nmem);
   uint8_t *p = n <= 128 && nmem <= SGX_UBUF_MAX && (!nmem || slot < n) ?
                arena_alloc(f, at + sizeof(loader), 16, &va) : NULL;

   if (!p)
      return 0;
   *rows = uniform_loader(f, va, n, loader);
   memcpy(p, words, (n + nmem) * 4);
   if (nmem)
      ((uint32_t *)p)[slot] = va + 4 * n - 4;
   memcpy(p + at, loader, sizeof(loader));
   return va + at;
}

/* A draw's whole state for our vertex side, its secondary attributes'
 * loader and words into the render's memory */
static int
draw_state(struct sgx_frame *f, const struct sgx_frame_draw *d, const struct sgx_eot *to,
           uint32_t *full)
{
   const struct sgx_frame_layout *l = &d->l;
   const struct sgx_pixel_program *pix = d->prog;
   bool f32 = !pix && (l->f32 >> l->colour & 1);

   memset(full, 0, STATE_WORDS * 4);
   memcpy(full, f->tmpl + f->toff[T_FULL], f->tsize[T_FULL]);
   state_size(full, to->w, to->h);
   /* ISP state B: the depth compare in bits 24:22, bit 20 set when depth
    * is not written */
   full[1] = (full[1] & ~(7u << 22 | 1u << 20)) | (uint32_t)(d->st.depth_func & 7) << 22 |
             (d->st.depth_write ? 0 : 1u << 20);
   full[18] = (full[18] & ~3u) | d->st.cull;
   if (d->st.stencil_on) {
      full[3] = d->st.stencil;
      full[1] = (full[1] & ~0xffu) | d->st.stencil_ref;
   }
   /* the viewport: words 9..14, translate and scale for x, y and z */
   if (d->st.viewport)
      for (unsigned i = 0; i < 3; i++) {
         memcpy(&full[9 + 2 * i], &d->st.translate[i], 4);
         memcpy(&full[10 + 2 * i], &d->st.scale[i], 4);
      }
   if (pix) {
      if (!pix->code_va || pix->nsa > 128)
         return -EINVAL;
      full[6] = pix->pds_rows << 27 | (pix->pds_va >> 4 & 0x07ffffff);
      full[5] = pixel_word5(pix);
      if (pix->nsa) {
         unsigned rows;
         uint32_t loader = arena_uniforms(f, d->sa, pix->nsa, pix->nubuf, pix->ubuf_sa, &rows);

         if (!loader)
            return -ENOMEM;
         full[4] = rows << 27 | (loader >> 4 & 0x07ffffff);
      }
   } else {
      if (l->colour >= l->nvaryings)
         return -EINVAL;
      full[6] = 1u << 27 | (iter_pds_at(f, l->colour, f32) >> 4 & 0x07ffffff);
   }
   full[16] = (4 + 4 * l->nvaryings) << 24 | (full[16] & 0x00ffffff);
   full[19] = 0;
   for (unsigned i = 0; i < l->nvaryings; i++)
      full[19] |= 7u << 3 * i;
   full[20] = ~l->f32 & ((1u << l->nvaryings) - 1);
   state_xor(full);
   if (f->debug_state) {
      fprintf(stderr, "sgx: state");
      for (unsigned i = 0; i < 21; i++)
         fprintf(stderr, " %u:%08x", i, full[i]);
      fprintf(stderr, "\n");
   }
   return 0;
}

/* the same state words a draw would write as the one before it: its
 * pixel program, secondary attributes (uniforms, textures), layout and the
 * ISP's state (M25: SDL's tiles, a texture atlas's, come in runs) */
static bool
same_state(const struct sgx_frame_draw *a, const struct sgx_frame_draw *b)
{
   if (!a->prog != !b->prog || a->vs != b->vs || a->l.nvaryings != b->l.nvaryings ||
       a->l.f32 != b->l.f32 || a->l.colour != b->l.colour)
      return false;
   if (a->prog && (a->prog->code_va != b->prog->code_va || a->prog->pds_va != b->prog->pds_va ||
                   a->prog->nsa != b->prog->nsa ||
                   !sgx_words_equal(a->sa, b->sa, a->prog->nsa)))
      return false;
   return sgx_frame_state_equal(&a->st, &b->st);
}

int
sgx_frame_render(struct sgx_frame *f, const struct sgx_frame_target *t,
                 const struct sgx_frame_draw *draws, unsigned n, const uint32_t *handles,
                 unsigned nhandles, float depth_clear, const struct sgx_frame_zls *zls,
                 struct sgx_fence *done)
{
   uint32_t full[32], prog[16], bg[4], eot, *v = f->vdmbuf, state_at = 0, ub_at = 0, vdm;
   unsigned total = 0, ub_rows = 0;
   struct own own, *o = own_copies(f) ? &own : NULL;
   struct sgx_eot to;
   int ret;

   if (!sgx_frame_can_render(f, t) || !n || n > f->max_draws ||
       f->tsize[T_FULL] > STATE_WORDS * 4 || f->tsize[T_FULLPROG] > (int)sizeof(prog) ||
       nhandles > SGX_FRAME_MAX_HANDLES)
      return -EINVAL;
   arena_release(f);
   if (f->queued[f->nqueued % MAX_QUEUED]) {
      sgx_trace_mark("render queue full: wait");
      sgx_fence_wait(f->queued[f->nqueued % MAX_QUEUED], OS_TIMEOUT_INFINITE);
   }
   if ((ret = begin_render(f, t, &to, &eot, bg, o)))
      goto out;

   ret = -ENOMEM;
   for (unsigned k = 0; k < n; k++) {
      const struct sgx_frame_draw *d = &draws[k];
      uint32_t idx_va, count, vb = 0, fetch, words[4 * (SGX_VS_MAX_ATTRIBS + 1) + 32];
      unsigned nwords, rows;

      count = d->vs ? d->count : d->nverts;
      if (!count || count % 3 || d->l.nvaryings > SGX_FRAME_MAX_VARYINGS ||
          (d->prog && d->l.nvaryings != d->prog->nvaryings) ||
          (d->vs && (!d->vs->code_va || d->vs->nvaryings != d->l.nvaryings)) ||
          (!d->indices && count > sgx_frame_list_max(f))) {
         ret = -EINVAL;
         goto out;
      }
      /* the vertices: the draw module's, or a vertex shader's the CPU made
       * one stream, into the render's memory (M26: else the fetch reads
       * them where they are) */
      if (!d->vs || d->vdata) {
         unsigned size = d->vs ? d->nverts * d->stride :
                                 d->nverts * sgx_frame_vertex_floats(&d->l) * sizeof(float);

         if (!(vb = arena_put(f, d->vs ? (const void *)d->vdata : d->verts, size, 16)))
            goto out;
      }
      /* the triangles: the draw's indices, or the frame's 0, 1, 2, ... (from
       * the draw's vertex 0) */
      idx_va = d->indices ? arena_put(f, d->indices, 2 * count, 16) : f->idx;
      if (!idx_va)
         goto out;
      /* the state: the last draw's again when it is the same */
      if (!k || !same_state(&draws[k - 1], d)) {
         uint8_t *slot;

         if ((ret = draw_state(f, d, &to, full)))
            goto out;
         ret = -ENOMEM;
         if (!(slot = arena_alloc(f, DRAW_PROG + f->tsize[T_FULLPROG], 64, &state_at)))
            goto out;
         memcpy(prog, f->tmpl + f->toff[T_FULLPROG], f->tsize[T_FULLPROG]);
         prog[0] = state_at;
         memcpy(slot, full, STATE_WORDS * 4);
         memcpy(slot + DRAW_PROG, prog, f->tsize[T_FULLPROG]);
      } else {
         sgx_stat_same++;
      }
      /* the vertex side's constants (a vertex shader's uniforms: its loader,
       * the words in fours -- the corpus's x00, x02), the state, the draw
       * (count, indices), the vertex fetch */
      if (d->vs && d->vs->nuniforms) {
         /* (the last draw's again when the same) */
         if (!k || !draws[k - 1].vs || draws[k - 1].vs->nuniforms != d->vs->nuniforms ||
             draws[k - 1].vs->nubuf != d->vs->nubuf ||
             !sgx_words_equal(draws[k - 1].vs_sa, d->vs_sa, d->vs->nuniforms + d->vs->nubuf)) {
            if (!(ub_at = arena_uniforms(f, d->vs_sa, d->vs->nuniforms, d->vs->nubuf,
                                         d->vs->ubuf_sa, &ub_rows)))
               goto out;
         }
         /* (the data rows in 31:27, as the state's PDS pointers have them) */
         *v++ = vdm4(4, ub_at);
         *v++ = ub_rows << 27 | 0x0000e100 | DIV_ROUND_UP(d->vs->nuniforms, 4);
      } else {
         *v++ = vdm4(4, f->consts0); *v++ = 0x1000e102;
      }
      *v++ = vdm4(4, state_at + DRAW_PROG); *v++ = 0x12022206;
      *v++ = 0x81c00000 | count; *v++ = idx_va;
      *v++ = 0x70000000; *v++ = 0x003fffff;
      if (d->vs) {
         rows = vs_fetch(f, d, vb, words, &nwords);
      } else {
         nwords = fetch_program(f, words, d->l.nvaryings, vb);
         rows = 0;
      }
      assert(nwords <= ARRAY_SIZE(words));
      if (!(fetch = arena_put(f, words, nwords * 4, 16)))
         goto out;
      *v++ = vdm4(vdm_fetch_tag(d->vs ? d->vs->nvaryings : d->l.nvaryings), fetch);
      *v++ = d->vs ? vs_fetch_word(d->vs, rows) : vdm_fetch_word(d->l.nvaryings);
      if (f->debug) {
         char line[512];
         unsigned m = 0;

         for (unsigned i = 0; i < nwords && m < sizeof(line) - 10; i++)
            m += snprintf(line + m, sizeof(line) - m, " %08x", words[i]);
         mesa_logi("sgx:   draw %u: %u vertices, %u varyings, ISP B %08x, state 4 %08x "
                   "5 %08x 6 %08x, %s; fetch at 0x%08x:%s", k, count, d->l.nvaryings,
                   full[1], full[4], full[5], full[6],
                   d->prog ? "compiled pixels" : "iterated colour", fetch, line);
      }
      total += count;
   }
   /* the stream's end; its terminate program the render's copy */
   for (unsigned i = 0; i < f->ntail; i++)
      *v++ = o && f->tail[i] >> 28 == 6 && (f->tail[i] & 0x0fffffff) << 4 == f->term - 0x10 ?
             vdm4(6, o->term) : f->tail[i];
   /* the stream, in the render's memory (the VDM reads ahead: clear of
    * its end by 512 bytes, sgx2d, M6) */
   {
      uint8_t *p = arena_alloc(f, (v - f->vdmbuf) * 4 + 512, 64, &vdm);

      if (!p)
         goto out;
      memcpy(p, f->vdmbuf, (v - f->vdmbuf) * 4);
   }

   if (f->debug)
      mesa_logi("sgx: a render of %u draws, %u vertices, into %ux%u at 0x%08x (end of tile "
                "0x%08x), its own memory %u chunks%s", n, total, to.w, to.h, to.va, eot,
                f->arena.n, o ? "" : " (the frame's words: after the last render)");
   ret = kick(f, t, depth_clear, zls, handles, nhandles, vdm, o, done);
   if (!ret) {
      sgx_fence_reference(&f->queued[f->nqueued++ % MAX_QUEUED], done);
      for (unsigned i = 0; i < f->arena.n; i++)
         sgx_bo_set_busy(f->arena.bo[i], done);
   }
out:
   arena_release(f);
   return ret;
}
