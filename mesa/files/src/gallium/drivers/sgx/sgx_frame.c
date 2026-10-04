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
 * They go in the pack's code page, in the 1 KiB below its first program,
 * where the pack's own programs are known to run.  The 3D pass's event
 * program names the end-of-tile program in its data (word 2), and the
 * background object reloads every tile from a linear descriptor (the 3D
 * PDS block +0x130): both are pointed at the render target before each
 * render.
 *
 * SGX_FRAME swaps pieces for the pack's, or moves them, to find on the
 * device which one is wrong when a clear is (sgx_frame.h).
 */
#include "sgx_frame.h"

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

/* the EXT window, as sgx2d uses it */
#define EXT_TEX_END     0x280000
#define EXT_VB          0x280000
#define EXT_FRAME       0x3c0000
#define VTX_FLOATS      SGX_FRAME_VERTEX_FLOATS  /* r g b a u v x y */

#define MAX_PACK_BOS    48

/* our programs: the replace program, then one end-of-tile program a slot */
#define REPLACE_SIZE    0x40
#define EOT_SLOT        0x80
#define MAX_EOT         256
#define CODE_BO_SIZE    (REPLACE_SIZE + MAX_EOT * EOT_SLOT)
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
static const uint64_t usse_dummy_load[3] = {
   0x488b0281a00c0000ull, 0xe9a30084a0000000ull, 0xf920000000000000ull,
};

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

struct sgx_frame {
   struct sgx_device *dev;
   struct sgx_bo *bo[MAX_PACK_BOS];
   unsigned nbo;
   uint32_t handles[MAX_PACK_BOS + 2];

   uint32_t kick[3];            /* PB descriptor, render details, TA command */
   uint32_t w, h;
   uint32_t consts0, idx, idx_count, vdm, vdm_size, ext, ext_size, heap, heap_size, pds;
   uint32_t fetch_tag, fetch_word;
   uint32_t tail[8];
   unsigned ntail;
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

