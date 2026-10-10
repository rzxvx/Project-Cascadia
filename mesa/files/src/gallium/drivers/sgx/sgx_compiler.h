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
 * units sampled linear (sgx_resource_linear(), M16) from memory in R G B A
 * order (red and blue swapped back), and with no alpha.  All zero but
 * colormask 0xf: nothing done. */
struct sgx_blend_key {
   uint8_t enable;
   uint8_t rgb_func, rgb_src, rgb_dst;
   uint8_t alpha_func, alpha_src, alpha_dst;
   uint8_t colormask;
   uint8_t tex_swap, tex_x8;
   uint8_t front_ccw;      /* gallium's, for gl_FrontFacing (0 when not read) */
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

/* A compiled vertex shader (docs/research/p105-mesa.md, M18): attribute n
 * (vertex element n) in pa4n.. as attr[n] says, uniforms (constant buffer
 * 0's words) in sa0.., the position and then the varyings varying_slot[]
 * -- the fragment shader's inputs, in its order -- out through o0.. to the
 * tiler. */
#define SGX_VS_MAX_ATTRIBS 16

/* How an attribute comes (M26), SGX_ATTR(kind, components): the vertex
 * fetch puts its words in pa4n.. as they are in memory, and the program
 * makes them four floats first, the components missing 0, 0, 1 */
enum sgx_attr_kind {
   SGX_ATTR_F32,        /* n F32 words */
   SGX_ATTR_U8N,        /* one word, n unsigned normalized bytes */
   SGX_ATTR_CONST,      /* not fetched: four F32 words in sa, from attr_sa[n] */
};
#define SGX_ATTR(kind, n)    ((kind) << 4 | (n))
#define SGX_ATTR_KIND(a)     ((a) >> 4)
#define SGX_ATTR_COMPS(a)    ((a) & 0xf)

/* the words the fetch reads for one */
static inline unsigned
sgx_attr_words(uint8_t a)
{
   return SGX_ATTR_KIND(a) == SGX_ATTR_F32 ? SGX_ATTR_COMPS(a) : 1;
}

struct sgx_vs {
   uint64_t *code;
   unsigned ncode, ntemps;
   unsigned nattrs, nuniforms;
   /* what it was compiled for: each attribute's SGX_ATTR (four words to
    * compare) and, a constant's it reads, its sa words (0xff: none) */
   union {
      uint8_t attr[SGX_VS_MAX_ATTRIBS];
      uint32_t attr_words[SGX_VS_MAX_ATTRIBS / 4];
   };
   uint8_t attr_sa[SGX_VS_MAX_ATTRIBS];
   /* the uniform words a, b for x and for y, x' = a x + b w: the TA clips
    * where the draw wants (sgx_draw.c) */
   unsigned clip_sa;
   /* uniform words in memory after the nuniforms of sa (M32), their
    * address less 4 in sa word ubuf_sa -- as a pixel program's */
   unsigned nubuf, ubuf_sa;
   unsigned varying_slot[SGX_FRAME_MAX_VARYINGS];
   unsigned nvaryings;
   bool branches;                   /* the code branches (M22) */
   uint32_t code_va;                /* where the frame put it */
};

/* the attributes (vertex elements) a vertex shader reads, all told */
unsigned sgx_vs_attr_count(const struct nir_shader *vs);
/* attr: an SGX_ATTR for each of the sgx_vs_attr_count() attributes */
struct sgx_vs *sgx_compile_vs(const struct nir_shader *vs, const unsigned *varying_slot,
                              unsigned nvaryings, const uint8_t *attr, char *why,
                              unsigned why_size);
void sgx_vs_destroy(struct sgx_vs *vs);

/* fs is not changed; blend NULL for none.  NULL, and why in why[], when
 * it cannot be compiled (yet). */
struct sgx_fs *sgx_compile_fs(const struct nir_shader *fs, const struct sgx_blend_key *blend,
                              char *why, unsigned why_size);
void sgx_fs_destroy(struct sgx_fs *fs);

#endif
