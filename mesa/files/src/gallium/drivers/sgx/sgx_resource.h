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

/* Every resource is linear: rows of stride bytes, levels and layers one
 * after another.  A texture the GPU samples also gets a twiddled copy, made
 * by the CPU when the linear content has changed since (seq): RGBA8 in
 * Morton order, padded to powers of two (sgx_resource_texture) -- unless
 * it is sampled as it is (sgx_resource_linear()). */
struct sgx_resource {
   struct pipe_resource base;
   struct sgx_bo *bo;
   uint32_t stride[PIPE_MAX_TEXTURE_LEVELS];      /* bytes per row */
   uint32_t layer_size[PIPE_MAX_TEXTURE_LEVELS];  /* bytes per layer or slice */
   uint32_t offset[PIPE_MAX_TEXTURE_LEVELS];      /* of each level */
   uint32_t seq;                                  /* bumped at every write */
   struct sgx_bo *tw;                             /* the twiddled copy */
   uint32_t tw_seq;                               /* the content it was made from */
   unsigned tw_w, tw_h;                           /* its size */
   struct sgx_bo *sampled;                        /* what the last state words point at */
   bool external;                                 /* shared: others may write it */
   bool gpu_written;                              /* a render has written it */
};

static inline struct sgx_resource *
sgx_resource(struct pipe_resource *p)
{
   return (struct sgx_resource *)p;
}

struct sgx_transfer {
   struct pipe_transfer base;
};

/* Whether res is sampled as it is, linear (M16): RGBA8 orders, one level,
 * written by the GPU or by others (a render's target, a shared buffer) --
 * the CPU's copy would read it back each time -- or not a power of two in
 * size (the copy is padded).  Minification is point
 * sampling then.  The sampler reads bytes as B G R A: *swap says the
 * shader swaps red and blue back, *x8 that it takes alpha as 1. */
bool sgx_resource_linear(const struct sgx_resource *res, bool *swap, bool *x8);

/* The four state words the sampler reads for res sampled with ss (M14):
 * the twiddled copy brought up to date first.  False when res cannot be
 * sampled yet (not 2D, no copy). */
bool sgx_resource_texture(struct sgx_screen *screen, struct sgx_resource *res,
                          const struct pipe_sampler_state *ss, uint32_t words[4]);

void sgx_resource_screen_init(struct sgx_screen *screen);
void sgx_resource_context_init(struct pipe_context *pctx);

#endif
