/* SPDX-License-Identifier: MIT */
#ifndef G9P_CONTEXT_H
#define G9P_CONTEXT_H

#include "pipe/p_context.h"
#include "pipe/p_state.h"
#include "util/slab.h"

#include "g9p/g9p_compile.h"
#include "g9p_device.h"

struct nir_shader;
struct g9p_resource;

#define G9P_MAX_ATTRIBS 16
#define G9P_MAX_TEXTURES 8

struct g9p_variant {
   struct g9p_variant *next;
   uint8_t key[512];
   unsigned key_size;
   bool failed;
   uint32_t code;              /* USC address */
   unsigned nregs;
   unsigned noutputs;          /* vertex: output dwords */
   struct g9p_varyings vary;   /* vertex: the layout; fragment: + interpolation */
};

struct g9p_cso_shader {
   struct nir_shader *nir;
   struct g9p_variant *variants;
};

struct g9p_velems {
   unsigned count;
   struct pipe_vertex_element e[G9P_MAX_ATTRIBS];
};

/* the render being gathered: one target, a clear, the draws' VDM stream */
struct g9p_frame {
   struct pipe_resource *target;
   unsigned level;
   bool cleared;
   float clear[4];
   unsigned ndraws;
   struct g9p_bo *bo;          /* per-render data: stream, PPP blocks, ... */
   uint32_t pos;               /* next free byte of bo */
   uint32_t *stream;           /* the VDM stream (start of bo) */
   unsigned nstream;
};

struct g9p_context {
   struct pipe_context base;
   struct g9p_device *dev;
   struct slab_child_pool transfer_pool;

   struct pipe_framebuffer_state fb;
   struct pipe_vertex_buffer vb[PIPE_MAX_ATTRIBS];
   unsigned nvb;
   struct g9p_velems *velems;
   struct g9p_cso_shader *vs, *fs;
   struct pipe_blend_state *blend;
   struct pipe_rasterizer_state *rast;
   struct pipe_depth_stencil_alpha_state *dsa;
   struct pipe_viewport_state viewport;
   /* constant buffer 0 (the uniforms) per stage: vertex, fragment */
   struct pipe_constant_buffer cb[2];
   /* the fragment shader's textures */
   struct pipe_sampler_view *views[G9P_MAX_TEXTURES];
   struct pipe_sampler_state *samplers[G9P_MAX_TEXTURES];

   struct g9p_frame frame;
};

static inline struct g9p_context *
g9p_context(struct pipe_context *p)
{
   return (struct g9p_context *)p;
}

struct pipe_context *g9p_context_create(struct pipe_screen *pscreen, void *priv, unsigned flags);
void g9p_context_screen_init(struct pipe_screen *pscreen);

/* send the gathered render and wait for it */
void g9p_flush_frame(struct g9p_context *ctx);

#endif
