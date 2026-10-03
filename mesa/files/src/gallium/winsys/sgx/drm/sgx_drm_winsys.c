/*
 * Copyright 2026 Project Cascadia
 * SPDX-License-Identifier: MIT
 */
#include "util/os_file.h"
#include "util/u_screen.h"

#include "sgx_drm_public.h"
#include "sgx/sgx_screen.h"

struct pipe_screen *
sgx_drm_screen_create(int fd, const struct pipe_screen_config *config)
{
   return u_pipe_screen_lookup_or_create(os_dupfd_cloexec(fd), config, NULL,
                                         sgx_screen_create);
}