   snprintf(path, sizeof(path), "%s/pack.txt", f->dir);
   if (!(fp = fopen(path, "r")))
      return false;
   while (ok && fgets(line, sizeof(line), fp)) {
      uint8_t *img = NULL;

      if (sscanf(line, "%63s %63s %63s", key, a, b) < 3 || strcmp(key, "img"))
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

/* The buffer for our programs: the lowest free 64 KiB step above the
 * pack's windows in the code zone, within a DOUTU's reach */
static struct sgx_bo *
code_bo(struct sgx_frame *f)
{
   struct sgx_device *dev = f->dev;
   uint32_t size = align(CODE_BO_SIZE, 4096);
   uint32_t lo = MAX2(dev->code_va_start, dev->code_base);
   uint32_t hi = MIN2(dev->code_va_end, dev->code_base + CODE_REACH);

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

/* Our pieces into the pack's buffers (again after load_images): the
 * replace program, a 4x4 white texture at the start of the texel heap, and
 * its block (word 0 the pixel program, 5 the size, log2, 6 the address).
 * End-of-tile programs are written as targets come. */
static bool
put_ours(struct sgx_frame *f)
{
   const uint64_t replace[3] = {
      USSE_PHAS, USSE_SOP2M_MOD,
      f->opts & SGX_FRAME_SOP2 ? USSE_SOP2_REPLACE : USSE_MOV_O0_PA0,
   };
   uint32_t white[16], blk[16];

   memcpy(f->code_map, replace, sizeof(replace));
   f->neot = 0;
   memset(white, 0xff, sizeof(white));
   memcpy(blk, f->tmpl + f->toff[T_TEX], f->tsize[T_TEX]);
   if (!(f->opts & SGX_FRAME_BLEND))
      blk[0] = doutu(f, f->code_va);
   blk[5] = 0x0c000000 | 2 << 16 | 2;
   blk[6] = f->heap;
   f->texblock = f->ext;
   return put(f, f->heap, white, sizeof(white)) &&
          put(f, f->texblock, blk, f->tsize[T_TEX]);
}

struct sgx_frame *
sgx_frame_create(struct sgx_device *dev, const char *dir)
{
   struct sgx_frame *f = CALLOC_STRUCT(sgx_frame);
   uint32_t page;
   uint8_t *at;

   if (!f)
      return NULL;
   f->dev = dev;
   f->debug = getenv("SGX_DEBUG") && strstr(getenv("SGX_DEBUG"), "frame");
   f->opts = sgx_frame_options();
   if (!(f->dir = strdup(dir)) || !load_pack(f, dir))
      goto fail;
   f->timeouts = sgx_device_param(dev, APPLE_SGX_PARAM_RENDERS_TIMED_OUT);
   f->nhandles = f->nbo;

   /* where our programs go: the free start of the pack's code page, or a
    * buffer of their own */
   page = dev->code_base + PAGE_OFFSET;
   at = cpu_at(f, page, PAGE_FREE, NULL);
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
      f->max_eot = (PAGE_FREE - REPLACE_SIZE) / EOT_SLOT;
   } else {
      if (!(f->code = code_bo(f)))
         goto fail;
      f->code_va = f->code->va;
      f->code_map = f->code->map;
      f->max_eot = MAX_EOT;
      f->handles[f->nhandles++] = f->code->handle;
   }
   if (!put_ours(f))
      goto fail;
   mesa_logi("sgx: template frame %ux%u from %s", f->w, f->h, dir);
   return f;

fail:
   sgx_frame_destroy(f);
   return NULL;
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

   return f && (p->format == PIPE_FORMAT_B8G8R8A8_UNORM ||
                p->format == PIPE_FORMAT_B8G8R8X8_UNORM) &&
          p->target == PIPE_TEXTURE_2D && p->width0 == f->w && p->height0 == f->h &&
          p->array_size == 1 && p->nr_samples <= 1 && rt->offset[0] == 0 &&
          !(rt->stride[0] & 15);
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
         return f->code_va + REPLACE_SIZE + i * EOT_SLOT;
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
   memcpy(f->code_map + REPLACE_SIZE + slot * EOT_SLOT, prog, sizeof(prog));
   return f->code_va + REPLACE_SIZE + slot * EOT_SLOT;
}

/* two triangles over the whole target, r g b a u v x y */
static void
quad(float *v, const float rgba[4])
{
   static const float corner[4][2] = { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 1 } };
   static const int order[6] = { 0, 1, 2, 0, 2, 3 };

   for (unsigned i = 0; i < 6; i++, v += VTX_FLOATS) {
      const float *c = corner[order[i]];

      memcpy(v, rgba, 4 * sizeof(float));
      v[4] = c[0];
      v[5] = c[1];
      v[6] = c[0] * 2 - 1;
      v[7] = c[1] * 2 - 1;
   }
}

unsigned
sgx_frame_max_vertices(struct sgx_frame *f)
{
   unsigned n = MIN2(f->idx_count, (EXT_FRAME - EXT_VB) / (VTX_FLOATS * sizeof(float)));

   return n - n % 3;
}

int
sgx_frame_clear(struct sgx_frame *f, struct sgx_resource *rt, const float rgba[4],
                struct sgx_fence *done)
{
   float verts[6 * VTX_FLOATS];

