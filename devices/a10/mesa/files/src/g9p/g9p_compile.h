/* SPDX-License-Identifier: MIT */
/*
 * The A10's (applegpu_g9p) shader compiler: NIR to G9P machine code, with
 * the encodings measured and checked on the GPU in devices/a10/docs/gpu.md
 * "G8.1" (tools/gpu/isa/g9pasm.py is their reference).
 */
#ifndef G9P_COMPILE_H
#define G9P_COMPILE_H

#include <stdbool.h>
#include <stdint.h>

struct nir_shader;

struct g9p_shader {
   uint16_t *code;          /* 16-bit words, the end word at a 64-byte block */
   unsigned size;           /* bytes */
   unsigned nregs;          /* registers r0..nregs-1 (bank 1), the mask stack in r[nregs] */
   unsigned local_size[3];
};

/* compile a compute shader; ssbo_binding[block index] = the buffer binding
 * (its common-store slot), NULL for the identity; NULL or an error message */
const char *g9p_compile(struct nir_shader *nir, bool print, const unsigned *ssbo_binding,
                        struct g9p_shader *out);

/* a G9PT pack (as tools/gpu/g9pshaders.pack) with `name` = the code and
 * `name`.info = {nregs, local size x, y, z} */
int g9p_write_pack(const char *path, const char *name, const struct g9p_shader *s);

#endif
