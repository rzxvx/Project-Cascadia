/*
 * Copyright 2026 Project Cascadia
 * SPDX-License-Identifier: MIT
 *
 * tmpl-test.c -- the template frame as sgx_template.c builds it, at the
 * pack's addresses, against the pack (./cascadia gpu's build/sgx2d/pack):
 *
 *   tmpl-test PACKDIR
 *
 * The PDS block, the state area and the index buffer against the GL
 * window's image, the programs against the code page's, the stream's tail
 * against pack.txt, the TA command and the 3D block (with the render
 * target's words from sgx_rt.c) against theirs.  Exit status 0: the same.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sgx_rt.h"
#include "sgx_template.h"

/* the pack's addresses (tools/sgx/frame.py, programs.py, rpack.py, rgen.py) */
#define GL_VA     0x98956000u
#define CODE_BASE 0x9a060000u
#define PAGE      0x9a061000u
#define RT_VA     0x87c00000u

static const uint32_t layout[SGX_TP_N] = {
   [SGX_TP_BG_RELOAD] = 0x400, [SGX_TP_EMPTY_A] = 0x540, [SGX_TP_EMPTY_B] = 0xe40,
   [SGX_TP_EMPTY_C] = 0xec0, [SGX_TP_EMPTY_D] = 0xf00, [SGX_TP_STATE_2] = 0x600,
   [SGX_TP_STATE_4] = 0x680, [SGX_TP_STATE_21] = 0xac0, [SGX_TP_DONE] = 0xc00,
   [SGX_TP_EOR] = 0xc40, [SGX_TP_VERTEX] = 0xdc0,
};

static uint8_t *
load(const char *dir, const char *name, size_t *len)
{
   char path[512];
   uint8_t *buf;
   FILE *f;
   long n;

   snprintf(path, sizeof(path), "%s/%s", dir, name);
   if (!(f = fopen(path, "rb"))) {
      perror(path);
      exit(2);
   }
   fseek(f, 0, SEEK_END);
   n = ftell(f);
   fseek(f, 0, SEEK_SET);
   buf = malloc(n);
   if (fread(buf, 1, n, f) != (size_t)n)
      exit(2);
   fclose(f);
   *len = n;
   return buf;
}

static int
compare(const char *what, const void *ours, const void *theirs, size_t n)
{
   const uint32_t *a = ours, *b = theirs;
   int bad = 0;

   for (size_t i = 0; i < n / 4; i++)
      if (a[i] != b[i] && bad++ < 6)
         printf("    %s +%#05zx: ours %08x, the pack's %08x\n", what, 4 * i, a[i], b[i]);
   printf("%-12s %s\n", what, bad ? "DIFFERS" : "same");
   return !!bad;
}

int
main(int argc, char **argv)
{
   static uint32_t pds[SGX_TMPL_PDS_SIZE / 4], state[SGX_TMPL_STATE_SIZE / 4];
   static uint32_t cmd[SGX_TMPL_CMD_SIZE / 4], blk[SGX_TMPL_BLOCK_SIZE / 4];
   static uint16_t idx[SGX_TMPL_IDX_COUNT];
   struct sgx_tmpl t = {
      .code_base = CODE_BASE, .eot = PAGE + 0xd40, .pixel = PAGE + 0xe80,
      .pds = GL_VA, .state = GL_VA + 0x5f000, .idx = GL_VA + 0x11000,
      .tex0 = GL_VA + 0x16000, .tex1 = GL_VA + 0x18000,
      .fb = 0x90000000, .fb_w = 768, .fb_h = 1024, .fb_stride = 768, .w = 768, .h = 1024,
   };
   uint32_t va[SGX_RT_NBUF], at = RT_VA, tail[5], ptail[5] = { 0 };
   uint8_t *gl, *page, *cmdimg, *rtimg, *txt;
   size_t gl_len, page_len, cmd_len, rt_len, txt_len;
   struct sgx_rt rt;
   int bad = 0;

   if (argc != 2) {
      fprintf(stderr, "usage: tmpl-test PACKDIR\n");
      return 2;
   }
   gl = load(argv[1], "m_98956000.bin", &gl_len);
   page = load(argv[1], "m_9a061000.bin", &page_len);
   cmdimg = load(argv[1], "m_87b00000.bin", &cmd_len);
   rtimg = load(argv[1], "m_87c00000.bin", &rt_len);
   txt = load(argv[1], "pack.txt", &txt_len);
   for (unsigned p = 0; p < SGX_TP_N; p++)
      t.prog[p] = PAGE + layout[p];

   /* the GL window: the event program comes from it (the IPSW's) */
   sgx_tmpl_pds(&t, (const uint32_t *)gl, pds);
   bad |= compare("PDS block", pds, gl, sizeof(pds));
   sgx_tmpl_state(&t, state);
   bad |= compare("state", state, gl + 0x5f000, 0x40c);
   for (unsigned i = 0; i < SGX_TMPL_IDX_COUNT; i++)
      idx[i] = i;
   bad |= compare("indices", idx, gl + 0x11000, sizeof(idx));

   for (unsigned p = 0; p < SGX_TP_N; p++) {
      uint64_t code[SGX_TMPL_PROG_MAX];
      unsigned n = sgx_tmpl_program(p, (t.prog[p] - CODE_BASE) / 8, code);
      char name[32];

      snprintf(name, sizeof(name), "program %u", p);
      bad |= compare(name, code, page + layout[p], n * 8);
   }

   sgx_tmpl_tail(&t, tail);
   {
      const char *l = strstr((const char *)txt, "\ntail ");

      if (l)
         sscanf(l + 6, "%x %x %x %x %x", &ptail[0], &ptail[1], &ptail[2], &ptail[3], &ptail[4]);
      bad |= compare("stream tail", tail, ptail, sizeof(tail));
   }

   /* the render target data where rtemu.py put them for the pack */
   sgx_rt_layout(&rt, 768, 1024, 2);
   for (unsigned i = 0; i < SGX_RT_NBUF; i++) {
      va[i] = at;
      at += (rt.size[i] + 0xfff) & ~0xfffu;
   }
   sgx_tmpl_ta_cmd(&t, 0x98f00000, 0x88000000, cmd);
   sgx_rt_ta_cmd(&rt, va, at, cmd);
   bad |= compare("TA command", cmd, cmdimg, sizeof(cmd));
   sgx_tmpl_block3d(&t, blk);
   sgx_rt_block3d(&rt, va, blk);
   bad |= compare("3D block", blk, rtimg + (at - RT_VA), sizeof(blk));
   printf("%s\n", bad ? "the built frame is NOT the pack's" : "the built frame is the pack's");
   return bad;
}
