/*
 * Copyright 2026 Project Cascadia
 * SPDX-License-Identifier: MIT
 */
#ifndef SGX_CONTEXT_H
#define SGX_CONTEXT_H

#include "pipe/p_context.h"
#include "pipe/p_state.h"
#include "util/slab.h"

struct sgx_fence;
struct sgx_screen;

struct sgx_shader {
   struct nir_shader *nir;
};

struct sgx_context {
   struct pipe_context base;
   struct slab_child_pool transfer_pool;
   struct pipe_framebuffer_state fb;
   struct pipe_vertex_buffer vb[PIPE_MAX_ATTRIBS];
   uint32_t vb_mask;
   struct sgx_fence *last;      /* the last render this context submitted */
   bool warned_draw;
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
