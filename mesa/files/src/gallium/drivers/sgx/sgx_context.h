/*
 * Copyright 2026 Project Cascadia
 * SPDX-License-Identifier: MIT
 */
#ifndef SGX_CONTEXT_H
#define SGX_CONTEXT_H

#include "pipe/p_context.h"
#include "pipe/p_state.h"
#include "util/slab.h"

#include "sgx_draw.h"
#include "sgx_frame.h"

struct draw_context;
struct draw_stage;
struct draw_vertex_shader;
struct sgx_fence;
struct sgx_screen;
struct vbuf_render;

struct sgx_shader {
   struct nir_shader *nir;
   struct draw_vertex_shader *draw;     /* a vertex shader's, on the CPU */
   struct sgx_fs_colour colour;         /* a fragment shader's colour */
   struct sgx_fs *compiled;             /* a fragment shader, compiled (M13c) */
   struct sgx_fs *variant[8];           /* and again for blend states (M14) */
   unsigned nvariants;
   struct sgx_vs *vs_variant[4];        /* a vertex shader on the GPU, for the varyings
                                           a fragment shader reads (M18) */
   unsigned nvs;
   bool vs_failed;                      /* it cannot be compiled: the draw module's */
};

/* Draws gathered into one render until something needs it done
 * (sgx_draw.c: sgx_batch_flush): a flush, a map of what it writes or
 * reads, another target, a clear, a full batch. */
struct sgx_batch_draw {
   struct sgx_frame_layout l;
   unsigned first, nverts;              /* its vertices: floats from first in verts */
   struct sgx_pixel_program prog;       /* a copy, uploaded */
   bool compiled;
   unsigned sa;                         /* its secondary attributes, from word sa */
   struct sgx_frame_state st;
   struct sgx_vs *vs;                   /* its vertex shader on the GPU (M18), or NULL */
   unsigned vs_sa;                      /* its uniforms, from word vs_sa in sa */
   unsigned idx, nidx;                  /* its indices, from idx in the batch's */
};

struct sgx_batch {
   struct pipe_resource *rt;            /* referenced */
   struct sgx_batch_draw draw[SGX_FRAME_MAX_DRAWS];
   unsigned ndraws, cursor;             /* cursor: sgx_frame_place's */
   float depth_clear;                   /* what the render's depth starts at */
   float *verts;
   unsigned nfloats, maxfloats;
   uint32_t *sa;
   unsigned nsa, maxsa;
   uint16_t *idx;
   unsigned nidx, maxidx;
   struct pipe_resource *tex[SGX_FRAME_MAX_HANDLES];   /* sampled, referenced */
   uint32_t handles[SGX_FRAME_MAX_HANDLES];           /* their twiddled copies */
   unsigned ntex;
};

struct sgx_vertex_elements {
   unsigned count;
   struct pipe_vertex_element e[PIPE_MAX_ATTRIBS];
};

struct sgx_context {
   struct pipe_context base;
   struct slab_child_pool transfer_pool;
   struct pipe_framebuffer_state fb;
   struct pipe_vertex_buffer vb[PIPE_MAX_ATTRIBS];
   uint32_t vb_mask;
   struct pipe_constant_buffer cb[2];   /* buffer 0: the vertex, fragment shader's */
   struct sgx_shader *vs, *fs;
   /* blending, depth (M14) */
   const struct pipe_blend_state *blend;
   const struct pipe_depth_stencil_alpha_state *dsa;
   struct pipe_blend_color blend_color;
   struct sgx_fs *draw_fs;              /* this draw's fragment shader (sgx_draw.c) */
   /* the fragment shader's textures (M14) */
   const struct pipe_sampler_state *fs_samplers[PIPE_MAX_SAMPLERS];
   struct pipe_sampler_view *fs_views[PIPE_MAX_SHADER_SAMPLER_VIEWS];
   struct sgx_fence *last;      /* the last render this context submitted */

   /* draws (sgx_draw.c): the draw module, its back end, and the vertices
    * gathered for the template frame */
   struct draw_context *draw;
   struct draw_stage *vbuf;
   struct vbuf_render *render;
   struct sgx_frame_layout layout;      /* of verts, for the bound shaders */
   float *verts;
   unsigned nverts, maxfloats;
   /* M18: the vertex shader on the GPU -- verts its attributes, the
    * triangles indices into them, its uniforms */
   struct sgx_vs *gpu_vs;
   uint16_t *indices;
   unsigned nindices, maxindices;
   uint32_t vs_sa[128];
   const struct pipe_rasterizer_state *rast;
   const struct sgx_vertex_elements *velems;
   struct pipe_viewport_state viewport;
   struct sgx_batch batch;
   const float *fs_constants;
   unsigned fs_constants_size;  /* bytes */
   bool warned_fs, debug_draw;
};

static inline struct sgx_context *
sgx_context(struct pipe_context *p)
{
   return (struct sgx_context *)p;
}

struct pipe_context *sgx_context_create(struct pipe_screen *pscreen, void *priv,
                                        unsigned flags);
void sgx_context_screen_init(struct sgx_screen *screen);

#endif
