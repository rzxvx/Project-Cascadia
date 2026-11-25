// SPDX-License-Identifier: MIT
/*
 * g9psdl -- SDL2's 2D renderer on the iPad 7's GPU (g9p2d), as an LD_PRELOAD
 * shim:
 *
 *   LD_PRELOAD=/usr/local/lib/g9p2d/libg9psdl.so SDL_VIDEODRIVER=offscreen \
 *       supertux2 --renderer sdl --geometry 1080x810
 *
 * The real SDL keeps the window, the events and its own bookkeeping (a
 * software renderer nobody presents); every drawing call is taken here and
 * drawn with g9p2d, straight into the display. A landscape window is turned
 * onto the portrait display and scaled to fill it (G9PSDL_ROTATE=0: no
 * turn; G9PSDL_SCALE: the scale, default: the largest that fits).
 *
 * Covered: textures from surfaces, colour / alpha mod, blend modes, render
 * targets (drawn on the GPU in order with the rest of the frame), viewport,
 * scale, clip rect, RenderCopy(Ex) with rotation and flips, fills, lines,
 * clear, present.
 *
 * Input: a touch gamepad drawn over the frame (G9PSDL_PAD=0 turns it off):
 * the touchscreen (G9PSDL_TOUCH, default /dev/input/event0, its axis ranges
 * from the device) is read on a thread and its buttons pushed to SDL as key
 * events -- arrows, space (jump), left control (action), escape, return.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <SDL2/SDL.h>
#include <dlfcn.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <fcntl.h>
#include <linux/input.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "g9p2d.h"

#define REAL(name) static __typeof__(name) *real_##name; \
	if (!real_##name) real_##name = dlsym(RTLD_NEXT, #name)

struct tex {
	SDL_Texture *t;
	int id, w, h, target;
	Uint8 r, g, b, a;
	SDL_BlendMode blend;
};

static struct {
	int on, rotate, fbw, fbh, white, ww, wh;
	float os;			/* window pixels -> display pixels */
	SDL_Renderer *r;
	Uint8 dr, dg, db, da;
	SDL_BlendMode dblend;
	SDL_Rect vp;
	int has_vp, has_clip;
	SDL_Rect clip;
	float sx, sy;
	struct tex *target;		/* NULL: the display */
	struct tex *tex;
	int ntex, cap;
	unsigned frames;
} G = { .sx = 1, .sy = 1, .da = 255 };

static unsigned slot_of(SDL_Texture *t)
{
	uintptr_t k = (uintptr_t)t;

	k ^= k >> 16;
	k *= 0x45d9f3b;
	k ^= k >> 16;
	return k & (G.cap - 1);
}

/* ---- the touch gamepad ------------------------------------------------------ */

/* buttons in a 1024 x 768 landscape window, scaled to the real one */
static const struct pad_button {
	float x, y, w, h;
	SDL_Keycode key;
	SDL_Scancode scan;
} pad[] = {
	{   20, 560, 140, 150, SDLK_LEFT,   SDL_SCANCODE_LEFT },
	{  180, 560, 140, 150, SDLK_RIGHT,  SDL_SCANCODE_RIGHT },
	{  100, 430, 140, 110, SDLK_UP,     SDL_SCANCODE_UP },
	{  100, 715, 140,  50, SDLK_DOWN,   SDL_SCANCODE_DOWN },
	{  864, 540, 150, 170, SDLK_SPACE,  SDL_SCANCODE_SPACE },
	{  700, 590, 140, 130, SDLK_LCTRL,  SDL_SCANCODE_LCTRL },
	{  914,  10, 100,  70, SDLK_ESCAPE, SDL_SCANCODE_ESCAPE },
	{  790,  10, 100,  70, SDLK_RETURN, SDL_SCANCODE_RETURN },
};
#define NPAD (sizeof(pad) / sizeof(pad[0]))

static struct {
	int fd, flipx, flipy, xmin, xmax, ymin, ymax;
	struct { int id, x, y; } slot[10];
	int cur;
	volatile unsigned down;		/* bit per button */
} T = { .fd = -1 };

