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
   struct sgx_fence *last;      /* the last render this context submitted */

   /* draws (sgx_draw.c): the draw module, its back end, and the vertices
    * gathered for the template frame */
   struct draw_context *draw;
   struct draw_stage *vbuf;
   struct vbuf_render *render;
   struct sgx_frame_layout layout;      /* of verts, for the bound shaders */
   float *verts;
   unsigned nverts, maxfloats;
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
