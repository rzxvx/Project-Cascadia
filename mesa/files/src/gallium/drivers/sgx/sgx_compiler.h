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
 * (pipe_blend_func, pipe_blendfactor), the colour mask, or a logic op
 * (M37).  And the texture units sampled linear (sgx_resource_linear(),
 * M16) from memory in R G B A order (red and blue swapped back), and with
 * no alpha; those whose texels
 * are sRGB, decoded after the lookup (GL 2.1's EXT_texture_sRGB: no gamma
 * of the sampler's found, M35).  All zero but colormask 0xf: nothing
 * done. */
struct sgx_blend_key {
   uint8_t enable;
   uint8_t rgb_func, rgb_src, rgb_dst;
   uint8_t alpha_func, alpha_src, alpha_dst;
   uint8_t colormask;
   uint8_t tex_swap, tex_x8, tex_srgb;
   uint8_t front_ccw;      /* gallium's, for gl_FrontFacing (0 when not read) */
   uint8_t logicop;        /* pipe_logicop + 1; 0: none (M37: iOS's are the program's) */
   /* (M37) the units whose texture is depth -- its texel F32, raw, as d,
    * 0, 0, 1 through its view's swizzle (GL's depth texture modes) -- and
    * of those the ones compared (shadow samplers: in the program, as iOS's
    * m07; nir_lower_tex_shadow), each one's func */
   uint8_t tex_depth, tex_shadow;
   uint8_t shadow_func[8];
   uint8_t depth_swizzle[8][4];
   /* (M43) the other units whose view swizzles the texel (EXT_texture_swizzle,
    * GL 3.3): pipe_swizzle each, after the texture unit's own order */
   uint8_t tex_swz;
   uint8_t tex_swizzle[8][4];
   /* (M43) the units whose copy is half floats (sgx_resource_copy_f16):
    * sampled as F32, not as bytes; not by the PDS */
   uint8_t tex_f16;
   /* (M43) what the target's tile holds (enum sgx_rt_kind: RGBA bytes,
    * half floats, F32): the colour packed so, the tile read back so */
   uint8_t rt_kind;
   uint8_t rt_srgb;        /* (M43) the target's bytes sRGB: encoded, decoded to blend */
   uint8_t rt_intensity;   /* (M43) an intensity target: the colour R R R R in the tile */
};

#define SGX_FS_MAX_SAMPLERS 8

/* (M39) what the vertex side does to a varying that only the PDS's
 * lookups read, so that its x and y are what the texture unit takes: x
 * and y times a rectangle texture's 1 / width, 1 / height
 * (SGX_FIX_SCALE; the texture unit's in vary_unit[]), and the divisor of
 * a projective lookup -- the program's z or w (SGX_FIX_DIV_Z, _W) --
 * into w, which the PDS's projective lookup divides by (glproj: not z;
 * iOS's s08 has a vec3's z there, its vertex program putting it in w). */
#define SGX_FIX_SCALE   0x10
#define SGX_FIX_DIV_Z   0x02
#define SGX_FIX_DIV_W   0x03
#define SGX_FIX_DIV(f)  ((f) & 3)
#define SGX_PROJ_DIV    3       /* the component the PDS divides x and y by */

/* A compiled fragment shader.  Its secondary attributes (prog.nsa words)
 * are constant buffer 0's words 0..nuniforms, then from sampler_sa four
 * state words for each texture it samples, sampler_unit[i] the unit of the
 * i-th, then (scale_sa) two words for each, 1 / its width and height, if a
 * rectangle texture is sampled (M35), then (blend_sa, if the blending reads
 * it) the blend colour. */
struct sgx_fs {
   struct sgx_pixel_program prog;
   unsigned input_slot[SGX_FRAME_MAX_VARYINGS];   /* gl_varying_slot, input i */
   /* the varyings the vertex side hands over, in the layout's order
    * (prog.nvaryings): the inputs' but gl_FragCoord, then those only the
    * PDS's lookups read (M35) */
   unsigned varying_slot[SGX_FRAME_MAX_VARYINGS];
   unsigned nuniforms, nsamplers, sampler_sa;
   unsigned sampler_unit[SGX_FS_MAX_SAMPLERS];
   int blend_sa;                                  /* -1: no blend colour */
   int scale_sa;              /* -1, or each texture's 1 / width, 1 / height (rectangles) */
   struct sgx_blend_key blend;                    /* what it was compiled for */
   /* (M39) each varying's SGX_FIX_*, in varying_slot[]'s order */
   uint8_t vary_fix[SGX_FRAME_MAX_VARYINGS];
   uint8_t vary_unit[SGX_FRAME_MAX_VARYINGS];
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
   /* (M43) its uniform buffers, as a pixel program's (ubo_mask, ubo_sa) */
   uint16_t ubo_mask;
   uint8_t ubo_sa[SGX_FRAME_MAX_UBOS];
   unsigned varying_slot[SGX_FRAME_MAX_VARYINGS];
   unsigned nvaryings;
   /* (M39) the fragment shader's SGX_FIX_* for each varying, and from sa
    * word fix_sa two words for each one SGX_FIX_SCALE, the draw's */
   uint8_t vary_fix[SGX_FRAME_MAX_VARYINGS];
   unsigned fix_sa;
   bool branches;                   /* the code branches (M22) */
   uint32_t code_va;                /* where the frame put it */
};

/* the attributes (vertex elements) a vertex shader reads, all told */
unsigned sgx_vs_attr_count(const struct nir_shader *vs);
/* attr: an SGX_ATTR for each of the sgx_vs_attr_count() attributes; fix:
 * each varying's SGX_FIX_* (sgx_fs.vary_fix), NULL none */
struct sgx_vs *sgx_compile_vs(const struct nir_shader *vs, const unsigned *varying_slot,
                              unsigned nvaryings, const uint8_t *attr, const uint8_t *fix,
                              char *why, unsigned why_size);
void sgx_vs_destroy(struct sgx_vs *vs);

/* fs is not changed; blend NULL for none.  NULL, and why in why[], when
 * it cannot be compiled (yet). */
struct sgx_fs *sgx_compile_fs(const struct nir_shader *fs, const struct sgx_blend_key *blend,
                              char *why, unsigned why_size);
void sgx_fs_destroy(struct sgx_fs *fs);

#endif