static void pad_key(int i, int press)
{
	SDL_Event e;

	memset(&e, 0, sizeof(e));
	e.type = press ? SDL_KEYDOWN : SDL_KEYUP;
	e.key.state = press ? SDL_PRESSED : SDL_RELEASED;
	e.key.keysym.sym = pad[i].key;
	e.key.keysym.scancode = pad[i].scan;
	SDL_PushEvent(&e);
}

/* display pixels -> window coordinates: to_fb inverted (no viewport, no scale) */
static void fb_to_window(float fx, float fy, float *wx, float *wy)
{
	if (G.rotate) {
		*wx = fy / G.os;
		*wy = (G.fbw - fx) / G.os;
	} else {
		*wx = fx / G.os;
		*wy = fy / G.os;
	}
}

static void pad_update(void)
{
	unsigned now = 0, changed;
	int s, i;

	for (s = 0; s < 10; s++) {
		float fx, fy, wx, wy, kx = G.ww / 1024.f, ky = G.wh / 768.f;

		if (T.slot[s].id < 0)
			continue;
		fx = (float)(T.slot[s].x - T.xmin) / (T.xmax - T.xmin) * G.fbw;
		fy = (float)(T.slot[s].y - T.ymin) / (T.ymax - T.ymin) * G.fbh;
		if (T.flipx)
			fx = G.fbw - fx;
		if (T.flipy)
			fy = G.fbh - fy;
		fb_to_window(fx, fy, &wx, &wy);
		for (i = 0; i < (int)NPAD; i++)
			if (wx >= pad[i].x * kx && wx < (pad[i].x + pad[i].w) * kx &&
			    wy >= pad[i].y * ky && wy < (pad[i].y + pad[i].h) * ky)
				now |= 1u << i;
	}
	changed = now ^ T.down;
	for (i = 0; i < (int)NPAD; i++)
		if (changed & (1u << i))
			pad_key(i, now & (1u << i));
	T.down = now;
}

static void *pad_thread(void *arg)
{
	struct input_event ev[32];
	int n, i;

	(void)arg;
	for (i = 0; i < 10; i++)
		T.slot[i].id = -1;
	while ((n = read(T.fd, ev, sizeof(ev))) > 0)
		for (i = 0; i < n / (int)sizeof(ev[0]); i++) {
			if (ev[i].type == EV_SYN && ev[i].code == SYN_REPORT) {
				pad_update();
				continue;
			}
			if (ev[i].type != EV_ABS)
				continue;
			switch (ev[i].code) {
			case ABS_MT_SLOT:
				T.cur = ev[i].value >= 0 && ev[i].value < 10 ? ev[i].value : 0;
				break;
			case ABS_MT_TRACKING_ID:
				T.slot[T.cur].id = ev[i].value;
				break;
			case ABS_MT_POSITION_X:
				T.slot[T.cur].x = ev[i].value;
				break;
			case ABS_MT_POSITION_Y:
				T.slot[T.cur].y = ev[i].value;
				break;
			}
		}
	return NULL;
}

static void pad_start(void)
{
	const char *dev = getenv("G9PSDL_TOUCH"), *flip = getenv("G9PSDL_TOUCH_FLIP");
	struct input_absinfo ax, ay;
	pthread_t th;

	if (getenv("G9PSDL_PAD") && !atoi(getenv("G9PSDL_PAD")))
		return;
	T.flipx = flip && strchr(flip, 'x');
	T.flipy = flip && strchr(flip, 'y');
	T.fd = open(dev ? dev : "/dev/input/event0", O_RDONLY | O_CLOEXEC);
	if (T.fd >= 0 && !ioctl(T.fd, EVIOCGABS(ABS_MT_POSITION_X), &ax) &&
	    !ioctl(T.fd, EVIOCGABS(ABS_MT_POSITION_Y), &ay) && ax.maximum > ax.minimum &&
	    ay.maximum > ay.minimum) {
		T.xmin = ax.minimum;
		T.xmax = ax.maximum;
		T.ymin = ay.minimum;
		T.ymax = ay.maximum;
	} else {
		T.xmin = T.ymin = 0;
		T.xmax = T.ymax = 4096;
	}
	if (T.fd < 0 || pthread_create(&th, NULL, pad_thread, NULL)) {
		fprintf(stderr, "g9psdl: no touch input\n");
		if (T.fd >= 0)
			close(T.fd);
		T.fd = -1;
		return;
	}
	pthread_detach(th);
}

