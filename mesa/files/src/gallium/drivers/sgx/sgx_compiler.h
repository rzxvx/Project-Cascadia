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

/* What a variant of a fragment shader is compiled for.  GL's blending for
 * render target 0, as the shader does it (M14): equations and factors
 * (pipe_blend_func, pipe_blendfactor), the colour mask.  And the texture
 * units sampled linear (sgx_resource_linear(), M16) from memory in B G R A
 * order, and with no alpha.  All zero but colormask 0xf: nothing done. */
struct sgx_blend_key {
   uint8_t enable;
   uint8_t rgb_func, rgb_src, rgb_dst;
   uint8_t alpha_func, alpha_src, alpha_dst;
   uint8_t colormask;
   uint8_t tex_bgra, tex_x8;
};

#define SGX_FS_MAX_SAMPLERS 8

/* A compiled fragment shader.  Its secondary attributes (prog.nsa words)
 * are constant buffer 0's words 0..nuniforms, then from sampler_sa four
 * state words for each texture it samples, sampler_unit[i] the unit of the
 * i-th, then (blend_sa, if the blending reads it) the blend colour. */
struct sgx_fs {
   struct sgx_pixel_program prog;
   unsigned input_slot[SGX_FRAME_MAX_VARYINGS];   /* gl_varying_slot, input i */
   unsigned nuniforms, nsamplers, sampler_sa;
   unsigned sampler_unit[SGX_FS_MAX_SAMPLERS];
   int blend_sa;                                  /* -1: no blend colour */
   struct sgx_blend_key blend;                    /* what it was compiled for */
};

/* fs is not changed; blend NULL for none.  NULL, and why in why[], when
 * it cannot be compiled (yet). */
struct sgx_fs *sgx_compile_fs(const struct nir_shader *fs, const struct sgx_blend_key *blend,
                              char *why, unsigned why_size);
void sgx_fs_destroy(struct sgx_fs *fs);

#endif
