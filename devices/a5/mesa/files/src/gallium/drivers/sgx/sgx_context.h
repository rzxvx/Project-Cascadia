/*
 * Copyright 2026 Project Cascadia
 * SPDX-License-Identifier: MIT
 */
#ifndef SGX_CONTEXT_H
#define SGX_CONTEXT_H

#include "pipe/p_context.h"
#include "util/u_debug.h"
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
    * vertex shader's the CPU made one stream (M26), at vdata_va in one of
    * the batch's vertex chunks (M42) -- or none, the fetch reading the
    * application's buffers */
   unsigned first, nverts;
   uint32_t vdata_va;
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

/* An occlusion query (M37): the renders made while it was active, each
 * with a visibility buffer of its own -- the samples its draws passed, a
 * word a core (sgx_frame_render's vis) -- and what those read already came
 * to.  A render goes at each begin and end: the ISP has one count. */
#define SGX_QUERY_BUFS 16
struct sgx_query {
   unsigned type;                       /* PIPE_QUERY_OCCLUSION_* */
   struct sgx_bo *bo[SGX_QUERY_BUFS];
   unsigned nbo;
   uint64_t samples;
   /* the last render made before its end: its result is there when every
    * render before is done, its own or not (piglit's occlusion_query_order:
    * a query with no draws after others) */
   struct sgx_fence *last;
   /* (M43) TIMESTAMP and TIME_ELAPSED: the render before each end (and its
    * call's time), the GPU's time there the later of the two */
   struct sgx_fence *begin_fence;
   uint64_t begin_cpu, end_cpu;
};

/* a render's visibility buffer for q, busy with it (sgx_batch_flush) */
void sgx_query_add(struct sgx_query *q, struct sgx_bo *bo);
/* whether rendering goes ahead under the render condition (M43) */
bool sgx_render_condition_ok(struct sgx_context *ctx);

#define SGX_BATCH_VCHUNKS 40
#define SGX_BATCH_VCHUNK  (256u << 10)

struct sgx_batch {
   struct pipe_resource *rt;            /* referenced */
   unsigned rt_level, rt_layer;         /* its level and face (M29) */
   unsigned w, h;                       /* its area (M43: sgx_frame_target) */
   struct sgx_batch_draw draw[SGX_FRAME_MAX_DRAWS];
   struct sgx_frame_draw fdraw[SGX_FRAME_MAX_DRAWS];    /* (sgx_batch_flush's) */
   unsigned ndraws;
   unsigned seq;                        /* this one's number, counted from 1 */
   float depth_clear;                   /* what the render's depth starts at */
   struct pipe_resource *zs;            /* the depth buffer, referenced (M24) */
   unsigned zs_level, zs_layer;         /* its level and layer (M43) */
   bool zs_cleared;                     /* its depth cleared at the render's start */
   bool zs_read, zs_written;            /* a draw tests depth, writes it */
   bool counted;                        /* a draw's samples counted (M37) */
   float *verts;
   unsigned nfloats, maxfloats;
   /* vertex shaders' vertices made one stream (M26), written there by
    * gpu_vs_draw straight into GPU memory (M42: once, not copied twice
    * more) -- chunks from the buffer cache, listed for the render and busy
    * with it; vat the next byte in the last; nbytes all of them told */
   struct sgx_bo *vchunk[SGX_BATCH_VCHUNKS];
   unsigned nvchunks;
   uint32_t vat;
   unsigned nbytes;
   uint32_t *sa;
   unsigned nsa, maxsa;
   uint16_t *idx;
   unsigned nidx, maxidx;
   struct pipe_resource *tex[SGX_FRAME_MAX_HANDLES];   /* sampled, referenced */
   /* the buffer each was sampled from (its copy then, M37: a copy remade
    * while draws read the old one is a new buffer -- the same texture
    * twice), referenced, and busy with the render once it is made */
   struct sgx_bo *tex_bo[SGX_FRAME_MAX_HANDLES];
   unsigned ntex;
   uint64_t tex_bytes;                  /* the memory they hold (SGX_BATCH_TEX_BYTES) */
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
   /* the buffers the draw reads: its attributes', and (M43) its uniform
    * buffers', both stages' */
   struct sgx_bo *bos[SGX_VS_MAX_ATTRIBS + 2 * SGX_FRAME_MAX_UBOS];
   unsigned nbos;
   /* repacked (M42): where the stream was written -- the batch's chunk
    * vchunk (referenced), vdata for the CPU, vdata_va for the GPU -- when
    * the batch was number vseq (a flush since: written again, there) */
   struct sgx_bo *vchunk;
   uint8_t *vdata;
   uint32_t vdata_va;
   unsigned vseq;
};

