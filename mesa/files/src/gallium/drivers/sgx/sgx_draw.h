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

bool sgx_draw_init(struct sgx_context *ctx);
void sgx_draw_fini(struct sgx_context *ctx);

void sgx_draw_vbo(struct pipe_context *pctx, const struct pipe_draw_info *info,
                  unsigned drawid_offset, const struct pipe_draw_indirect_info *indirect,
                  const struct pipe_draw_start_count_bias *draws, unsigned num_draws);

#endif
