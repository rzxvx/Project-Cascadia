/*
 * Copyright 2026 Project Cascadia
 * SPDX-License-Identifier: MIT
 */
#ifndef SGX_DRM_PUBLIC_H
#define SGX_DRM_PUBLIC_H

struct pipe_screen;
struct pipe_screen_config;

/* The render node of drivers/gpu/drm/apple-sgx ("apple_sgx") */
struct pipe_screen *sgx_drm_screen_create(int fd, const struct pipe_screen_config *config);

#endif