/* ---- textures ---------------------------------------------------------------- */

static struct tex *find(SDL_Texture *t)
{
	unsigned i;

	if (!G.cap || !t)
		return NULL;
	for (i = slot_of(t); G.tex[i].t; i = (i + 1) & (G.cap - 1))
		if (G.tex[i].t == t)
			return &G.tex[i];
	return NULL;
}

static struct tex *add(SDL_Texture *t)
{
	struct tex *x;
	unsigned i;

	if ((x = find(t)))
		goto init;
	if (2 * (G.ntex + 1) > G.cap) {
		struct tex *old = G.tex;
		int ocap = G.cap, k;

		G.cap = G.cap ? 2 * G.cap : 1024;
		G.tex = calloc(G.cap, sizeof(*G.tex));
		for (k = 0; k < ocap; k++)
			if (old[k].t) {
				for (i = slot_of(old[k].t); G.tex[i].t; i = (i + 1) & (G.cap - 1))
					;
				G.tex[i] = old[k];
			}
		free(old);
		if (G.target)		/* moved with the table */
			G.target = find(G.target->t);
	}
	for (i = slot_of(t); G.tex[i].t; i = (i + 1) & (G.cap - 1))
		;
	x = &G.tex[i];
	G.ntex++;
init:
	memset(x, 0, sizeof(*x));
	x->t = t;
	x->id = -1;
	x->r = x->g = x->b = x->a = 255;
	x->blend = SDL_BLENDMODE_BLEND;
	return x;
}

static void del(struct tex *x)
{
	unsigned i = x - G.tex, j = i, k;

	G.tex[i].t = NULL;
	G.ntex--;
	for (;;) {
		j = (j + 1) & (G.cap - 1);
		if (!G.tex[j].t)
			return;
		k = slot_of(G.tex[j].t);
		if ((j > i && (k <= i || k > j)) || (j < i && (k <= i && k > j))) {
			G.tex[i] = G.tex[j];
			G.tex[j].t = NULL;
			i = j;
		}
	}
}

static void start(SDL_Window *w)
{
	const char *rot = getenv("G9PSDL_ROTATE"), *sc = getenv("G9PSDL_SCALE");
	int ret;

	if (G.on)
		return;
	if ((ret = g9p2d_open())) {
		fprintf(stderr, "g9psdl: g9p2d_open: %d (%s)\n", ret, strerror(-ret));
		exit(1);
	}
	G.fbw = g9p2d_width();
	G.fbh = g9p2d_height();
	SDL_GetWindowSize(w, &G.ww, &G.wh);
	G.rotate = rot ? atoi(rot) : G.ww > G.wh;
	{
		float ax = (float)(G.rotate ? G.fbh : G.fbw) / G.ww, ay = (float)(G.rotate ? G.fbw : G.fbh) / G.wh;

		G.os = sc ? atof(sc) : ax < ay ? ax : ay;
	}
	{
		Uint32 px[16];
		int i;

		for (i = 0; i < 16; i++)
			px[i] = 0xffffffff;
		G.white = g9p2d_texture(px, 4, 4);
	}
	G.on = 1;
	pad_start();
	fprintf(stderr, "g9psdl: on, display %dx%d, window %dx%d%s, x%.2f\n", G.fbw, G.fbh, G.ww, G.wh,
		G.rotate ? " turned" : "", G.os);
}

/* renderer coordinates -> the display, or the target texture */
static void to_fb(float x, float y, float *fx, float *fy)
{
	float ox = ((G.has_vp ? G.vp.x : 0) + x) * G.sx;
	float oy = ((G.has_vp ? G.vp.y : 0) + y) * G.sy;

	if (G.target) {
		*fx = ox;
		*fy = oy;
	} else if (G.rotate) {		/* window x -> display y, window y -> display right to left */
		*fx = G.fbw - oy * G.os;
		*fy = ox * G.os;
	} else {
		*fx = ox * G.os;
		*fy = oy * G.os;
	}
}

