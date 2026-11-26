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
};

static inline struct g9p_resource *
g9p_resource(struct pipe_resource *p)
{
   return (struct g9p_resource *)p;
}

void g9p_resource_screen_init(struct pipe_screen *pscreen);
void g9p_resource_context_init(struct pipe_context *pctx);

#endif
