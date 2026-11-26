/* SPDX-License-Identifier: MIT */
#ifndef G9P_RESOURCE_H
#define G9P_RESOURCE_H

#include "pipe/p_state.h"

#include "g9p_device.h"

#define G9P_MAX_LEVELS 16

/* every resource one buffer object, linear: levels one after the other,
 * rows 64-byte aligned (the end-of-tile image state's stride unit) */
struct g9p_resource {
   struct pipe_resource base;
   struct g9p_bo *bo;
   unsigned stride[G9P_MAX_LEVELS];
   unsigned offset[G9P_MAX_LEVELS];
   /* the sampler reads textures twiddled (Morton order, the y bit first):
    * a copy of the levels, made again when seq (the CPU's writes, renders
    * into it) has moved past tw_seq */
   struct g9p_bo *tw;
   unsigned seq, tw_seq;
   /* with a software winsys: its display target, the render copied there */
   struct sw_displaytarget *dt;
   unsigned dt_stride;
};

static inline struct g9p_resource *
g9p_resource(struct pipe_resource *p)
{
   return (struct g9p_resource *)p;
}

void g9p_resource_screen_init(struct pipe_screen *pscreen);

/* the twiddled copy of a texture's levels, up to date; NULL if none */
struct g9p_bo *g9p_resource_twiddled(struct g9p_device *dev, struct g9p_resource *res, unsigned *nlevels);
void g9p_resource_context_init(struct pipe_context *pctx);

#endif
