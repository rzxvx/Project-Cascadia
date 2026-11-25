// SPDX-License-Identifier: MIT
/*
 * g9p2d-test: g9p2d against a CPU reference, then on the display.
 *
 *   g9p2d-test            offscreen: quads of every kind into a texture
 *                         target, every pixel compared with the same maths
 *                         on the CPU; then 300 frames of sprites on the
 *                         display (frames per second, GPU time per frame)
 *   g9p2d-test check      only the offscreen comparison
 *   g9p2d-test screen N   only N frames on the display
 *   g9p2d-test shot       the display's memory to stdout (no GPU)
 *   g9p2d-test stress N L [S]  N frames into an SxS target (1024), each a clear and
 *                         L translucent full-target fills (slow tiles): every
 *                         pixel the same value, a tile left out shows
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/mman.h>

#include "g9p2d.h"

/* the CPU reference: the same quads, the kernel's maths */
struct ref {
	uint32_t *px;
	int w, h;
};

static float unorm(uint32_t v, int sh)
{
	return ((v >> sh) & 255) / 255.f;
}

static void ref_quad(struct ref *r, const uint32_t *tex, int tw, int th, const float xy[8], const float uv[8],
		     const float col[4], int mode)
{
	float e1x = xy[2] - xy[0], e1y = xy[3] - xy[1], e2x = xy[6] - xy[0], e2y = xy[7] - xy[1];
	float det = e1x * e2y - e1y * e2x;
	int x, y;

	for (y = 0; y < r->h; y++)
		for (x = 0; x < r->w; x++) {
			float fx = x + 0.5f - xy[0], fy = y + 0.5f - xy[1];
			float s = (fx * e2y - fy * e2x) / det, t = (fy * e1x - fx * e1y) / det;
			float src[4], d[3], o[3];
			uint32_t *p = &r->px[y * r->w + x];
			int k;

			if (!(s >= 0 && s < 1 && t >= 0 && t < 1))
				continue;
			memcpy(src, col, sizeof(src));
			if (tex) {
				float u = uv[0] * tw + s * (uv[2] - uv[0]) * tw, v = uv[1] * th + t * (uv[7] - uv[1]) * th;
				int iu = u < 0 ? 0 : u > tw - 1 ? tw - 1 : (int)u;
				int iv = v < 0 ? 0 : v > th - 1 ? th - 1 : (int)v;
				uint32_t c = tex[iv * tw + iu];

				src[0] *= unorm(c, 0);
				src[1] *= unorm(c, 8);
				src[2] *= unorm(c, 16);
				src[3] *= unorm(c, 24);
			}
			d[0] = unorm(*p, 0);
			d[1] = unorm(*p, 8);
			d[2] = unorm(*p, 16);
			for (k = 0; k < 3; k++) {
				o[k] = mode == G9P2D_NONE ? src[k] :
				       mode == G9P2D_BLEND ? src[k] * src[3] + d[k] * (1 - src[3]) :
				       mode == G9P2D_ADD ? fminf(src[k] * src[3] + d[k], 1) : src[k] * d[k];
				o[k] = o[k] < 0 ? 0 : o[k] > 1 ? 1 : o[k];
			}
			*p = 0xff000000u | (uint32_t)(o[2] * 255 + 0.5f) << 16 |
			     (uint32_t)(o[1] * 255 + 0.5f) << 8 | (uint32_t)(o[0] * 255 + 0.5f);
		}
}

