/*
 * Copyright 2026 Project Cascadia
 * SPDX-License-Identifier: MIT
 */
#ifndef SGX_RESOURCE_H
#define SGX_RESOURCE_H

#include "pipe/p_state.h"
#include "util/simple_mtx.h"
#include "util/u_transfer.h"

struct sgx_bo;
struct sgx_context;
struct sgx_screen;

/* Every resource is linear: rows of stride bytes, levels and layers one
 * after another.  A texture the GPU samples also gets a twiddled copy, made
 * by the CPU when the linear content has changed since (seq): every level
 * and face, in Morton order, padded to powers of two, as the sampler
 * lays them out (M28: sgx_resource_texture) -- its texels as they are for
 * the formats the sampler has (M37: native_format()), else RGBA8 -- unless
 * it is sampled as it is (sgx_resource_linear()). */
struct sgx_resource {
   struct pipe_resource base;
   struct sgx_bo *bo;
   /* a buffer's (M26): data is the CPU's copy, what maps see; bo the
    * GPU's, made at the first draw that reads the buffer from there, and
    * behind data by the bytes [dirty_lo, dirty_hi) -- written since by maps
    * that were synchronized too if dirty_sync */
   uint8_t *data;
   uint32_t dirty_lo, dirty_hi;
   bool dirty_sync;
   /* (M34) the maps that stay while draws read it -- glthread's uploads, in
    * the application's thread: each draw takes what it reads of data as it
    * is -- and the lock the dirty bytes are under (both threads') */
   int live_maps;
   simple_mtx_t dirty_lock;
   uint32_t stride[PIPE_MAX_TEXTURE_LEVELS];      /* bytes per row */
   uint32_t layer_size[PIPE_MAX_TEXTURE_LEVELS];  /* bytes per layer or slice */
   uint32_t offset[PIPE_MAX_TEXTURE_LEVELS];      /* of each level */
   uint32_t seq;                                  /* bumped at every write */
   struct sgx_bo *tw;                             /* the twiddled copy */
   uint32_t tw_seq;                               /* the content it was made from */
   unsigned tw_w, tw_h;                           /* its size (linear: the stride's) */
   unsigned tw_levels;                            /* its levels (of each face, M28) */
   unsigned tw_first;                             /* the level its level 0 is (M35) */
   /* (M37) the writes since the copy was made, when every one of them was
    * a box of level 0 of a texture of one level (tw_writes of the seq
    * bumps since tw_seq): that box alone made again, in the CPU's shadow
    * of the copy (tw_cpu, cached memory) and from there into the copy --
    * a new one when a render may read the old (copy on write) */
   unsigned tw_writes;
   unsigned tw_x0, tw_y0, tw_x1, tw_y1;
   uint8_t *tw_cpu;
   struct sgx_bo *sampled;                        /* what the last state words point at */
   bool external;                                 /* shared: others may write it */
   bool gpu_written;                              /* a render has written it */
   /* a depth buffer's tiles (M24), each level's face or layer its own
    * (M43: a depth texture's levels rendered to), level * array_size +
    * layer; made at their first use */
   struct sgx_zls *zls;
   unsigned nzls;
};

static inline struct sgx_resource *
sgx_resource(struct pipe_resource *p)
{
   return (struct sgx_resource *)p;
}

/* The ISP's tiles of one level and layer of a depth buffer, as a render
 * stores them (M24) */
struct sgx_zls {
   struct sgx_bo *bo;
   bool valid;                                    /* a render has stored them */
   bool cleared;                                  /* cleared since, to clear */
   float clear;
};

/* a level's layer's tiles (state made the first time, the buffer not), or
 * NULL when there is no memory for the state */
struct sgx_zls *sgx_resource_zls(struct sgx_resource *res, unsigned level, unsigned layer);

/* whether p's copy for the sampler is four half floats (M43) */
bool sgx_resource_copy_f16(const struct pipe_resource *p);

struct sgx_transfer {
   struct pipe_transfer base;
};

/* Whether res is sampled as it is, linear (M16): a format the sampler has
 * (native_format(), M37), one level, written by the GPU or by others (a
 * render's target, a shared buffer) -- the CPU's copy would read it back
 * each time -- or sampled with ss's nearest texel both ways.
 * Minification is point sampling then. */
bool sgx_resource_linear(const struct sgx_resource *res, const struct pipe_sampler_state *ss);

/* The four state words the sampler reads for res sampled through view
 * with ss (M14): the twiddled copy brought up to date first.  False when
 * res cannot be sampled yet (not 2D, no copy).  A copy the gathered draws
 * or a running render read is not written over: a new one is made (M37),
 * the old one kept for them (the batch holds the buffer res->sampled). */
bool sgx_resource_texture(struct sgx_context *ctx, struct sgx_resource *res,
                          const struct pipe_sampler_view *view,
                          const struct pipe_sampler_state *ss, uint32_t words[4]);

/* A buffer's copy for the GPU (M26), brought up to date with the CPU's
 * first: written in place when no render reads it -- gathered or running
 * -- or when only unsynchronized maps wrote it since, else made anew (the
 * old one stays with the renders that read it).  NULL if it cannot be. */
struct sgx_bo *sgx_buffer_bo(struct sgx_context *ctx, struct sgx_resource *res, uint32_t read_lo,
                             uint32_t read_hi);

void sgx_resource_screen_init(struct sgx_screen *screen);
void sgx_resource_context_init(struct pipe_context *pctx);

#endif
