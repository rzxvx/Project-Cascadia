/*
 * Copyright 2026 Project Cascadia
 * SPDX-License-Identifier: MIT
 */
#ifndef SGX_SCREEN_H
#define SGX_SCREEN_H

#include "pipe/p_screen.h"
#include "util/simple_mtx.h"
#include "util/slab.h"

#include "sgx_device.h"

struct renderonly;
struct sgx_frame;

struct sgx_screen {
   struct pipe_screen base;
   struct sgx_device dev;

   /* sgx2d's template frame, for clears; NULL without one */
   struct sgx_frame *frame;
   simple_mtx_t frame_lock;

   struct slab_parent_pool transfer_pool;
};

static inline struct sgx_screen *
sgx_screen(struct pipe_screen *p)
{
   return (struct sgx_screen *)p;
}

struct pipe_screen *sgx_screen_create(int fd, const struct pipe_screen_config *config,
                                      struct renderonly *ro);

#endif