static int check(void)
{
	enum { W = 300, H = 200, TW = 7, TH = 5 };
	static uint32_t tex[TW * TH];
	struct ref r = { calloc(W * H, 4), W, H };
	int target, t, x, y, i, bad = 0, maxd = 0;
	uint32_t *gpu;
	static int tbad[8][8];
	struct {
		int tex, mode;
		float col[4], xy[8], uv[8];
	} q[] = {
		{ 0, G9P2D_NONE, { 0.2f, 0.4f, 0.6f, 1 }, { 0, 0, 300, 0, 300, 200, 0, 200 }, { 0, 0, 1, 0, 1, 1, 0, 1 } },
		{ 1, G9P2D_BLEND, { 1, 1, 1, 1 }, { 10, 10, 80, 10, 80, 60, 10, 60 }, { 0, 0, 1, 0, 1, 1, 0, 1 } },
		{ 1, G9P2D_BLEND, { 1, 0.5f, 1, 0.7f }, { 100.3f, 20.7f, 190, 20.7f, 190, 90, 100.3f, 90 },
		  { 1, 0, 0, 0, 0, 1, 1, 1 } },		/* flipped in x */
		{ 0, G9P2D_BLEND, { 0, 1, 0, 0.5f }, { 50, 40, 150, 40, 150, 120, 50, 120 }, { 0 } },
		{ 0, G9P2D_ADD, { 0.5f, 0.1f, 0, 0.8f }, { 120, 100, 260, 100, 260, 180, 120, 180 }, { 0 } },
		{ 1, G9P2D_MOD, { 1, 1, 1, 1 }, { 200, 10, 290, 10, 290, 95, 200, 95 }, { 0, 0, 1, 0, 1, 1, 0, 1 } },
		/* turned by 30 degrees about (150, 150) */
		{ 1, G9P2D_BLEND, { 1, 1, 1, 1 }, { 0 }, { 0.2f, 0.2f, 0.8f, 0.2f, 0.8f, 0.8f, 0.2f, 0.8f } },
		{ 0, G9P2D_NONE, { 1, 1, 0, 1 }, { 290, 190, 310, 190, 310, 210, 290, 210 }, { 0 } },	/* past the edge */
	};
	int nq = sizeof(q) / sizeof(q[0]);

	{
		float c = cosf(M_PI / 6), s = sinf(M_PI / 6), px[4] = { -40, 40, 40, -40 }, py[4] = { -25, -25, 25, 25 };

		for (i = 0; i < 4; i++) {
			q[6].xy[2 * i] = 150 + px[i] * c - py[i] * s;
			q[6].xy[2 * i + 1] = 150 + px[i] * s + py[i] * c;
		}
	}
	for (y = 0; y < TH; y++)
		for (x = 0; x < TW; x++)
			tex[y * TW + x] = (uint32_t)(x * 36) | (uint32_t)(y * 60) << 8 |
					  (uint32_t)((x + y) & 1 ? 255 : 40) << 16 | (uint32_t)(x == 3 ? 64 : 255) << 24;
	t = g9p2d_texture(tex, TW, TH);
	target = g9p2d_texture(NULL, W, H);
	if (t < 0 || target < 0) {
		printf("FAIL  textures: %d %d\n", t, target);
		return 1;
	}
	g9p2d_target(target);
	for (i = 0; i < nq; i++) {
		g9p2d_color(q[i].col[0], q[i].col[1], q[i].col[2], q[i].col[3]);
		g9p2d_blend(q[i].mode);
		g9p2d_quad4(q[i].tex ? t : 0, q[i].xy, q[i].uv);
		ref_quad(&r, q[i].tex ? tex : NULL, TW, TH, q[i].xy, q[i].uv, q[i].col, q[i].mode);
	}
	g9p2d_target(0);
	i = g9p2d_end();
	if (i || (i = g9p2d_finish())) {
		printf("FAIL  submit / wait: %d\n", i);
		return 1;
	}
	gpu = g9p2d_texture_pixels(target);
	if (getenv("RECHECK")) {
		uint32_t *snap = malloc(W * H * 4);
		int changed = 0;

		memcpy(snap, gpu, W * H * 4);
		usleep(50000);
		for (i = 0; i < W * H; i++)
			changed += snap[i] != gpu[i];
		printf("  50 ms after finish: %d pixels changed\n", changed);
		free(snap);
	}
	for (i = 0; i < W * H; i++) {
		int k, d = 0;

		for (k = 0; k < 32; k += 8) {
			int dk = abs((int)((gpu[i] >> k) & 255) - (int)((r.px[i] >> k) & 255));

			d = dk > d ? dk : d;
		}
		if (d > maxd)
			maxd = d;
		if (d > 2 && bad++ < 4)
			printf("  (%d, %d): gpu %08x cpu %08x\n", i % W, i / W, gpu[i], r.px[i]);
		if (d > 2)
			tbad[(i / W) / 32][(i % W) / 64]++;
	}
	for (y = 0; y < (H + 31) / 32; y++)
		for (x = 0; x < (W + 63) / 64; x++)
			if (tbad[y][x])
				printf("  tile (%d, %d) = record %d: %d pixels wrong\n", x, y, y * ((W + 63) / 64) + x,
				       tbad[y][x]);
	{
		int q_, tiles, ent, drop;
		double us;

		g9p2d_frame_stats(&q_, &tiles, &ent, &drop, &us);
		printf("%s  offscreen %dx%d: %d quads, %d tiles, %d list entries; %d pixels off by > 2 (max %d); "
		       "%.0f us\n", bad ? "FAIL" : "ok  ", W, H, q_, tiles, ent, bad, maxd, us);
	}
	{
		/* two passes in one frame: a 16x16 target drawn, then copied with
		 * MOD onto the first target (as SuperTux's lightmap) */
		int lm = g9p2d_texture(NULL, 16, 16), bad2 = 0;
		static const float uv[8] = { 0, 0, 1, 0, 1, 1, 0, 1 };
		float all[8] = { 0, 0, W, 0, W, H, 0, H };
		uint32_t before[4];

		before[0] = gpu[0];
		before[1] = gpu[W - 1];
		before[2] = gpu[(H - 1) * W];
		before[3] = gpu[(H - 1) * W + W - 1];
		g9p2d_target(lm);
		g9p2d_blend(G9P2D_NONE);
		g9p2d_color(0.5f, 0.5f, 0.5f, 1);
		g9p2d_fill(0, 0, 16, 16);
		g9p2d_color(1, 1, 1, 1);
		g9p2d_fill(8, 8, 8, 8);			/* the bottom right quarter white */
		g9p2d_target(target);
		g9p2d_blend(G9P2D_MOD);
		g9p2d_quad4(lm, all, uv);
		g9p2d_target(0);
		if (g9p2d_end() || g9p2d_finish())
			bad2 = 1;
		/* top left halved, bottom right kept */
		for (i = 0; i < 3; i++) {
			uint32_t want = 0xff000000u;
			int k;

			for (k = 0; k < 24; k += 8)
				want |= (uint32_t)((((before[i] >> k) & 255) * 128 + 127) / 255) << k;
			if (abs((int)(gpu[i == 0 ? 0 : i == 1 ? W - 1 : (H - 1) * W] & 255) - (int)(want & 255)) > 2) {
				printf("  pass 2 corner %d: %08x, want %08x\n", i, gpu[i == 0 ? 0 : i == 1 ? W - 1 :
				       (H - 1) * W], want);
				bad2++;
			}
		}
		if (gpu[(H - 1) * W + W - 1] != before[3]) {
			printf("  pass 2 bottom right: %08x, want %08x\n", gpu[(H - 1) * W + W - 1], before[3]);
			bad2++;
		}
		printf("%s  render target, then sampled in the same frame (MOD)\n", bad2 ? "FAIL" : "ok  ");
		bad += bad2;
		g9p2d_texture_free(lm);
	}
	g9p2d_texture_free(t);
	g9p2d_texture_free(target);
	free(r.px);
	return bad != 0;
}

