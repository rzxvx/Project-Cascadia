/* sgx2d -- textured quads on the iPad mini's SGX543, full screen, straight
 * into the framebuffer.  Frames are built from the templates in a pack made
 * by tools/sgx/rpack.py; memory goes through apple-sgx's debugfs files.
 *
 *   sgx2d_open(packdir)                   map, boot, load the pack
 *   id = sgx2d_texture(rgba, w, h)        RGBA8, any size up to 1024
 *   sgx2d_begin(bg)                       a frame; texture bg fills the screen
 *   sgx2d_quad(id, x, y, w, h)            pixels, top left origin
 *   sgx2d_quad_uv(id, ..., u0, v0, u1, v1) a part of the texture
 *   sgx2d_fill(rgba, x, y, w, h)          a rectangle of one colour
 *   sgx2d_texture_update(id, rgba)        new contents, same size
 *   sgx2d_end()                           build, render; 0 or -errno
 *   sgx2d_finish()                        wait until the frame is on screen
 *
 * sgx2d_end() first waits for the previous frame's 3D pass, so the CPU
 * builds frame N+1 while the GPU draws frame N.
 *
 * Colours and texels are RGBA8 in memory order (R first).  Quads are drawn
 * in call order with alpha blending (SRC_ALPHA, ONE_MINUS_SRC_ALPHA);
 * consecutive quads with the same texture share a draw (about 390 draws fit
 * in a frame; the rest is dropped).
 */
#ifndef SGX2D_H
#define SGX2D_H

int sgx2d_open(const char *packdir);
void sgx2d_close(void);
#include <stdint.h>
int sgx2d_texture(const void *rgba, int w, int h);
int sgx2d_texture_update(int id, const void *rgba);
void sgx2d_fill(uint32_t rgba, float x, float y, float w, float h);
void sgx2d_begin(int bg);
void sgx2d_quad(int tex, float x, float y, float w, float h);
void sgx2d_quad_uv(int tex, float x, float y, float w, float h,
		   float u0, float v0, float u1, float v1);
int sgx2d_end(void);
int sgx2d_finish(void);
int sgx2d_width(void);
int sgx2d_height(void);

#endif
