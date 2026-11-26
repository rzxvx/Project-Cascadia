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

#define G9P_MAX_VARYINGS 32

/* the varyings between a vertex and a fragment shader: the vertex shader's
 * compile fills slot / comps / offset (output slots 4 + offset on, in
 * location order), the fragment shader's compile reads them and fills
 * persp */
struct g9p_varyings {
   unsigned n;
   unsigned slot[G9P_MAX_VARYINGS];     /* the VARYING_SLOT_* location */
   unsigned comps[G9P_MAX_VARYINGS];
   unsigned offset[G9P_MAX_VARYINGS];   /* the first component's index */
   bool persp[G9P_MAX_VARYINGS];
   bool flat[G9P_MAX_VARYINGS];         /* one vertex's value (the driver picks which) */
   unsigned total;                      /* components */
};

/* blending, done by the fragment shader (pipe_blend_func /
 * pipe_blendfactor values, the target's pipe_format) */
struct g9p_blend {
   unsigned rgb_func, rgb_src, rgb_dst;
   unsigned alpha_func, alpha_src, alpha_dst;
   unsigned format;
};

/* a vertex attribute as the Gallium driver lays it out: its address in
 * common-store dwords 2i, 2i + 1 (i = the load's base), `comps` f32
 * components `stride` bytes apart per vertex */
struct g9p_vertex_attrib {
   unsigned stride;
   unsigned comps;
};

struct g9p_options {
   const unsigned *ssbo_binding;        /* block index -> binding (its common-store slot), NULL: identity */
   struct g9p_varyings *varyings;       /* vertex and fragment shaders */
   unsigned nattribs;                   /* vertex: the buffer's stride in vec4s, 0: the attributes read */
   const struct g9p_blend *blend;       /* fragment: NULL = none */
   unsigned colormask;                  /* fragment, with blend: RGBA bits */
   const struct g9p_vertex_attrib *attribs;   /* vertex: by load base; NULL: g9p-cc's one buffer */
   unsigned num_attribs;
   /* the common store as the driver fills it: constant buffer 0 (the
    * uniforms) from dword ubo_dword, ubo_dwords of it; fragment: the
    * texture states from dword 0 (8 each), the samplers from
    * sampler_dword (2 each; 0: after the states) */
   unsigned ubo_dword, ubo_dwords;
   unsigned sampler_dword;
   /* vertex: GL's clip-space depth (-w..w) made the hardware's (0..w):
    * z = (z + w) / 2 */
   bool clip_halfz;
};

struct g9p_shader {
   unsigned stage;          /* mesa_shader_stage */
   uint16_t *code;          /* 16-bit words */
   unsigned size;           /* bytes */
   unsigned nregs;          /* registers r0..nregs-1 (bank 1; fragment: bank 0), the mask stack in r[nregs] when used */
   unsigned local_size[3];
   unsigned noutputs;       /* vertex: output dwords (4 + the varyings' components) */
   unsigned nattribs;       /* vertex: vec4 attributes per vertex (the interleaved buffer's stride / 16) */
};

/* compile a compute, vertex or fragment shader; NULL or an error message */
const char *g9p_compile(struct nir_shader *nir, bool print, const struct g9p_options *opts,
                        struct g9p_shader *out);

/* the background object's fragment program: every pixel the colour (RGBA,
 * as f16 halves); returns its size in bytes (<= 128) */
unsigned g9p_clear_program(uint16_t *code, const float rgba[4]);

/* a G9PT pack (as tools/gpu/g9pshaders.pack) of n entries: names[i] = data[i] */
int g9p_write_entries(const char *path, unsigned n, const char *const *names, const void *const *data,
                      const unsigned *sizes);

/* a G9PT pack with `name` = the code and `name`.info = {nregs, local size x, y, z} */
int g9p_write_pack(const char *path, const char *name, const struct g9p_shader *s);

#endif
