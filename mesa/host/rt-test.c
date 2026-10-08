/*
 * Copyright 2026 Project Cascadia
 * SPDX-License-Identifier: MIT
 *
 * rt-test.c -- writes what the render target code (sgx_rt.c) makes for one
 * size, for rt-test.py to compare with rtemu.py's:
 *
 *   rt-test W H NCORES BLK_VA VA0..VA5 OUTDIR PAYLOAD BLK CMD
 *
 * OUTDIR gets b0.bin..b5.bin (each buffer, its size rounded up to pages),
 * and payload.bin, blk3d.bin, cmd.bin: the files PAYLOAD (iOS's payload
 * words from w2), BLK (a 3D block) and CMD (a TA command) with the render
 * target's words written over theirs.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sgx_rt.h"

static size_t
load(const char *path, uint32_t *buf, size_t max)
{
   FILE *f = fopen(path, "rb");
   size_t n;

   if (!f) {
      perror(path);
      exit(2);
   }
   n = fread(buf, 1, max, f);
   fclose(f);
   return n;
}

static void
save(const char *dir, const char *name, const void *buf, size_t n)
{
   char path[512];
   FILE *f;

   snprintf(path, sizeof(path), "%s/%s", dir, name);
   if (!(f = fopen(path, "wb")) || fwrite(buf, 1, n, f) != n) {
      perror(path);
      exit(2);
   }
   fclose(f);
}

int
main(int argc, char **argv)
{
   static uint32_t payload[256], blk[1024], cmd[1024];
   uint32_t va[SGX_RT_NBUF], *buf[SGX_RT_NBUF], blk_va;
   struct sgx_rt rt;
   size_t n;

   if (argc != 15) {
      fprintf(stderr, "usage: rt-test W H NCORES BLK_VA VA0..VA5 OUTDIR PAYLOAD BLK CMD\n");
      return 2;
   }
   if (!sgx_rt_layout(&rt, atoi(argv[1]), atoi(argv[2]), atoi(argv[3]))) {
      fprintf(stderr, "rt-test: no layout for %sx%s\n", argv[1], argv[2]);
      return 1;
   }
   blk_va = strtoul(argv[4], NULL, 0);
   for (unsigned i = 0; i < SGX_RT_NBUF; i++)
      va[i] = strtoul(argv[5 + i], NULL, 0);

   for (unsigned i = 0; i < SGX_RT_NBUF; i++)
      buf[i] = calloc(1, (rt.size[i] + 0xfff) & ~0xfffu);
   sgx_rt_fill(&rt, va, buf[SGX_RT_DETAILS], buf[SGX_RT_STATE]);
   for (unsigned i = 0; i < SGX_RT_NBUF; i++) {
      char name[16];

      snprintf(name, sizeof(name), "b%u.bin", i);
      save(argv[11], name, buf[i], (rt.size[i] + 0xfff) & ~0xfffu);
      free(buf[i]);
   }

   /* iOS's payload is from w2 on */
   n = load(argv[12], payload + 2, sizeof(payload) - 8);
   sgx_rt_payload(&rt, va, blk_va, payload);
   save(argv[11], "payload.bin", payload + 2, n);
   n = load(argv[13], blk, sizeof(blk));
   sgx_rt_block3d(&rt, va, blk);
   save(argv[11], "blk3d.bin", blk, n);
   n = load(argv[14], cmd, sizeof(cmd));
   sgx_rt_ta_cmd(&rt, va, blk_va, cmd);
   save(argv[11], "cmd.bin", cmd, n);
   return 0;
}