   quad(verts, rgba);
   return sgx_frame_draw(f, rt, verts, 6, done);
}

int
sgx_frame_draw(struct sgx_frame *f, struct sgx_resource *rt, const float *verts,
               unsigned nverts, struct sgx_fence *done)
{
   uint32_t vdm[32] = { 0 }, full[32], prog[16], fetch[32], bg[4], *v = vdm, *cmd;
   uint32_t frame = f->ext + EXT_FRAME, vb = f->ext + EXT_VB, d0, p0, fb, eot;
   struct sgx_eot to = {
      .va = rt->bo->va, .w = f->w, .h = f->h, .stride = rt->stride[0],
   };
   unsigned det_bo, i;
   uint64_t timeouts;
   uint8_t *det;
   int ret;

   if (!sgx_frame_can_render(f, rt) || f->tsize[T_FULL] > (int)sizeof(full) ||
       f->tsize[T_FULLPROG] > (int)sizeof(prog) || f->tsize[T_FETCH] > (int)sizeof(fetch) ||
       !nverts || nverts % 3 || nverts > sgx_frame_max_vertices(f))
      return -EINVAL;

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
   bg[1] = 0xcc000000 | (f->w - 1) << 12 | (f->h - 1);
   bg[2] = to.va;
   bg[3] = 0x10000000;
   d0 = doutu(f, eot);
   if (!(f->opts & SGX_FRAME_FB) &&
       (!put(f, f->pds + 8, &d0, 4) || !put(f, f->pds + 0x130, bg, sizeof(bg))))
      return -EFAULT;

   /* draw 0: the whole state, the white texture with the replace program */
   memcpy(full, f->tmpl + f->toff[T_FULL], f->tsize[T_FULL]);
   full[6] = p27(f->texblock);
   d0 = frame;
   memcpy(prog, f->tmpl + f->toff[T_FULLPROG], f->tsize[T_FULLPROG]);
   prog[0] = d0;
   p0 = (frame + f->tsize[T_FULL] + 0x3f) & ~0x3fu;
   memcpy(fetch, f->tmpl + f->toff[T_FETCH], f->tsize[T_FETCH]);
   fetch[0] = vb;               /* r g b a */
   fetch[4] = vb + 16;          /* u v */
   fetch[8] = vb + 24;          /* x y */
   fb = (p0 + f->tsize[T_FULLPROG] + 0x3f) & ~0x3fu;
   if (!put(f, d0, full, f->tsize[T_FULL]) || !put(f, p0, prog, f->tsize[T_FULLPROG]) ||
       !put(f, fb, fetch, f->tsize[T_FETCH]) ||
       !put(f, vb, verts, nverts * VTX_FLOATS * sizeof(float)))
      return -EFAULT;

   /* the draw: index count, the index buffer (0, 1, 2, ...) */
   *v++ = vdm4(4, f->consts0); *v++ = 0x1000e102;
   *v++ = vdm4(4, p0);         *v++ = 0x12022206;
   *v++ = 0x81c00000 | nverts; *v++ = f->idx; *v++ = 0x70000000; *v++ = 0x003fffff;
   *v++ = vdm4(f->fetch_tag, fb); *v++ = f->fetch_word;
   for (i = 0; i < f->ntail; i++)
      *v++ = f->tail[i];
   if (!put(f, f->vdm, vdm, (v - vdm) * 4))
      return -EFAULT;

   /* the render: the pack's TA command, PB and render details */
   cmd = (uint32_t *)cpu_at(f, f->kick[2], APPLE_SGX_TA_CMD_MIN, NULL);
   det = cpu_at(f, f->kick[1], 0xa8, &det_bo);
   if (!cmd || !det || !cpu_at(f, f->kick[2], cmd[0], NULL))
      return -EFAULT;
   f->handles[f->nhandles] = rt->bo->handle;

   if (f->debug) {
      mesa_logi("sgx: %u vertices into %ux%u at 0x%08x (stride %u); the first's colour "
                "%.3f %.3f %.3f %.3f, position %.3f %.3f", nverts, f->w, f->h, rt->bo->va,
                rt->stride[0], verts[0], verts[1], verts[2], verts[3], verts[6], verts[7]);
      mesa_logi("sgx:   SGX_FRAME=%s; our programs at 0x%08x (%s)",
                getenv("SGX_FRAME") ? getenv("SGX_FRAME") : "", f->code_va,
                f->code ? "a buffer of their own" : "the pack's code page");
      if (f->opts & SGX_FRAME_FB)
         mesa_logi("sgx:   the pack's end of tile and background: to the screen");
      else
         mesa_logi("sgx:   end of tile at 0x%08x (DOUTU 0x%08x) to 0x%08x, background "
                   "%08x %08x %08x %08x", eot, doutu(f, eot), to.va, bg[0], bg[1], bg[2], bg[3]);
      mesa_logi("sgx:   pixel program: %s (texture block 0x%08x word 0 = 0x%08x)",
                f->opts & SGX_FRAME_BLEND ? "the pack's" :
                f->opts & SGX_FRAME_SOP2 ? "replace by SOP2" : "replace by MOV",
                f->texblock, *(uint32_t *)cpu_at(f, f->texblock, 4, NULL));
      mesa_logi("sgx:   state 0x%08x, its program 0x%08x, vertex fetch 0x%08x, vertices 0x%08x",
                d0, p0, fb, vb);
      for (i = 0; i < (unsigned)(v - vdm); i += 5)
         mesa_logi("sgx:   VDM +%02x: %08x %08x %08x %08x %08x", i * 4, vdm[i], vdm[i + 1],
                   vdm[i + 2], vdm[i + 3], vdm[i + 4]);
      mesa_logi("sgx:   kick: PB 0x%08x, details 0x%08x, TA command 0x%08x (%u bytes), %u buffers",
                f->kick[0], f->kick[1], f->kick[2], cmd[0], f->nhandles + 1);
   }
   ret = sgx_submit(f->dev, cmd, f->kick[0], f->bo[det_bo]->handle,
                    f->kick[1] - f->bo[det_bo]->va, f->handles, f->nhandles + 1, done);
   if (!ret)
      sgx_fence_reference(&f->last, done);
   return ret;
}
