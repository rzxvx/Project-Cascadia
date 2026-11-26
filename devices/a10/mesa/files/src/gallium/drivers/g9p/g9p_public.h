/* SPDX-License-Identifier: MIT */
#ifndef G9P_PUBLIC_H
#define G9P_PUBLIC_H

struct pipe_screen;
struct sw_winsys;

/* the GPU's screen showing through a software winsys (its display targets
 * filled from the GPU's renders at flush_frontbuffer) */
struct pipe_screen *g9p_create_sw_screen(struct sw_winsys *ws);

#endif
