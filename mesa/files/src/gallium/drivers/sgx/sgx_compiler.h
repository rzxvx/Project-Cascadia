/*
 * Copyright 2026 Project Cascadia
 * SPDX-License-Identifier: MIT
 *
 * The shader compiler, NIR to USSE (docs/research/p105-mesa.md, M13c).  So
 * far fragment shaders only, and of them the ones that are one block of
 * float arithmetic once NIR has unrolled loops and turned ifs into
 * selects: varyings, uniforms, constants, the ALU, comparisons, selects.
 * Textures, discard, gl_FragCoord and real control flow are not there yet;
 * a shader that needs them is not compiled, and the draw falls back to M13a
 * (sgx_draw.c reads its colour).
 */
#ifndef SGX_COMPILER_H
#define SGX_COMPILER_H

#include <stdbool.h>

#include "sgx_frame.h"

struct nir_shader;

struct sgx_fs {
   struct sgx_pixel_program prog;
   unsigned input_slot[SGX_FRAME_MAX_VARYINGS];   /* gl_varying_slot, input i */
};

/* fs is not changed.  NULL, and why in why[], when it cannot be compiled
 * (yet). */
struct sgx_fs *sgx_compile_fs(const struct nir_shader *fs, char *why, unsigned why_size);
void sgx_fs_destroy(struct sgx_fs *fs);

#endif