struct sgx_context {
   struct pipe_context base;
   struct slab_child_pool transfer_pool;
   struct pipe_framebuffer_state fb;
   struct pipe_vertex_buffer vb[PIPE_MAX_ATTRIBS];
   uint32_t vb_mask;
   struct pipe_constant_buffer cb[2];   /* buffer 0: the vertex, fragment shader's */
   /* (M43) the uniform buffers, 1..15, of the vertex and fragment shader */
   struct pipe_constant_buffer ubo[2][SGX_FRAME_MAX_UBOS];
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
   /* a framebuffer of depth and stencil alone: the colour the tiles end in
    * goes here (M35), the depth buffer's size */
   struct pipe_resource *no_colour;

   /* draws (sgx_draw.c): the draw module, its back end, and the vertices
    * gathered for the template frame */
   struct draw_context *draw;
   struct draw_stage *vbuf;
   struct vbuf_render *render;
   struct sgx_frame_layout layout;      /* of verts, for the bound shaders */
   float *verts;
   unsigned nverts, maxfloats;
   /* M18: the vertex shader on the GPU -- its vertices (M26: vtx; M42: a
    * stream the CPU makes in the batch's vertex chunks), the triangles
    * indices into them, its uniforms */
   struct sgx_vs *gpu_vs;
   struct sgx_vtx vtx;
   uint16_t *indices;
   unsigned maxindices;
   uint32_t vs_sa[128 + SGX_UBUF_MAX];   /* (sa's, then the uniforms in memory, M32) */
   const struct pipe_rasterizer_state *rast;
   const struct sgx_vertex_elements *velems;
   struct pipe_viewport_state viewport;
   bool depth_clamp;            /* the program's rasterizer state's (M43) */
   struct pipe_scissor_state scissor;
   struct pipe_stencil_ref stencil_ref;
   struct sgx_pixel_program clear_prog;  /* writes nothing: sgx_zs_clear() */
   struct sgx_pixel_program colour_prog; /* o0 = sa0: sgx_colour_clear() */
   uint64_t colour_code[2];
   /* (M43) the colour clear's program for each kind of float tile */
   struct sgx_pixel_program colour_float_prog[SGX_RT_F32X2 + 1];
   uint64_t colour_float_code[SGX_RT_F32X2 + 1][2];
   struct sgx_batch batch;
   struct sgx_query *query;             /* the active occlusion query (M37) */
   bool queries_off;                    /* (set_active_query_state: meta ops) */
   /* (M43) conditional rendering (NV_conditional_render): the query whose
    * result decides, render_condition()'s condition and mode */
   struct pipe_query *cond_query;
   bool cond_condition;
   enum pipe_render_cond_flag cond_mode;
   const float *fs_constants;
   unsigned fs_constants_size;  /* bytes */
   bool warned_fs, debug_draw;
   /* SGX_DEBUG=fps: frames (swaps), renders and draws counted, and said
    * every two seconds */
   bool debug_fps, debug_sync, debug_flushes;
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

/* what the rasterizer state's polygon offset units are times GL's, for
 * the draw module (sgx_create_rs_state): 4, or with the ISP's offset
 * (SGX_ISP_OFFSET=1, M37) 16 -- its unit, 32 of 24-bit depth's at depths
 * 0.5..1, the draw module doubling it -- the same on both vertex paths */
static inline float
sgx_offset_scale(void)
{
   static int isp = -1;

   if (isp < 0)
      isp = debug_get_bool_option("SGX_ISP_OFFSET", false);
   return isp ? 16.0f : 4.0f;
}
#define SGX_OFFSET_SCALE sgx_offset_scale()

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
