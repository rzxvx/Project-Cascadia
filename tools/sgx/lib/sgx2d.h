/* sgx2d -- textured quads on the iPad mini's SGX543, full screen, straight
 * into the framebuffer.  Frames are built from the templates in a pack made
 * by tools/sgx/rpack.py; memory goes through apple-sgx's debugfs files.
 *
 *   sgx2d_open(packdir)                    map, boot, load the pack
 *   id = sgx2d_texture(rgba, w, h)         RGBA8, any size up to 1024
 *   sgx2d_texture_update(id, rgba)         new contents, same size
 *   sgx2d_texture_free(id)                 between frames
 *   sgx2d_begin(bg)                        a frame; texture bg fills the screen
 *   sgx2d_color(r, g, b, a)                multiplies the texels (default 1)
 *   sgx2d_blend(mode)                      SGX2D_BLEND (default), _ADD, _MOD
 *   sgx2d_quad(id, x, y, w, h)             pixels, top left origin
 *   sgx2d_quad_uv(id, x, y, w, h, u0, v0, u1, v1)   a part of the texture
 *   sgx2d_quad4(id, xy[8], uv[8])          any four corners (rotation, flips)
 *   sgx2d_fill(x, y, w, h)                 a rectangle of the current colour
 *   sgx2d_fill4(xy[8])                     any four corners of it
 *   sgx2d_end()                            build, render; 0 or -errno
 *   sgx2d_finish()                         wait until the frame is on screen
 *
 * Corners go top left, top right, bottom right, bottom left.  Texels are
 * RGBA8 in memory order (R first).  Blend modes: BLEND src*a + dst*(1-a),
 * ADD src*a + dst, MOD src*dst.  Quads are drawn in call order; consecutive
 * quads with the same texture and blend mode share a draw (about 390 draws
 * fit in a frame; the rest is dropped).  sgx2d_end() first waits for the
 * previous frame's 3D pass, so the CPU builds frame N+1 while the GPU draws
 * frame N.
 */
#ifndef SGX2D_H
#define SGX2D_H

#include <stdint.h>

enum { SGX2D_BLEND, SGX2D_ADD, SGX2D_MOD, SGX2D_NMODES };

int sgx2d_open(const char *packdir);
void sgx2d_close(void);
int sgx2d_texture(const void *rgba, int w, int h);
int sgx2d_texture_update(int id, const void *rgba);
void sgx2d_texture_free(int id);
void sgx2d_begin(int bg);
void sgx2d_color(float r, float g, float b, float a);
void sgx2d_blend(int mode);
void sgx2d_quad(int tex, float x, float y, float w, float h);
void sgx2d_quad_uv(int tex, float x, float y, float w, float h,
		   float u0, float v0, float u1, float v1);
void sgx2d_quad4(int tex, const float xy[8], const float uv[8]);
void sgx2d_fill(float x, float y, float w, float h);
void sgx2d_fill4(const float xy[8]);
int sgx2d_end(void);
int sgx2d_finish(void);
void sgx2d_stats(int *ntex, uint32_t *heap_free, uint32_t *ext_used);
void sgx2d_frame_stats(int *quads, int *draws, int *dropped);	/* after sgx2d_end */
uint32_t sgx2d_texture_addr(int id);
int sgx2d_width(void);
int sgx2d_height(void);

#endif
