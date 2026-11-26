/* SPDX-License-Identifier: MIT */
#ifndef G9P_DRM_PUBLIC_H
#define G9P_DRM_PUBLIC_H

struct pipe_screen;
struct pipe_screen_config;

/* the render node of the iPad 7's GPU (drivers/gpu/drm/imagination with
 * Cascadia's Apple backend, "powervr") */
struct pipe_screen *g9p_drm_screen_create(int fd, const struct pipe_screen_config *config);

#endif
