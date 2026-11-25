/* SPDX-License-Identifier: MIT */
/*
 * g9p2d -- textured quads on the iPad 7's GPU (Apple A10, G9P), through the
 * powervr UAPI (/dev/dri/renderD128, Cascadia's kernel driver), straight into
 * the display. A compute compositor: the target is cut into 64 x 32 tiles,
 * each frame's quads are sorted into the tiles they touch on the CPU, and one
 * workgroup per tile applies them in order (docs/gpu.md "2D on the GPU").
 *
 *   g9p2d_open()                         device, VM, kernel, the display
 *   id = g9p2d_texture(rgba, w, h)       RGBA8 (R first in memory), any size
 *   g9p2d_texture_update(id, rgba)       new contents, same size
 *   g9p2d_texture_free(id)               freed once the GPU is done with it
 *   g9p2d_begin()                        a frame (drawing into the display)
 *   g9p2d_target(id)                     from here on draw into texture id
 *                                        (0: the display again)
 *   g9p2d_color(r, g, b, a)              multiplies the texels (default 1)
 *   g9p2d_blend(mode)                    G9P2D_BLEND (default), _NONE, _ADD, _MOD
 *   g9p2d_quad(id, x, y, w, h)           pixels, top left origin
 *   g9p2d_quad_uv(id, x, y, w, h, u0, v0, u1, v1)   a part of the texture
 *   g9p2d_quad4(id, xy[8], uv[8])        any parallelogram (rotation, flips)
 *   g9p2d_fill(x, y, w, h), g9p2d_fill4(xy[8])      the colour alone
 *   g9p2d_end()                          submit; 0 or -errno
 *   g9p2d_finish()                       wait until the GPU is done
 *
 * Corners go top left, top right, bottom right, bottom left; uv in 0..1.
 * Blend modes as SDL's: NONE src, BLEND src*a + dst*(1-a), ADD src*a + dst,
 * MOD src*dst. Quads are drawn in call order, nearest-texel sampling.
 * g9p2d_end() returns at once: the CPU builds frame N+1 while the GPU draws
 * frame N (two frames in flight).
 */
#ifndef G9P2D_H
#define G9P2D_H

#include <stdint.h>

enum { G9P2D_NONE, G9P2D_BLEND, G9P2D_ADD, G9P2D_MOD };

int g9p2d_open(void);
void g9p2d_close(void);
int g9p2d_width(void);
int g9p2d_height(void);
int g9p2d_texture(const void *rgba, int w, int h);
int g9p2d_texture_update(int id, const void *rgba);
void g9p2d_texture_free(int id);
uint32_t *g9p2d_texture_pixels(int id);	/* CPU view (read back a target after g9p2d_finish) */
void g9p2d_begin(void);
void g9p2d_target(int id);
void g9p2d_color(float r, float g, float b, float a);
void g9p2d_blend(int mode);
void g9p2d_quad(int tex, float x, float y, float w, float h);
void g9p2d_quad_uv(int tex, float x, float y, float w, float h,
		   float u0, float v0, float u1, float v1);
void g9p2d_quad4(int tex, const float xy[8], const float uv[8]);
void g9p2d_fill(float x, float y, float w, float h);
void g9p2d_fill4(const float xy[8]);
int g9p2d_end(void);
int g9p2d_finish(void);
/* the last submitted frame: quads, tiles (workgroups), tile list entries,
 * dropped quads; GPU time of the last finished frame in microseconds */
void g9p2d_frame_stats(int *quads, int *tiles, int *entries, int *dropped, double *gpu_us);

#endif
