/*
 * Copyright 2026 Project Cascadia
 * SPDX-License-Identifier: MIT
 *
 * The shader compiler, NIR to USSE (docs/research/p105-mesa.md, M13c).  So
 * far fragment shaders only, and of them the ones that are one block of
 * float arithmetic once NIR has unrolled loops and turned ifs into
 * selects: varyings, uniforms, constants, the ALU, comparisons, selects,
 * 2D texture lookups.  Discard, gl_FragCoord and real control flow are not
 * there yet;
 * a shader that needs them is not compiled, and the draw falls back to M13a
 * (sgx_draw.c reads its colour).
 */
#ifndef SGX_COMPILER_H
#define SGX_COMPILER_H

#include <stdbool.h>

#include "sgx_frame.h"

struct nir_shader;

#define SGX_FS_MAX_SAMPLERS 8

/* A compiled fragment shader.  Its secondary attributes (prog.nsa words)
 * are constant buffer 0's words 0..nuniforms, then from sampler_sa four
 * state words for each texture it samples, sampler_unit[i] the unit of the
 * i-th. */
struct sgx_fs {
   struct sgx_pixel_program prog;
   unsigned input_slot[SGX_FRAME_MAX_VARYINGS];   /* gl_varying_slot, input i */
   unsigned nuniforms, nsamplers, sampler_sa;
   unsigned sampler_unit[SGX_FS_MAX_SAMPLERS];
};

/* fs is not changed.  NULL, and why in why[], when it cannot be compiled
 * (yet). */
struct sgx_fs *sgx_compile_fs(const struct nir_shader *fs, char *why, unsigned why_size);
void sgx_fs_destroy(struct sgx_fs *fs);

#endif
