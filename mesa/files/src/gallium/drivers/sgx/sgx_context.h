/*
 * Copyright 2026 Project Cascadia
 * SPDX-License-Identifier: MIT
 */
#ifndef SGX_CONTEXT_H
#define SGX_CONTEXT_H

#include "pipe/p_context.h"
#include "pipe/p_state.h"
#include "util/slab.h"

#include "sgx_compiler.h"
#include "sgx_device.h"
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
   struct draw_fragment_shader *draw_fs; /* a fragment shader's, for the draw module's
                                            point and line stages (M24) */
   struct sgx_fs_colour colour;         /* a fragment shader's colour */
   struct sgx_fs *compiled;             /* a fragment shader, compiled (M13c) */
   struct sgx_fs *variant[8];           /* and again for blend states (M14) */
   unsigned nvariants;
   struct sgx_vs *vs_variant[8];        /* a vertex shader on the GPU, for the varyings
                                           a fragment shader reads (M18) and the
                                           attributes' formats (M26) */
   unsigned nvs;
   unsigned vs_nattrs;                  /* the attributes it reads (sgx_vs_attr_count) */
   bool vs_failed;                      /* it cannot be compiled: the draw module's */
};

/* Draws gathered into one render until something needs it done
 * (sgx_draw.c: sgx_batch_flush): a flush, a map of what it writes or
 * reads, another target, a clear, a full batch. */
struct sgx_batch_draw {
   struct sgx_frame_layout l;
   /* its vertices: the draw module's, floats from first in verts; a
    * vertex shader's the CPU made one stream (M26), bytes from first in
    * vdata -- or none, the fetch reading the application's buffers */
   unsigned first, nverts;
   struct sgx_pixel_program prog;       /* a copy, uploaded */
   bool compiled;
   unsigned sa;                         /* its secondary attributes, from word sa */
   struct sgx_frame_state st;
   struct sgx_vs *vs;                   /* its vertex shader on the GPU (M18), or NULL */
   unsigned vs_sa;                      /* its uniforms, from word vs_sa in sa */
   unsigned idx, nidx;                  /* its indices, from idx in the batch's */
   /* a vertex shader's (M26): where the fetch reads each attribute of
    * vertex 0, the stride, the vertices drawn (nidx of them when it has
    * indices, else 0, 1, 2, ...), the largest index */
   uint32_t base[SGX_VS_MAX_ATTRIBS];
   unsigned stride, count, max_index;
   bool repacked;
};

struct sgx_batch {
   struct pipe_resource *rt;            /* referenced */
   unsigned rt_level, rt_layer;         /* its level and face (M29) */
   struct sgx_batch_draw draw[SGX_FRAME_MAX_DRAWS];
   struct sgx_frame_draw fdraw[SGX_FRAME_MAX_DRAWS];    /* (sgx_batch_flush's) */
   unsigned ndraws;
   unsigned seq;                        /* this one's number, counted from 1 */
   float depth_clear;                   /* what the render's depth starts at */
   struct pipe_resource *zs;            /* the depth buffer, referenced (M24) */
   bool zs_cleared;                     /* its depth cleared at the render's start */
   bool zs_read, zs_written;            /* a draw tests depth, writes it */
   float *verts;
   unsigned nfloats, maxfloats;
   uint8_t *vdata;                      /* vertex shaders' vertices made one stream */
   unsigned nbytes, maxbytes;
   uint32_t *sa;
   unsigned nsa, maxsa;
   uint16_t *idx;
   unsigned nidx, maxidx;
   struct pipe_resource *tex[SGX_FRAME_MAX_HANDLES];   /* sampled, referenced */
   unsigned ntex;
   /* the buffers the render lists for the kernel -- the textures' copies,
    * then buffers' (M26: their GPU copies, referenced in bos[], stamped
    * with seq) -- SGX_FRAME_MAX_HANDLES at most all told */
   uint32_t handles[SGX_FRAME_MAX_HANDLES];
   struct sgx_bo *bos[SGX_FRAME_MAX_HANDLES];
   unsigned nbos;
   /* programs deleted while draws here may use them: retired from the
    * frame and freed at the flush (M24) */
   struct sgx_vs **dead_vs;
   unsigned ndead_vs;
   struct sgx_fs **dead_fs;
   unsigned ndead_fs;
};

struct sgx_vertex_elements {
   unsigned count;
   struct pipe_vertex_element e[PIPE_MAX_ATTRIBS];
   uint8_t attr[PIPE_MAX_ATTRIBS];      /* how the fetch reads each: an SGX_ATTR, 0 if
                                           it cannot (M26) */
};

/* M26: a vertex shader's draw on its way into the batch (sgx_draw.c,
 * gpu_vs_draw to submit): its attributes' addresses for vertex 0 -- in
 * buffers' GPU copies (bos[]), or offsets in a vertex of the nverts the CPU
 * made one stream in vdata -- one stride; the vertices drawn, count of
 * them: from indices[] when indexed, else 0, 1, 2, ... */
struct sgx_vtx {
   uint32_t base[SGX_VS_MAX_ATTRIBS];
   unsigned stride;
   bool repacked;
   unsigned nverts;
   unsigned count, max_index;
   bool indexed;
   struct sgx_bo *bos[SGX_VS_MAX_ATTRIBS];
   unsigned nbos;
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
   /* M18: the vertex shader on the GPU -- its vertices (M26: vtx, vdata),
    * the triangles indices into them, its uniforms */
   struct sgx_vs *gpu_vs;
   struct sgx_vtx vtx;
   uint8_t *vdata;
   unsigned maxvdata;
   uint16_t *indices;
   unsigned maxindices;
   uint32_t vs_sa[128 + SGX_UBUF_MAX];   /* (sa's, then the uniforms in memory, M32) */
   const struct pipe_rasterizer_state *rast;
   const struct sgx_vertex_elements *velems;
   struct pipe_viewport_state viewport;
   struct pipe_scissor_state scissor;
   struct pipe_stencil_ref stencil_ref;
   struct sgx_pixel_program clear_prog;  /* writes nothing: sgx_zs_clear() */
   struct sgx_pixel_program colour_prog; /* o0 = sa0: sgx_colour_clear() */
   uint64_t colour_code[2];
   struct sgx_batch batch;
   const float *fs_constants;
   unsigned fs_constants_size;  /* bytes */
   bool warned_fs, debug_draw;
   /* SGX_DEBUG=fps: frames (swaps), renders and draws counted, and said
    * every two seconds */
   bool debug_fps, debug_sync;
   unsigned stat_frames, stat_renders, stat_draws;
   int64_t stat_t0, stat_cpu0, stat_gpu;
   int64_t stat_swap, stat_between;     /* the last frame's end; to the next draw */
   int64_t stat_swap_cpu, stat_between_cpu;
};

static inline struct sgx_context *
sgx_context(struct pipe_context *p)
{
   return (struct sgx_context *)p;
}

/* whether the gathered draws read bo (a buffer's GPU copy, M26) */
static inline bool
sgx_batch_reads(const struct sgx_context *ctx, const struct sgx_bo *bo)
{
   return ctx->batch.ndraws && bo->batch_ctx == ctx && bo->batch_seq == ctx->batch.seq;
}

struct pipe_context *sgx_context_create(struct pipe_screen *pscreen, void *priv,
                                        unsigned flags);
void sgx_context_screen_init(struct sgx_screen *screen);

#endif
