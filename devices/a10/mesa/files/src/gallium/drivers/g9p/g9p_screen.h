/* SPDX-License-Identifier: MIT */
#ifndef G9P_SCREEN_H
#define G9P_SCREEN_H

#include "pipe/p_screen.h"
#include "util/slab.h"

#include "g9p_device.h"

struct renderonly;
struct pipe_screen_config;

struct sw_winsys;

struct g9p_screen {
   struct pipe_screen base;
   struct g9p_device dev;
   struct sw_winsys *sw;       /* a software winsys shows the display targets (g9p_public.h) */
   struct slab_parent_pool transfer_pool;
};

static inline struct g9p_screen *
g9p_screen(struct pipe_screen *p)
{
   return (struct g9p_screen *)p;
}

struct pipe_screen *g9p_screen_create(int fd, const struct pipe_screen_config *config,
                                      struct renderonly *ro);

/* the render-target formats the end-of-tile program writes */
bool g9p_rt_format(enum pipe_format format);

#endif
