/* demo2: fills, sub-rectangles of a sprite sheet and a streaming texture */
#include "sgx2d.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#define RGBA(r, g, b, a) ((uint32_t)(r) | (uint32_t)(g) << 8 | (uint32_t)(b) << 16 | (uint32_t)(a) << 24)

int main(int argc, char **argv)
{
	static uint32_t sheet[64 * 64], stream[64 * 64], bgpx[16];
	int bg, sh, st, f, i, x, y;

	if (sgx2d_open(argc > 1 ? argv[1] : ".")) return 1;
	for (i = 0; i < 16; i++) bgpx[i] = RGBA(20, 20, 30, 255);
	bg = sgx2d_texture(bgpx, 4, 4);
	for (y = 0; y < 64; y++)		/* a 2x2 sheet of 32x32 cells */
		for (x = 0; x < 64; x++) {
			int c = (y / 32) * 2 + x / 32, dx = x % 32 - 16, dy = y % 32 - 16;
			uint32_t col[4] = { RGBA(255, 60, 60, 255), RGBA(60, 255, 60, 255),
					    RGBA(60, 60, 255, 255), RGBA(255, 255, 60, 255) };
			sheet[y * 64 + x] = dx * dx + dy * dy < 196 ? col[c] : 0;
		}
	sh = sgx2d_texture(sheet, 64, 64);
	st = sgx2d_texture(stream, 64, 64);
	for (f = 0; f < 600; f++) {
		for (y = 0; y < 64; y++)		/* plasma, recomputed every frame */
			for (x = 0; x < 64; x++) {
				float v = sinf(x * 0.2f + f * 0.1f) + sinf(y * 0.15f - f * 0.07f);
				stream[y * 64 + x] = RGBA(128 + 120 * sinf(v), 128 + 120 * cosf(v * 1.3f),
							  200, 255);
			}
		sgx2d_texture_update(st, stream);
		sgx2d_begin(bg);
		sgx2d_quad(st, 184, 100, 400, 400);
		for (i = 0; i < 8; i++) {		/* translucent fills */
			sgx2d_color((40 + i * 25) / 255.f, (200 - i * 20) / 255.f, 90 / 255.f, 0.7f);
			sgx2d_fill(60 + i * 80, 560 + 40 * sinf(f * 0.05f + i), 70, 70);
		}
		sgx2d_blend(SGX2D_ADD);			/* glows */
		for (i = 0; i < 3; i++) {
			sgx2d_color(0.6f, 0.4f, 0.1f, 1);
			sgx2d_quad(sh, 100 + i * 220 + 60 * sinf(f * 0.03f), 330, 180, 180);
		}
		sgx2d_blend(SGX2D_MOD);			/* darken the bottom third */
		sgx2d_color(0.4f, 0.4f, 0.7f, 1);
		sgx2d_fill(0, 700, 768, 324);
		sgx2d_blend(SGX2D_BLEND);
		{					/* a turning, fading sprite */
			float a = f * 0.04f, c = cosf(a) * 90, s = sinf(a) * 90, cx = 384, cy = 960;
			float xy[8] = { cx - c + s, cy - s - c, cx + c + s, cy + s - c,
					cx + c - s, cy + s + c, cx - c - s, cy - s + c };
			float uv[8] = { 0, 0, 0.5f, 0, 0.5f, 0.5f, 0, 0.5f };

			sgx2d_color(1, 1, 1, 0.5f + 0.5f * sinf(f * 0.1f));
			sgx2d_quad4(sh, xy, uv);
		}
		sgx2d_color(1, 1, 1, 1);
		for (i = 0; i < 4; i++)
			sgx2d_quad_uv(sh, 100 + i * 150 + 30 * cosf(f * 0.04f), 760, 128, 128,
				      (i % 2) * 0.5f, (i / 2) * 0.5f, (i % 2) * 0.5f + 0.5f, (i / 2) * 0.5f + 0.5f);
		if (sgx2d_end()) { fprintf(stderr, "frame %d failed\n", f); return 1; }
	}
	sgx2d_finish();
	printf("demo2: 600 frames\n");
	return 0;
}