static void corners_to_fb(const float in[8], float out[8])
{
	int i;

	for (i = 0; i < 4; i++)
		to_fb(in[2 * i], in[2 * i + 1], &out[2 * i], &out[2 * i + 1]);
}

static int clip(float *x, float *y, float *w, float *h, float uv[4])
{
	float x0, y0, x1, y1, cx0, cy0, cx1, cy1, du, dv;

	if (!G.has_clip)
		return *w > 0 && *h > 0;
	x0 = *x; y0 = *y; x1 = *x + *w; y1 = *y + *h;
	cx0 = G.clip.x; cy0 = G.clip.y; cx1 = cx0 + G.clip.w; cy1 = cy0 + G.clip.h;
	du = (uv[2] - uv[0]) / (*w ? *w : 1);
	dv = (uv[3] - uv[1]) / (*h ? *h : 1);
	if (x0 < cx0) { uv[0] += (cx0 - x0) * du; x0 = cx0; }
	if (y0 < cy0) { uv[1] += (cy0 - y0) * dv; y0 = cy0; }
	if (x1 > cx1) { uv[2] -= (x1 - cx1) * du; x1 = cx1; }
	if (y1 > cy1) { uv[3] -= (y1 - cy1) * dv; y1 = cy1; }
	*x = x0; *y = y0; *w = x1 - x0; *h = y1 - y0;
	return *w > 0 && *h > 0;
}

static int mode_of(SDL_BlendMode b)
{
	return b == SDL_BLENDMODE_NONE ? G9P2D_NONE : b == SDL_BLENDMODE_ADD ? G9P2D_ADD :
	       b == SDL_BLENDMODE_MOD ? G9P2D_MOD : G9P2D_BLEND;
}

static void fill(float x, float y, float w, float h)
{
	float uv[4] = { 0, 0, 1, 1 }, c[8], f[8];

	if (!clip(&x, &y, &w, &h, uv))
		return;
	c[0] = x; c[1] = y; c[2] = x + w; c[3] = y; c[4] = x + w; c[5] = y + h; c[6] = x; c[7] = y + h;
	corners_to_fb(c, f);
	g9p2d_color(G.dr / 255.f, G.dg / 255.f, G.db / 255.f, G.da / 255.f);
	g9p2d_blend(mode_of(G.dblend));
	g9p2d_fill4(f);
}

/* ---- the intercepted calls ------------------------------------------------ */

SDL_Renderer *SDL_CreateRenderer(SDL_Window *w, int index, Uint32 flags)
{
	REAL(SDL_CreateRenderer);
	(void)flags;
	G.r = real_SDL_CreateRenderer(w, index, SDL_RENDERER_SOFTWARE | SDL_RENDERER_TARGETTEXTURE);
	if (G.r)
		start(w);
	return G.r;
}

SDL_Texture *SDL_CreateTexture(SDL_Renderer *r, Uint32 format, int access, int w, int h)
{
	SDL_Texture *t;
	struct tex *x;
	REAL(SDL_CreateTexture);

	t = real_SDL_CreateTexture(r, format, access, w, h);
	if (t && G.on) {
		x = add(t);
		x->w = w;
		x->h = h;
		x->target = access == SDL_TEXTUREACCESS_TARGET;
		x->id = g9p2d_texture(NULL, w, h);	/* zeros: transparent black */
		if (x->id < 0)
			x->id = G.white;
	}
	return t;
}

SDL_Texture *SDL_CreateTextureFromSurface(SDL_Renderer *r, SDL_Surface *s)
{
	SDL_Texture *t;
	SDL_Surface *c;
	struct tex *x;
	REAL(SDL_CreateTextureFromSurface);

	t = real_SDL_CreateTextureFromSurface(r, s);
	if (!t || !G.on)
		return t;
	x = add(t);
	x->w = s->w;
	x->h = s->h;
	x->id = G.white;
	c = SDL_ConvertSurfaceFormat(s, SDL_PIXELFORMAT_ABGR8888, 0);	/* R G B A bytes */
	if (c) {
		Uint32 *px = malloc((size_t)c->w * c->h * 4);
		int j, id;

		SDL_LockSurface(c);
		for (j = 0; j < c->h; j++)
			memcpy(px + (size_t)j * c->w, (Uint8 *)c->pixels + (size_t)j * c->pitch, (size_t)c->w * 4);
		SDL_UnlockSurface(c);
		id = g9p2d_texture(px, c->w, c->h);
		free(px);
		SDL_FreeSurface(c);
		if (id >= 0)
			x->id = id;
		else
			fprintf(stderr, "g9psdl: texture %dx%d: %d\n", s->w, s->h, id);
	}
	return t;
}

