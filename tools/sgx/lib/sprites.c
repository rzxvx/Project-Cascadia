/* sprites: N bouncing sprites over a gradient, through sgx2d; prints fps.
 *   sprites PACKDIR [N] [FRAMES] */
#include "sgx2d.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define RGBA(r, g, b, a) ((uint32_t)(r) | (uint32_t)(g) << 8 | (uint32_t)(b) << 16 | (uint32_t)(a) << 24)

static uint32_t tex_bg[64 * 64], tex_ball[32 * 32], tex_ring[32 * 32], tex_box[32 * 32];

static void make_textures(void)
{
	int x, y;

	for (y = 0; y < 64; y++)
		for (x = 0; x < 64; x++)
			tex_bg[y * 64 + x] = RGBA(10, 20 + y, 60 + y * 2, 255);
	for (y = 0; y < 32; y++)
		for (x = 0; x < 32; x++) {
			float dx = x - 15.5f, dy = y - 15.5f, r = sqrtf(dx * dx + dy * dy);
			float hx = x - 11, hy = y - 10;

			tex_ball[y * 32 + x] = r < 14 ? (hx * hx + hy * hy < 12 ? RGBA(255, 255, 255, 255)
						       : RGBA(255, 80, 40, 255)) : 0;
			tex_ring[y * 32 + x] = r < 15 && r > 10 ? RGBA(80, 255, 120, 200) : 0;
			tex_box[y * 32 + x] = (x / 8 + y / 8) & 1 ? RGBA(250, 220, 0, 255)
							       : RGBA(40, 40, 200, 160);
		}
}

int main(int argc, char **argv)
{
	int n = argc > 2 ? atoi(argv[2]) : 200, frames = argc > 3 ? atoi(argv[3]) : 600;
	int bg, tex[3], i, f, ret;
	struct { float x, y, s, dx, dy; int t; } *sp;
	struct timespec t0, t1;
	float W, H;

	if ((ret = sgx2d_open(argc > 1 ? argv[1] : "."))) {
		fprintf(stderr, "sgx2d_open: %d\n", ret);
		return 1;
	}
	W = sgx2d_width();
	H = sgx2d_height();
	make_textures();
	bg = sgx2d_texture(tex_bg, 64, 64);
	tex[0] = sgx2d_texture(tex_ball, 32, 32);
	tex[1] = sgx2d_texture(tex_ring, 32, 32);
	tex[2] = sgx2d_texture(tex_box, 32, 32);
	if (bg < 0 || tex[0] < 0 || tex[1] < 0 || tex[2] < 0) {
		fprintf(stderr, "sgx2d_texture failed\n");
		return 1;
	}
	sp = calloc(n, sizeof(*sp));
	srand(3);
	for (i = 0; i < n; i++) {
		sp[i].s = 24 + rand() % 104;
		sp[i].x = rand() % (int)(W - sp[i].s);
		sp[i].y = rand() % (int)(H - sp[i].s);
		sp[i].dx = (rand() % 1000) / 100.0f - 5;
		sp[i].dy = (rand() % 1000) / 100.0f - 5;
		sp[i].t = i % 3;	/* interleaved: worst case, one draw per quad */
	}
	clock_gettime(CLOCK_MONOTONIC, &t0);
	for (f = 0; f < frames; f++) {
		sgx2d_begin(bg);
		for (i = 0; i < n; i++) {
			sp[i].x += sp[i].dx;
			sp[i].y += sp[i].dy;
			if (sp[i].x < 0 || sp[i].x > W - sp[i].s) sp[i].dx = -sp[i].dx;
			if (sp[i].y < 0 || sp[i].y > H - sp[i].s) sp[i].dy = -sp[i].dy;
			sgx2d_quad(tex[sp[i].t], sp[i].x, sp[i].y, sp[i].s, sp[i].s);
		}
		if ((ret = sgx2d_end())) {
			fprintf(stderr, "frame %d: %d\n", f, ret);
			return 1;
		}
	}
	clock_gettime(CLOCK_MONOTONIC, &t1);
	printf("%d frames, %d sprites: %.1f fps\n", frames, n,
	       frames / ((t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9));
	sgx2d_close();
	return 0;
}
