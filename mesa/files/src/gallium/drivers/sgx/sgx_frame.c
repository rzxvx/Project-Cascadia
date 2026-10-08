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
/* a gathered render's draws (sgx_frame_render): a slot each in the frame's
 * part of the EXT window -- the whole state, its state program, the
 * secondary attributes' loader and words */
#define DRAW_SLOT       0x400
#define DRAW_PROG       0x80
#define DRAW_UNI_PDS    0xc0
#define DRAW_UNIFORMS   0x100
/* the code of our pixel programs: a buffer of its own in the code zone,
 * filled from the start, never reused (the USSE caches code) */
#define HEAP_SIZE       (256 << 10)
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

static unsigned
vdm_fetch_tag(unsigned pack_tag)
{
   static int forced = -2;

   if (forced == -2)
      forced = getenv("SGX_FETCH_TAG") ? (int)strtoul(getenv("SGX_FETCH_TAG"), NULL, 0) : -1;
   return forced >= 0 ? forced : pack_tag;
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
};

struct sgx_frame {
   struct sgx_device *dev;
   struct sgx_bo *bo[MAX_PACK_BOS];
   unsigned nbo;
   uint32_t handles[MAX_PACK_BOS + 3];

   uint32_t kick[3];            /* PB descriptor, render details, TA command */
   uint32_t w, h;
   uint32_t consts0, idx, idx_count, vdm, vdm_size, ext, ext_size, heap, heap_size, pds;
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
   unsigned neot, max_eot;
   uint32_t texblock;           /* the white texture's block, replace program */
   uint32_t iter_pds;           /* the iterated-colour pixel program's PDS, per varying */
   uint32_t fetch_pds;          /* the vertex fetch, per number of varyings */
   struct sgx_bo *code_heap;    /* our pixel programs' code */
   uint32_t heap_used, empty_prog, pds_used;
   uint32_t vdmbuf[10 * SGX_FRAME_MAX_DRAWS + 32];   /* a gathered render's stream */
   uint32_t vertex_prog[SGX_FRAME_MAX_VARYINGS + 1];
   struct sgx_fence *last;      /* the last render through the frame */
   char *dir;                   /* the pack: loaded again after a render hangs */
   uint64_t timeouts;           /* the kernel's count of renders that did */
   bool debug;                  /* SGX_DEBUG=frame: say what each render is */
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
#define BUILT_VDM_SIZE  0x4000
#define BUILT_EXT_SIZE  0x400000

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
   if (dev->uapi < 3 && !pack_pb(f, dir))
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
 * at EXT_VB */
static unsigned
fetch_program(struct sgx_frame *f, uint32_t *p, unsigned nvaryings)
{
   uint32_t vb = f->ext + EXT_VB;
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
      unsigned len = fetch_program(f, pds, n);

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
   f->heap_used = 0x40;
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
   sgx_bo_destroy(f->code);
   sgx_bo_destroy(f->code_heap);
   drop_rt_sets(f);
   for (unsigned i = 0; i < f->nbo; i++)
      sgx_bo_destroy(f->bo[i]);
   free(f->tmpl);
   free(f->dir);
   FREE(f);
}

bool
sgx_frame_can_render(struct sgx_frame *f, struct sgx_resource *rt)
{
   struct pipe_resource *p = &rt->base;
   bool pack_size = f && p->width0 == f->w && p->height0 == f->h;

   return f && (p->format == PIPE_FORMAT_B8G8R8A8_UNORM ||
                p->format == PIPE_FORMAT_B8G8R8X8_UNORM) &&
          p->target == PIPE_TEXTURE_2D && p->width0 <= SGX_RT_MAX_SIZE &&
          p->height0 <= SGX_RT_MAX_SIZE &&
          (pack_size || (f->term && !(f->opts & SGX_FRAME_PACKRT))) &&
          p->array_size == 1 && p->nr_samples <= 1 && rt->offset[0] == 0 &&
          !(rt->stride[0] & 15);
}

/* The render target data for a w x h target: made the first time a render
 * goes to that size -- the buffers as sgx_rt.c fills them, the pack's 3D
 * block with the size's words -- and kept.  No render is running (the
 * caller waited for the last), so a slot can be taken over. */
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
   sgx_bo_destroy(s->bo);
   s->bo = NULL;
   if (!blk || !sgx_rt_layout(&s->rt, w, h, MAX2(f->dev->num_cores, 1)))
      return NULL;
   for (unsigned i = 0; i < SGX_RT_NBUF; i++)
      size += align(s->rt.size[i], 4096);
   size += BLOCK_SIZE;
   if (size > RT_SLOT_SIZE)
      return NULL;
   /* a slot of our own in the TA's heap; the kernel's pick with the frame
    * built (M17) */
   va = RT_SLOT_VA + slot * RT_SLOT_SIZE;
   s->bo = f->built && f->dev->uapi >= 3 ?
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
 * (words 7 and 8: the last tile across and down) and the viewport (9..12:
 * half the width twice, half the height twice -- iOS's captures of other
 * sizes, logs/ios/size) */
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

/* The render: the pack's TA command with the target's render target data
 * (or the pack's own, SGX_FRAME=packrt), its parameter buffer, and the
 * buffers -- the frame's, the target, the render target data, what the
 * draws read besides */
static int
kick(struct sgx_frame *f, struct sgx_resource *rt, float depth, const uint32_t *handles,
     unsigned nhandles, struct sgx_fence *done)
{
   uint32_t hs[MAX_PACK_BOS + 3 + 2 + SGX_FRAME_MAX_HANDLES], cmd[APPLE_SGX_TA_CMD_MAX / 4];
   const uint32_t *pack = f->built ? f->cmd_tmpl :
                          (const uint32_t *)cpu_at(f, f->kick[2], APPLE_SGX_TA_CMD_MIN, NULL);
   unsigned n = f->nhandles, det_bo;
   uint32_t det_handle, det_offset;
   struct rt_set *s = NULL;
   uint8_t *blk;
   int ret;

   if (!pack || pack[0] > sizeof(cmd) ||
       (!f->built && !cpu_at(f, f->kick[2], pack[0], NULL)) || n + 2 + nhandles > ARRAY_SIZE(hs))
      return -EINVAL;
   memcpy(cmd, pack, pack[0]);
   memcpy(hs, f->handles, n * sizeof(uint32_t));
   hs[n++] = rt->bo->handle;
   if (f->opts & SGX_FRAME_PACKRT) {
      if (!cpu_at(f, f->kick[1], 0xa8, &det_bo))
         return -EFAULT;
      det_handle = f->bo[det_bo]->handle;
      det_offset = f->kick[1] - f->bo[det_bo]->va;
      blk = cpu_at(f, cmd[0x50 / 4] + 0x80, 4, NULL);
   } else {
      if (!(s = rt_set(f, rt->base.width0, rt->base.height0)))
         return -ENOMEM;
      sgx_rt_ta_cmd(&s->rt, s->va, s->blk_va, cmd);
      det_handle = hs[n++] = s->bo->handle;
      det_offset = s->va[SGX_RT_DETAILS] - s->bo->va;
      blk = s->bo->map + (s->blk_va - s->bo->va) + 0x80;
   }
   /* the depth the tiles start at: register 0x4b8, the 3D block's +0x80
    * (1.0 as the kext sets it, M4) */
   if (!blk)
      return -EFAULT;
   memcpy(blk, &depth, 4);
   memcpy(hs + n, handles, nhandles * sizeof(uint32_t));
   n += nhandles;
   if (f->debug)
      mesa_logi("sgx:   kick: PB 0x%08x, details 0x%08x (%s), 3D block 0x%08x, TA command "
                "%u bytes, %u buffers", f->kick[0], cmd[0x54 / 4],
                s ? "ours" : "the pack's", cmd[0x50 / 4], cmd[0], n);
   ret = sgx_submit(f->dev, cmd, f->kick[0], det_handle, det_offset, hs, n, done);
   if (!ret)
      sgx_fence_reference(&f->last, done);
   return ret;
}

/* The end-of-tile program for a target: written once per target, in a
 * slot of its own.  When the slots run out (seven in the pack's page) the
 * oldest is written over -- no render is running then (the caller waits for
 * the last), but the USSE may still have the old program in its cache. */
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
      if (!memcmp(&f->eot[i], want, sizeof(*want)))
         return f->code_va + PROGS_SIZE + i * EOT_SLOT;
   slot = f->neot++ % f->max_eot;
   if (f->neot == f->max_eot + 1)
      mesa_logw("sgx: more render targets than end-of-tile slots (%u); old ones are "
                "written over", f->max_eot);
   f->eot[slot] = *want;

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

/* A pixel program into GPU memory, at its first draw: the code into the
 * heap, and its PDS program -- start the program, then iterate each input
 * (F32, four registers; iOS's shape, the corpus's v04 and v05) -- into the
 * EXT window.  Neither place is used again for anything else. */
static int
upload(struct sgx_frame *f, struct sgx_pixel_program *p)
{
   uint32_t pds[PROG_PDS_SLOT / 4], size = p->ncode * 8;
   unsigned n = 0, data;

   if (p->code_va)
      return 0;
   if (f->heap_used + size > HEAP_SIZE || f->pds_used + PROG_PDS_SLOT >
       EXT_PROG_PDS_END - EXT_PROG_PDS || p->ninputs > SGX_FRAME_MAX_VARYINGS) {
      mesa_logw("sgx: no room for another pixel program");
      return -ENOSPC;
   }
   memcpy((uint8_t *)f->code_heap->map + f->heap_used, p->code, size);
   p->code_va = f->code_heap->va + f->heap_used;
   f->heap_used = align(f->heap_used + size, 64);

   pds[n++] = doutu(f, p->code_va);
   pds[n++] = p->ninputs ? ITER_TEMPS : 2;
   pds[n++] = 0;
   for (unsigned i = 0; i < p->ninputs; i++)
      pds[n++] = (i == p->ninputs - 1 ? ITERATE_F32_VEC4 : ITERATE_F32_VEC4_MORE) | i << 12;
   while (n % 4)
      pds[n++] = 0;
   data = n;
   pds[n++] = PDS_DOUTU_ROW0_ITER;
   for (unsigned i = 0; i < p->ninputs; i++)
      pds[n++] = pds_iterate(3 + i, 0x32);
   pds[n++] = PDS_END;
   p->pds_va = f->ext + EXT_PROG_PDS + f->pds_used;
   p->pds_rows = data / 4;
   f->pds_used += PROG_PDS_SLOT;
   return put(f, p->pds_va, pds, n * 4) ? 0 : -EFAULT;
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
       (prog ? l->nvaryings != prog->ninputs : l->colour >= l->nvaryings))
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

/* A render through the frame: with l, our vertex side (M13b) and verts
 * laid out as it says; without, the pack's, verts r g b a u v x y.  The
 * pixel side is the iterated colour (l's colour varying, or the pack's) or
 * the pack's texel x colour. */
/* What every render starts with: the last one done (the frame's buffers
 * are its until then), the pack again after a hang, and the end of tile
 * and background aimed at the target (or, with SGX_FRAME=screen, the
 * framebuffer). */
static int
begin_render(struct sgx_frame *f, struct sgx_resource *rt, struct sgx_eot *out_to,
             uint32_t *out_eot, uint32_t bg[4])
{
   struct sgx_eot to = {
      .va = rt->bo->va, .w = rt->base.width0, .h = rt->base.height0, .stride = rt->stride[0],
   };
   uint32_t eot, d0, tiles = term_tiles(to.w, to.h);
   uint64_t timeouts;

   /* the frame's buffers are the last render's until it is done */
   if (f->last && !sgx_fence_wait(f->last, 5ull * 1000 * 1000 * 1000))
      mesa_logw("sgx: the last render through the template frame is still running");

   /* a render that hung (this process's or another's) leaves the parameter
    * buffer and the render target data half-used: the pack again */
   timeouts = sgx_device_param(f->dev, APPLE_SGX_PARAM_RENDERS_TIMED_OUT);
   if (timeouts != f->timeouts) {
      mesa_logw("sgx: a render timed out (%llu so far); the template frame is "
                "loaded again", (unsigned long long)timeouts);
      f->timeouts = timeouts;
      if (!load_images(f) || !put_ours(f))
         return -EFAULT;
      drop_rt_sets(f);
   }
   /* the tiles the stream's end covers: the target's */
   if (f->term && !put(f, f->term, &tiles, 4))
      return -EFAULT;

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
   if (!(f->opts & SGX_FRAME_FB) &&
       (!put(f, f->pds + 8, &d0, 4) || !put(f, f->pds + 0x130, bg, 4 * sizeof(uint32_t))))
      return -EFAULT;

   *out_to = to;
   *out_eot = eot;
   return 0;
}

static int
render(struct sgx_frame *f, struct sgx_resource *rt, const struct sgx_frame_layout *l,
       const float *verts, unsigned nverts, bool iterated, struct sgx_pixel_program *pix,
       const uint32_t *sa, const uint32_t *handles, unsigned nhandles,
       const struct sgx_frame_state *st, struct sgx_fence *done)
{
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

   if (!sgx_frame_can_render(f, rt) || f->tsize[T_FULL] > (int)sizeof(full) ||
       f->tsize[T_FULLPROG] > (int)sizeof(prog) || f->tsize[T_FETCH] > (int)sizeof(fetch) ||
       !nverts || nverts % 3 || nverts > max_vertices(f, stride))
      return -EINVAL;

   if ((ret = begin_render(f, rt, &to, &eot, bg)))
      return ret;

   /* draw 0: the whole state, the white texture with the replace program */
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
       * takes (word 5: bits 31:27 primary attributes + temporaries in fours;
       * 26:23 as iOS has them for 2, 3 and 4 fours, 12 / fours -- M13b),
       * and its secondary attributes in sa0.. (word 4: a DMA, then the empty
       * program) */
      unsigned fours = DIV_ROUND_UP(4 * pix->ninputs + pix->ntemps, 4);
      uint32_t loader[12] = {
         uni, pix->nsa - 1, 0, 0, doutu(f, f->empty_prog), 2, 0, 0,
         PDS_DMA_ROW0, PDS_DOUTU_ROW1_AFTER, PDS_END, 0,
      };

      if ((ret = upload(f, pix)))
         return ret;
      full[6] = pix->pds_rows << 27 | (pix->pds_va >> 4 & 0x07ffffff);
      full[5] = fours << 27 | (fours > 1 ? 12 / fours : 0) << 23 | 0x0003e000;
      if (pix->nsa) {
         if (!put(f, uni, sa, pix->nsa * 4) || !put(f, uni_pds, loader, sizeof(loader)))
            return -EFAULT;
         full[4] = 2u << 27 | (uni_pds >> 4 & 0x07ffffff);
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
   p0 = (frame + f->tsize[T_FULL] + 0x3f) & ~0x3fu;
   if (!put(f, d0, full, f->tsize[T_FULL]) || !put(f, p0, prog, f->tsize[T_FULLPROG]) ||
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
   *v++ = vdm4(l ? vdm_fetch_tag(f->fetch_tag) : f->fetch_tag, fb); *v++ = fetch_word;
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
   return kick(f, rt, 1.0f, handles, nhandles, done);
}

int
sgx_frame_upload(struct sgx_frame *f, struct sgx_pixel_program *p)
{
   return upload(f, p);
}

bool
sgx_frame_place(struct sgx_frame *f, unsigned *cursor, const struct sgx_frame_layout *l,
                unsigned nverts, unsigned *first)
{
   unsigned stride = sgx_frame_vertex_floats(l) * sizeof(float);
   /* a draw's vertices start at a multiple of eight vertices of its own
    * size: its indices (into the pack's buffer of 0, 1, 2, ...) at a
    * 16-byte boundary */
   unsigned start = align(DIV_ROUND_UP(*cursor, stride), 8);

   if (start + nverts > f->idx_count || (start + nverts) * stride > EXT_FRAME - EXT_VB)
      return false;
   if (first)
      *first = start;
   *cursor = (start + nverts) * stride;
   return true;
}

/* A draw's whole state for our vertex side, its secondary attributes'
 * loader and words written into its slot at base */
static int
draw_state(struct sgx_frame *f, const struct sgx_frame_draw *d, uint32_t base,
           const struct sgx_eot *to, uint32_t *full)
{
   const struct sgx_frame_layout *l = &d->l;
   const struct sgx_pixel_program *pix = d->prog;
   bool f32 = !pix && (l->f32 >> l->colour & 1);

   memcpy(full, f->tmpl + f->toff[T_FULL], f->tsize[T_FULL]);
   state_size(full, to->w, to->h);
   /* ISP state B: the depth compare in bits 24:22, bit 20 set when depth
    * is not written */
   full[1] = (full[1] & ~(7u << 22 | 1u << 20)) | (uint32_t)(d->st.depth_func & 7) << 22 |
             (d->st.depth_write ? 0 : 1u << 20);
   if (pix) {
      unsigned fours = DIV_ROUND_UP(4 * pix->ninputs + pix->ntemps, 4);
      const uint32_t loader[12] = {
         base + DRAW_UNIFORMS, pix->nsa - 1, 0, 0, doutu(f, f->empty_prog), 2, 0, 0,
         PDS_DMA_ROW0, PDS_DOUTU_ROW1_AFTER, PDS_END, 0,
      };

      if (!pix->code_va || pix->nsa * 4 > DRAW_SLOT - DRAW_UNIFORMS)
         return -EINVAL;
      full[6] = pix->pds_rows << 27 | (pix->pds_va >> 4 & 0x07ffffff);
      full[5] = fours << 27 | (fours > 1 ? 12 / fours : 0) << 23 | 0x0003e000;
      if (pix->nsa) {
         if (!put(f, base + DRAW_UNIFORMS, d->sa, pix->nsa * 4) ||
             !put(f, base + DRAW_UNI_PDS, loader, sizeof(loader)))
            return -EFAULT;
         full[4] = 2u << 27 | ((base + DRAW_UNI_PDS) >> 4 & 0x07ffffff);
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
   return 0;
}

int
sgx_frame_render(struct sgx_frame *f, struct sgx_resource *rt,
                 const struct sgx_frame_draw *draws, unsigned n, const uint32_t *handles,
                 unsigned nhandles, float depth_clear, struct sgx_fence *done)
{
   uint32_t full[32], prog[16], bg[4], eot, *v = f->vdmbuf;
   unsigned cursor = 0, total = 0;
   struct sgx_eot to;
   int ret;

   if (!sgx_frame_can_render(f, rt) || !n || n > SGX_FRAME_MAX_DRAWS ||
       f->tsize[T_FULL] > (int)sizeof(full) || f->tsize[T_FULLPROG] > (int)sizeof(prog) ||
       nhandles > SGX_FRAME_MAX_HANDLES)
      return -EINVAL;
   if ((ret = begin_render(f, rt, &to, &eot, bg)))
      return ret;

   for (unsigned k = 0; k < n; k++) {
      const struct sgx_frame_draw *d = &draws[k];
      uint32_t base = f->ext + EXT_FRAME + k * DRAW_SLOT;
      unsigned first, stride = sgx_frame_vertex_floats(&d->l) * sizeof(float);

      if (!d->nverts || d->nverts % 3 || d->l.nvaryings > SGX_FRAME_MAX_VARYINGS ||
          (d->prog && d->l.nvaryings != d->prog->ninputs))
         return -EINVAL;
      if (!sgx_frame_place(f, &cursor, &d->l, d->nverts, &first))
         return -ENOSPC;
      if ((ret = draw_state(f, d, base, &to, full)))
         return ret;
      memcpy(prog, f->tmpl + f->toff[T_FULLPROG], f->tsize[T_FULLPROG]);
      prog[0] = base;
      if (!put(f, f->ext + EXT_VB + first * stride, d->verts, d->nverts * stride) ||
          !put(f, base, full, f->tsize[T_FULL]) ||
          !put(f, base + DRAW_PROG, prog, f->tsize[T_FULLPROG]))
         return -EFAULT;
      /* the constants, the state, the draw (count, its first index in the
       * pack's buffer of 0, 1, 2, ...), the vertex fetch */
      *v++ = vdm4(4, f->consts0); *v++ = 0x1000e102;
      *v++ = vdm4(4, base + DRAW_PROG); *v++ = 0x12022206;
      *v++ = 0x81c00000 | d->nverts; *v++ = f->idx + 2 * first;
      *v++ = 0x70000000; *v++ = 0x003fffff;
      *v++ = vdm4(vdm_fetch_tag(f->fetch_tag), f->fetch_pds + d->l.nvaryings * FETCH_PDS_SIZE);
      *v++ = vdm_fetch_word(d->l.nvaryings);
      total += d->nverts;
      if (f->debug)
         mesa_logi("sgx:   draw %u: %u vertices from %u, %u varyings, ISP B %08x, state "
                   "4 %08x 5 %08x 6 %08x, %s", k, d->nverts, first, d->l.nvaryings, full[1],
                   full[4], full[5], full[6], d->prog ? "compiled pixels" : "iterated colour");
   }
   for (unsigned i = 0; i < f->ntail; i++)
      *v++ = f->tail[i];
   /* the VDM reads ahead: keep clear of its window's end (sgx2d, M6) */
   if ((v - f->vdmbuf) * 4 + 512 > f->vdm_size)
      return -ENOSPC;
   if (!put(f, f->vdm, f->vdmbuf, (v - f->vdmbuf) * 4))
      return -EFAULT;

   if (f->debug)
      mesa_logi("sgx: a render of %u draws, %u vertices, into %ux%u at 0x%08x (end of tile "
                "0x%08x)", n, total, to.w, to.h, to.va, eot);
   return kick(f, rt, depth_clear, handles, nhandles, done);
}