int SDL_UpdateTexture(SDL_Texture *t, const SDL_Rect *rect, const void *pixels, int pitch)
{
	struct tex *x = find(t);
	Uint32 fmt;
	REAL(SDL_UpdateTexture);

	if (x && x->id != G.white && !rect && !SDL_QueryTexture(t, &fmt, NULL, NULL, NULL)) {
		Uint32 *px = malloc((size_t)x->w * x->h * 4);

		if (px && !SDL_ConvertPixels(x->w, x->h, fmt, pixels, pitch, SDL_PIXELFORMAT_ABGR8888, px,
					     x->w * 4))
			g9p2d_texture_update(x->id, px);
		free(px);
	}
	return real_SDL_UpdateTexture(t, rect, pixels, pitch);
}

void SDL_DestroyTexture(SDL_Texture *t)
{
	struct tex *x = find(t);
	REAL(SDL_DestroyTexture);

	if (x) {
		if (G.target == x) {
			G.target = NULL;
			g9p2d_target(0);
		}
		if (x->id != G.white && x->id >= 0)
			g9p2d_texture_free(x->id);
		del(x);
	}
	real_SDL_DestroyTexture(t);
}

int SDL_SetTextureColorMod(SDL_Texture *t, Uint8 r, Uint8 g, Uint8 b)
{
	struct tex *x = find(t);
	REAL(SDL_SetTextureColorMod);

	if (x) { x->r = r; x->g = g; x->b = b; }
	return real_SDL_SetTextureColorMod(t, r, g, b);
}

int SDL_SetTextureAlphaMod(SDL_Texture *t, Uint8 a)
{
	struct tex *x = find(t);
	REAL(SDL_SetTextureAlphaMod);

	if (x) x->a = a;
	return real_SDL_SetTextureAlphaMod(t, a);
}

int SDL_SetTextureBlendMode(SDL_Texture *t, SDL_BlendMode b)
{
	struct tex *x = find(t);
	REAL(SDL_SetTextureBlendMode);

	if (x) x->blend = b;
	return real_SDL_SetTextureBlendMode(t, b);
}

int SDL_SetRenderDrawColor(SDL_Renderer *r, Uint8 cr, Uint8 cg, Uint8 cb, Uint8 ca)
{
	REAL(SDL_SetRenderDrawColor);
	G.dr = cr; G.dg = cg; G.db = cb; G.da = ca;
	return real_SDL_SetRenderDrawColor(r, cr, cg, cb, ca);
}

int SDL_SetRenderDrawBlendMode(SDL_Renderer *r, SDL_BlendMode b)
{
	REAL(SDL_SetRenderDrawBlendMode);
	G.dblend = b;
	return real_SDL_SetRenderDrawBlendMode(r, b);
}

int SDL_RenderSetViewport(SDL_Renderer *r, const SDL_Rect *rect)
{
	REAL(SDL_RenderSetViewport);
	G.has_vp = rect != NULL;
	if (rect)
		G.vp = *rect;
	return real_SDL_RenderSetViewport(r, rect);
}

int SDL_RenderSetScale(SDL_Renderer *r, float sx, float sy)
{
	REAL(SDL_RenderSetScale);
	G.sx = sx; G.sy = sy;
	return real_SDL_RenderSetScale(r, sx, sy);
}

int SDL_RenderSetClipRect(SDL_Renderer *r, const SDL_Rect *rect)
{
	REAL(SDL_RenderSetClipRect);
	G.has_clip = rect != NULL;
	if (rect)
		G.clip = *rect;
	return real_SDL_RenderSetClipRect(r, rect);
}

