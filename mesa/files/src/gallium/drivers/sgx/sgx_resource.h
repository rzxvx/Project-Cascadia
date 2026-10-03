/*
 * Copyright 2026 Project Cascadia
 * SPDX-License-Identifier: MIT
 */
#ifndef SGX_RESOURCE_H
#define SGX_RESOURCE_H

#include "pipe/p_state.h"
#include "util/u_transfer.h"

struct sgx_bo;
struct sgx_screen;

/* Every resource is linear for now: rows of stride bytes, levels and
 * layers one after another.  (Textures will be twiddled, M14.) */
struct sgx_resource {
   struct pipe_resource base;
   struct sgx_bo *bo;
   uint32_t stride[PIPE_MAX_TEXTURE_LEVELS];      /* bytes per row */
   uint32_t layer_size[PIPE_MAX_TEXTURE_LEVELS];  /* bytes per layer or slice */
   uint32_t offset[PIPE_MAX_TEXTURE_LEVELS];      /* of each level */
};

static inline struct sgx_resource *
sgx_resource(struct pipe_resource *p)
{
   return (struct sgx_resource *)p;
}

struct sgx_transfer {
   struct pipe_transfer base;
};

void sgx_resource_screen_init(struct sgx_screen *screen);
void sgx_resource_context_init(struct pipe_context *pctx);

#endif