static uint32_t dt_be32(const char *name, int idx)
{
	char path[300];
	unsigned char b[16] = { 0 };
	FILE *f;
	DIR *d = opendir("/proc/device-tree/chosen");
	struct dirent *e;

	path[0] = 0;
	while (d && (e = readdir(d)))
		if (!strncmp(e->d_name, "framebuffer", 11))
			snprintf(path, sizeof(path), "/proc/device-tree/chosen/%s/%s", e->d_name, name);
	if (d)
		closedir(d);
	if (!path[0] || !(f = fopen(path, "rb")))
		return 0;
	if (fread(b, 1, sizeof(b), f) < (size_t)(4 * idx + 4))
		memset(b, 0, sizeof(b));
	fclose(f);
	return (uint32_t)b[4 * idx] << 24 | b[4 * idx + 1] << 16 | b[4 * idx + 2] << 8 | b[4 * idx + 3];
}

static uint64_t fb_pa(void)
{
	return (uint64_t)dt_be32("reg", 0) << 32 | dt_be32("reg", 1);
}

static int fb_stride(void)
{
	return dt_be32("stride", 0);
}

static int screen(int frames)
{
	enum { S = 64, N = 400 };
	static uint32_t spr[S * S], bg[256 * 256];
	int ts, tb, f, i, W = g9p2d_width(), H = g9p2d_height();
	struct timespec t0, t1;
	double gpu_sum = 0;

	for (i = 0; i < S * S; i++) {
		int x = i % S - S / 2, y = i / S - S / 2, d2 = x * x + y * y;

		spr[i] = d2 < (S / 2) * (S / 2) ? 0xff000000u | (uint32_t)(255 - d2 / 4) << 8 | 0xff : 0;
	}
	for (i = 0; i < 256 * 256; i++)
		bg[i] = 0xff000000u | (uint32_t)(i % 256) | (uint32_t)(i / 256) << 8 | 0x600000;
	ts = g9p2d_texture(spr, S, S);
	tb = g9p2d_texture(bg, 256, 256);
	clock_gettime(CLOCK_MONOTONIC, &t0);
	for (f = 0; f < frames; f++) {
		int q, tiles, ent, drop;
		double us;

		g9p2d_blend(G9P2D_NONE);
		g9p2d_quad(tb, 0, 0, W, H);
		g9p2d_blend(G9P2D_BLEND);
		for (i = 0; i < N; i++) {
			float a = f * 0.02f + i * 0.7f, x = W / 2 + cosf(a * (1 + i % 7 * 0.1f)) * (W / 2 - S),
			      y = H / 2 + sinf(a * 1.3f) * (H / 2 - S);

			g9p2d_color(1, (i % 5) / 4.f, (i % 3) / 2.f, 0.9f);
			g9p2d_quad(ts, x - S / 2, y - S / 2, S, S);
		}
		g9p2d_end();
		g9p2d_frame_stats(&q, &tiles, &ent, &drop, &us);
		gpu_sum += us;
		if (f % 100 == 99)
			printf("  frame %d: %d quads, %d tiles, %d entries, %d dropped\n", f + 1, q, tiles, ent, drop);
	}
	g9p2d_finish();
	clock_gettime(CLOCK_MONOTONIC, &t1);
	printf("ok    display %dx%d: %d frames of %d sprites + background, %.1f frames/s\n", W, H, frames, N,
	       frames / ((t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9));
	{
		/* one frame alone: its time from submit to done */
		double us;

		g9p2d_blend(G9P2D_NONE);
		g9p2d_quad(tb, 0, 0, W, H);
		g9p2d_end();
		g9p2d_finish();
		g9p2d_frame_stats(NULL, NULL, NULL, NULL, &us);
		printf("      one full-screen textured quad alone: %.0f us submit to done\n", us);
	}
	{
		/* that frame on the display itself: its memory through /dev/mem
		 * (the device tree's /chosen/framebuffer reg), against the texture */
		int fd = open("/dev/mem", O_RDONLY | O_SYNC), bad = 0, n = 0, x, y;
		uint64_t pa = fb_pa();
		size_t len = (size_t)fb_stride() * H;
		uint32_t *fb = fd < 0 || !pa ? MAP_FAILED : mmap(NULL, len, PROT_READ, MAP_SHARED, fd, pa);

		if (fb == MAP_FAILED) {
			printf("FAIL  /dev/mem at %#llx: %s\n", (unsigned long long)pa, strerror(errno));
			return 1;
		}
		for (y = 0; y < H; y += 7)
			for (x = 0; x < W; x += 5) {
				uint32_t t = bg[(int)((y + 0.5f) * 256 / H) * 256 + (int)((x + 0.5f) * 256 / W)];
				uint32_t want = 0xff000000u | (t & 0xff) << 16 | (t & 0xff00) | (t >> 16 & 0xff);
				uint32_t got = fb[y * (fb_stride() / 4) + x] | 0xff000000u;

				n++;
				if (got != want && bad++ < 3)
					printf("  display (%d, %d): %08x, texture says %08x\n", x, y, got, want);
			}
		printf("%s  on the display (/dev/mem, %#llx): %d of %d sampled pixels as the texture says\n",
		       bad ? "FAIL" : "ok  ", (unsigned long long)pa, n - bad, n);
		munmap(fb, len);
		close(fd);
		if (bad)
			return 1;
	}
	(void)gpu_sum;
	return 0;
}

/* the display's memory, raw (x8r8g8b8, stride as the device tree has it), to stdout */
static int stress(int frames, int layers, int S)
{
	int target = g9p2d_texture(NULL, S, S), f, l, i, bad_frames = 0, bad_tiles = 0;
	uint32_t *px = g9p2d_texture_pixels(target);
	double us_sum = 0;

	if (target < 0) {
		printf("FAIL  stress: target %d\n", target);
		return 1;
	}
	for (f = 0; f < frames; f++) {
		float c[3] = { (f % 7) / 6.f, (f % 5) / 4.f, (f % 3) / 2.f }, a = 0.05f;
		uint32_t want = 0xff000000u;
		static int tb[256][64];
		int nb = 0, k;
		double us;

		g9p2d_target(target);
		g9p2d_blend(G9P2D_NONE);
		g9p2d_color(c[0], c[1], c[2], 1);
		g9p2d_fill(0, 0, S, S);
		g9p2d_blend(G9P2D_BLEND);
		for (l = 0; l < layers; l++) {
			float lc[3] = { (l % 2) ? 1.f : 0.f, (l % 3) ? 0.5f : 1.f, (l % 4) ? 0.f : 1.f };

			g9p2d_color(lc[0], lc[1], lc[2], a);
			g9p2d_fill(0, 0, S, S);
			for (k = 0; k < 3; k++)		/* the kernel's float maths */
				c[k] = lc[k] * a + c[k] * (1 - a);
		}
		g9p2d_target(0);
		if (g9p2d_end() || g9p2d_finish()) {
			printf("FAIL  stress frame %d: submit / wait\n", f);
			return 1;
		}
		g9p2d_frame_stats(NULL, NULL, NULL, NULL, &us);
		us_sum += us;
		for (k = 0; k < 3; k++) {
			float v = c[k] < 0 ? 0 : c[k] > 1 ? 1 : c[k];

			want |= (uint32_t)(v * 255 + 0.5f) << (8 * k);
		}
		memset(tb, 0, sizeof(tb));
		for (i = 0; i < S * S; i++) {
			int d = 0;

			for (k = 0; k < 24; k += 8) {
				int dk = abs((int)((px[i] >> k) & 255) - (int)((want >> k) & 255));

				d = dk > d ? dk : d;
			}
			if (d > 2 && !tb[(i / S) / 32][(i % S) / 64]++)
				nb++;
		}
		if (nb) {
			bad_frames++;
			bad_tiles += nb;
			if (bad_frames <= 3)
				printf("  frame %d: %d tiles wrong (want %08x, pixel 0 %08x)\n", f, nb, want, px[0]);
		}
	}
	printf("%s  stress: %d frames x %d layers, %dx%d (%d tiles): %d frames with wrong tiles (%d tiles); %.0f us a frame\n",
	       bad_frames ? "FAIL" : "ok  ", frames, layers, S, S, ((S + 63) / 64) * ((S + 31) / 32), bad_frames, bad_tiles, us_sum / frames);
	g9p2d_texture_free(target);
	return bad_frames != 0;
}

static int shot(void)
{
	int fd = open("/dev/mem", O_RDONLY | O_SYNC);
	size_t len = (size_t)fb_stride() * dt_be32("height", 0);
	void *fb = fd < 0 ? MAP_FAILED : mmap(NULL, len, PROT_READ, MAP_SHARED, fd, fb_pa());

	if (fb == MAP_FAILED || fwrite(fb, 1, len, stdout) != len)
		return 1;
	return 0;
}

int main(int argc, char **argv)
{
	int r, fails = 0;

	if (argc > 1 && !strcmp(argv[1], "shot"))
		return shot();
	r = g9p2d_open();

	if (r) {
		printf("FAIL  g9p2d_open: %d (%s)\n", r, strerror(-r));
		return 1;
	}
	printf("g9p2d: display %dx%d\n", g9p2d_width(), g9p2d_height());
	if (argc < 2 || !strcmp(argv[1], "check"))
		fails += check();
	if (argc > 1 && !strcmp(argv[1], "stress"))
		fails += stress(argc > 2 ? atoi(argv[2]) : 100, argc > 3 ? atoi(argv[3]) : 60,
				argc > 4 ? atoi(argv[4]) : 1024);
	if (argc < 2 || !strcmp(argv[1], "screen"))
		fails += screen(argc > 2 ? atoi(argv[2]) : 300);
	g9p2d_close();
	return fails ? 1 : 0;
}