int SDL_SetRenderTarget(SDL_Renderer *r, SDL_Texture *t)
{
	struct tex *x = find(t);
	REAL(SDL_SetRenderTarget);

	G.target = x && x->id != G.white ? x : NULL;
	if (G.on)
		g9p2d_target(G.target ? G.target->id : 0);
	/* SDL resets the viewport and the clip rect with the target */
	G.has_vp = 0;
	G.has_clip = 0;
	return real_SDL_SetRenderTarget(r, t);
}

/* the size drawing covers: the target texture or the window */
static void out_size(int *w, int *h)
{
	if (G.target) {
		*w = G.target->w;
		*h = G.target->h;
	} else
		SDL_GetRendererOutputSize(G.r, w, h);
}

int SDL_RenderClear(SDL_Renderer *r)
{
	SDL_BlendMode b = G.dblend;
	int w = 0, h = 0, hc = G.has_clip, hv = G.has_vp;

	(void)r;
	if (!G.on)
		return 0;
	out_size(&w, &h);
	G.dblend = SDL_BLENDMODE_NONE;
	G.has_clip = G.has_vp = 0;
	fill(0, 0, w / G.sx, h / G.sy);
	G.has_clip = hc; G.has_vp = hv;
	G.dblend = b;
	return 0;
}

int SDL_RenderFillRect(SDL_Renderer *r, const SDL_Rect *rect)
{
	(void)r;
	if (!G.on)
		return 0;
	if (!rect) {
		int w = 0, h = 0;

		out_size(&w, &h);
		fill(0, 0, w / G.sx, h / G.sy);
	} else
		fill(rect->x, rect->y, rect->w, rect->h);
	return 0;
}

int SDL_RenderFillRects(SDL_Renderer *r, const SDL_Rect *rects, int n)
{
	int i;

	for (i = 0; i < n; i++)
		SDL_RenderFillRect(r, &rects[i]);
	return 0;
}

int SDL_RenderDrawLine(SDL_Renderer *r, int x1, int y1, int x2, int y2)
{
	float dx = x2 - x1, dy = y2 - y1, l = sqrtf(dx * dx + dy * dy), nx, ny, c[8], f[8];

	(void)r;
	if (!G.on)
		return 0;
	if (l < 0.5f) {
		fill(x1, y1, 1, 1);
		return 0;
	}
	nx = -dy / l * 0.5f; ny = dx / l * 0.5f;	/* a one-pixel-wide quad */
	c[0] = x1 + nx; c[1] = y1 + ny; c[2] = x2 + nx; c[3] = y2 + ny;
	c[4] = x2 - nx; c[5] = y2 - ny; c[6] = x1 - nx; c[7] = y1 - ny;
	corners_to_fb(c, f);
	g9p2d_color(G.dr / 255.f, G.dg / 255.f, G.db / 255.f, G.da / 255.f);
	g9p2d_blend(mode_of(G.dblend));
	g9p2d_fill4(f);
	return 0;
}

static int copy(SDL_Texture *t, const SDL_Rect *src, const SDL_Rect *dst,
		double angle, const SDL_Point *center, SDL_RendererFlip flip)
{
	struct tex *x = find(t);
	float dx, dy, dw, dh, uv[4], c[8], f[8], u[8];
	int w = 0, h = 0;

	if (!G.on || !x || x == G.target)
		return 0;
	if (dst) {
		dx = dst->x; dy = dst->y; dw = dst->w; dh = dst->h;
	} else {
		out_size(&w, &h);
		dx = 0; dy = 0; dw = w / G.sx; dh = h / G.sy;
	}
	if (src) {
		uv[0] = (float)src->x / x->w; uv[1] = (float)src->y / x->h;
		uv[2] = (float)(src->x + src->w) / x->w; uv[3] = (float)(src->y + src->h) / x->h;
	} else {
		uv[0] = 0; uv[1] = 0; uv[2] = 1; uv[3] = 1;
	}
	if (flip & SDL_FLIP_HORIZONTAL) { float s = uv[0]; uv[0] = uv[2]; uv[2] = s; }
	if (flip & SDL_FLIP_VERTICAL) { float s = uv[1]; uv[1] = uv[3]; uv[3] = s; }
	if (angle == 0) {
		if (!clip(&dx, &dy, &dw, &dh, uv))
			return 0;
		c[0] = dx; c[1] = dy; c[2] = dx + dw; c[3] = dy;
		c[4] = dx + dw; c[5] = dy + dh; c[6] = dx; c[7] = dy + dh;
	} else {			/* turned about center (dst-relative), clockwise */
		float cx = dx + (center ? center->x : dw / 2), cy = dy + (center ? center->y : dh / 2);
		float a = angle * M_PI / 180, ca = cosf(a), sa = sinf(a);
		float px[4] = { dx, dx + dw, dx + dw, dx }, py[4] = { dy, dy, dy + dh, dy + dh };
		int i;

		for (i = 0; i < 4; i++) {
			c[2 * i] = cx + (px[i] - cx) * ca - (py[i] - cy) * sa;
			c[2 * i + 1] = cy + (px[i] - cx) * sa + (py[i] - cy) * ca;
		}
	}
	u[0] = uv[0]; u[1] = uv[1]; u[2] = uv[2]; u[3] = uv[1];
	u[4] = uv[2]; u[5] = uv[3]; u[6] = uv[0]; u[7] = uv[3];
	corners_to_fb(c, f);
	g9p2d_color(x->r / 255.f, x->g / 255.f, x->b / 255.f, x->a / 255.f);
	g9p2d_blend(mode_of(x->blend));
	g9p2d_quad4(x->id, f, u);
	return 0;
}

