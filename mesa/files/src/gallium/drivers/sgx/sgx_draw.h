/*
 * Copyright 2026 Project Cascadia
 * SPDX-License-Identifier: MIT
 *
 * Draws, for now (docs/research/p105-mesa.md, M13a): vertex shaders on the
 * CPU in Gallium's draw module, triangles on the GPU through the template
 * frame (sgx_frame.h), the fragment colour worked out per vertex.
 */
#ifndef SGX_DRAW_H
#define SGX_DRAW_H

#include <stdbool.h>

#include "pipe/p_state.h"

struct nir_shader;
struct sgx_context;
struct sgx_fs;
struct sgx_vs;

/* Where each channel of a fragment shader's colour comes from: the
 * template frame's programs interpolate a colour between the vertices, so
 * a shader can be drawn when every channel is a constant, a component of a
 * varying or a component of a uniform. */
enum sgx_colour_src {
   SGX_SRC_CONST,
   SGX_SRC_VARYING,     /* slot: gl_varying_slot */
   SGX_SRC_UNIFORM,     /* slot: the float's index in constant buffer 0 */
};

struct sgx_fs_colour {
   bool ok;
   struct {
      enum sgx_colour_src src;
      unsigned slot, comp;
      float value;
   } ch[4];
};

/* reads fs (it is not changed) */
void sgx_fs_colour_analyse(const struct nir_shader *fs, struct sgx_fs_colour *out);

/* the gathered draws rendered now (nothing if there are none) */
void sgx_batch_flush_at(struct sgx_context *ctx, const char *file, int line);
/* (where from: SGX_DEBUG=fps counts the renders by it) */
#define sgx_batch_flush(ctx) sgx_batch_flush_at(ctx, __FILE__, __LINE__)
void sgx_batch_why(struct sgx_context *ctx);
/* a program gone from its shader: retired and freed at the next flush
 * (or now, when nothing is gathered) */
void sgx_batch_bury_fs(struct sgx_context *ctx, struct sgx_fs *fs);
void sgx_batch_bury_vs(struct sgx_context *ctx, struct sgx_vs *vs);
/* depth (to d) and/or stencil (to value, through mask) cleared within the
 * render, over sc or all of the target: a quad that writes nothing but
 * them (M23, M24) */
struct pipe_scissor_state;
bool sgx_colour_clear(struct sgx_context *ctx, const union pipe_color_union *color,
                      const struct pipe_scissor_state *sc);
void sgx_zs_clear(struct sgx_context *ctx, bool depth, float d, bool stencil, unsigned value,
                  unsigned mask, const struct pipe_scissor_state *sc);
/* whether the gathered draws write or read p */
bool sgx_batch_uses(struct sgx_context *ctx, struct pipe_resource *p);

bool sgx_draw_init(struct sgx_context *ctx);
void sgx_draw_fini(struct sgx_context *ctx);

void sgx_draw_vbo(struct pipe_context *pctx, const struct pipe_draw_info *info,
                  unsigned drawid_offset, const struct pipe_draw_indirect_info *indirect,
                  const struct pipe_draw_start_count_bias *draws, unsigned num_draws);

#endif