int SDL_RenderCopy(SDL_Renderer *r, SDL_Texture *t, const SDL_Rect *src, const SDL_Rect *dst)
{
	(void)r;
	return copy(t, src, dst, 0, NULL, SDL_FLIP_NONE);
}

int SDL_RenderCopyEx(SDL_Renderer *r, SDL_Texture *t, const SDL_Rect *src, const SDL_Rect *dst,
		     const double angle, const SDL_Point *center, const SDL_RendererFlip flip)
{
	(void)r;
	return copy(t, src, dst, angle, center, flip);
}

/* the buttons, over everything, in window coordinates */
static void pad_draw(void)
{
	float kx = G.ww / 1024.f, ky = G.wh / 768.f;
	int i;

	if (T.fd < 0)
		return;
	g9p2d_target(0);
	for (i = 0; i < (int)NPAD; i++) {
		float x0 = pad[i].x * kx, y0 = pad[i].y * ky, x1 = (pad[i].x + pad[i].w) * kx;
		float y1 = (pad[i].y + pad[i].h) * ky;
		float c[8] = { x0, y0, x1, y0, x1, y1, x0, y1 }, f[8];
		int k;

		for (k = 0; k < 4; k++) {	/* turn and scale only: no viewport, no render scale */
			f[2 * k] = G.rotate ? G.fbw - c[2 * k + 1] * G.os : c[2 * k] * G.os;
			f[2 * k + 1] = G.rotate ? c[2 * k] * G.os : c[2 * k + 1] * G.os;
		}
		g9p2d_blend(G9P2D_BLEND);
		g9p2d_color(1, 1, 1, T.down & (1u << i) ? 0.45f : 0.16f);
		g9p2d_fill4(f);
	}
	if (G.target)
		g9p2d_target(G.target->id);
}

void SDL_RenderPresent(SDL_Renderer *r)
{
	int ret;

	(void)r;
	if (!G.on)
		return;
	pad_draw();
	if ((ret = g9p2d_end()))
		fprintf(stderr, "g9psdl: frame %u: %d\n", G.frames, ret);
	if (getenv("G9PSDL_FPS") && G.frames % 100 == 50) {
		int q, tiles, ent, drop;
		double us;

		g9p2d_frame_stats(&q, &tiles, &ent, &drop, &us);
		fprintf(stderr, "g9psdl: frame %u: %d quads, %d tiles, %d entries, %d dropped\n", G.frames, q,
			tiles, ent, drop);
	}
	if (G.target)
		g9p2d_target(G.target->id);
	G.frames++;
	if (getenv("G9PSDL_FPS") && G.frames % 100 == 0) {
		static Uint32 last;
		Uint32 now = SDL_GetTicks();

		if (last)
			fprintf(stderr, "g9psdl: %.1f fps\n", 100000.0 / (now - last));
		last = now;
	}
}
