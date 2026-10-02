/* sgxsdl -- SDL2's 2D renderer on the SGX, as an LD_PRELOAD shim.
 *
 *   LD_PRELOAD=./libsgxsdl.so SGXSDL_PACK=/root/sgx2d2 SDL_VIDEODRIVER=offscreen \
 *       supertux2 --renderer sdl --geometry 1024x768
 *
 * The real SDL keeps the window, the events and its own bookkeeping (a
 * software renderer nobody presents); every drawing call is taken here and
 * drawn with sgx2d, straight into the framebuffer.  SGXSDL_ROTATE=1 (the
 * default) turns a landscape window onto the portrait screen.
 *
 * Covered: textures from surfaces, colour/alpha mod, blend modes (NONE is
 * drawn as BLEND), viewport, scale, clip rect, RenderCopy(Ex) with rotation
 * and flips, fills, lines, clear, present.  Render targets are not: drawing
 * into one is dropped and the target reads as white, so a lightmap applied
 * with MOD leaves the scene as it is.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <SDL2/SDL.h>
#include <dlfcn.h>
#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "sgx2d.h"

#define REAL(name) static __typeof__(name) *real_##name; \
	if (!real_##name) real_##name = dlsym(RTLD_NEXT, #name)

struct tex {
	SDL_Texture *t;
	int id, w, h, target;
	Uint8 r, g, b, a;
	SDL_BlendMode blend;
};

static struct {
	int on, rotate, fbw, fbh, white;
	SDL_Renderer *r;
	Uint8 dr, dg, db, da;
	SDL_BlendMode dblend;
	SDL_Rect vp;
	int has_vp, has_clip;
	SDL_Rect clip;
	float sx, sy;
	SDL_Texture *target;
	struct tex *tex;
	int ntex, cap;
	unsigned frames;
} G = { .sx = 1, .sy = 1, .da = 255 };

/* SDL_Texture * -> struct tex: open addressing, linear probing, the table
 * kept at most half full (SuperTux has thousands of textures, and every
 * draw call looks its texture up several times) */
static unsigned slot_of(SDL_Texture *t)
{
	uintptr_t k = (uintptr_t)t;

	k ^= k >> 16;
	k *= 0x45d9f3b;
	k ^= k >> 16;
	return k & (G.cap - 1);
}

static struct tex *find(SDL_Texture *t)
{
	unsigned i;

	if (!G.cap)
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
		goto init;		/* SDL made it through our CreateTexture already */
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
	for (;;) {			/* move the rest of the run back over the hole */
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

static void start(void)
{
	const char *pack = getenv("SGXSDL_PACK");
	const char *rot = getenv("SGXSDL_ROTATE");
	int ret;

	if (G.on)
		return;
	if ((ret = sgx2d_open(pack ? pack : "/root/sgx2d2"))) {
		fprintf(stderr, "sgxsdl: sgx2d_open: %d\n", ret);
		exit(1);
	}
	G.fbw = sgx2d_width();
	G.fbh = sgx2d_height();
	G.rotate = !rot || atoi(rot);
	{
		Uint32 px[16];
		int i;

		for (i = 0; i < 16; i++)
			px[i] = 0xffffffff;
		G.white = sgx2d_texture(px, 4, 4);
	}
	sgx2d_begin(G.white);
	G.on = 1;
	fprintf(stderr, "sgxsdl: on, %dx%d%s\n", G.fbw, G.fbh, G.rotate ? ", rotated" : "");
}

/* renderer coordinates -> output pixels -> framebuffer */
static void to_fb(float x, float y, float *fx, float *fy)
{
	float ox = ((G.has_vp ? G.vp.x : 0) + x) * G.sx;
	float oy = ((G.has_vp ? G.vp.y : 0) + y) * G.sy;

	if (G.rotate) {			/* window x -> screen y, window y -> screen right-to-left */
		*fx = G.fbw - oy;
		*fy = ox;
	} else {
		*fx = ox;
		*fy = oy;
	}
}

static void corners_to_fb(const float in[8], float out[8])
{
	int i;

	for (i = 0; i < 4; i++)
		to_fb(in[2 * i], in[2 * i + 1], &out[2 * i], &out[2 * i + 1]);
}

/* clip an axis-aligned rectangle (renderer coordinates) and its uv; 0 if
 * nothing is left */
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
	return b == SDL_BLENDMODE_ADD ? SGX2D_ADD : b == SDL_BLENDMODE_MOD ? SGX2D_MOD : SGX2D_BLEND;
}

static void fill(float x, float y, float w, float h)
{
	float uv[4] = { 0, 0, 1, 1 }, c[8], f[8];

	if (getenv("SGXSDL_TRACE") && G.frames % 200 == 100)
		fprintf(stderr, "fill %.0f,%.0f %.0fx%.0f rgba %d %d %d %d blend %d target %p clip %d "
			"%d,%d %dx%d vp %d %d,%d %dx%d\n",
			x, y, w, h, G.dr, G.dg, G.db, G.da, G.dblend, (void *)G.target, G.has_clip,
			G.clip.x, G.clip.y, G.clip.w, G.clip.h, G.has_vp, G.vp.x, G.vp.y, G.vp.w, G.vp.h);

	if (G.target || !clip(&x, &y, &w, &h, uv))
		return;
	c[0] = x; c[1] = y; c[2] = x + w; c[3] = y; c[4] = x + w; c[5] = y + h; c[6] = x; c[7] = y + h;
	corners_to_fb(c, f);
	sgx2d_color(G.dr / 255.f, G.dg / 255.f, G.db / 255.f,
		    G.dblend == SDL_BLENDMODE_NONE ? 1 : G.da / 255.f);
	sgx2d_blend(mode_of(G.dblend));
	sgx2d_fill4(f);
}

/* ---- the intercepted calls ------------------------------------------------ */

SDL_Renderer *SDL_CreateRenderer(SDL_Window *w, int index, Uint32 flags)
{
	REAL(SDL_CreateRenderer);
	(void)flags;
	G.r = real_SDL_CreateRenderer(w, index, SDL_RENDERER_SOFTWARE);
	if (G.r)
		start();
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
		x->w = w; x->h = h;
		x->target = access == SDL_TEXTUREACCESS_TARGET;
		x->id = G.white;		/* targets read as white; others get pixels later */
	}
	return t;
}

/* the power of two a texture edge is stored at (at most 1024) */
static int pow2_for(int n)
{
	int p = 4;

	while (p < n && p < 1024)
		p *= 2;
	/* a little over the smaller one: shrink to it -- 25% for small images,
	 * 50% above 512 (backgrounds; texel memory is the scarce thing) */
	if (p > n && p / 2 >= 4 && n * 4 <= p / 2 * (p / 2 >= 512 ? 6 : 5))
		p /= 2;
	return p;
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
	x->w = s->w; x->h = s->h;
	x->id = G.white;
	c = SDL_ConvertSurfaceFormat(s, SDL_PIXELFORMAT_ABGR8888, 0);	/* R G B A bytes */
	if (c) {
		int tw = pow2_for(c->w), th = pow2_for(c->h), i, j, id;
		Uint32 *px = malloc(tw * th * 4);

		/* resampled to powers of two, so no texel memory is padding: a
		 * little over one is shrunk to it, more is stretched to the next */
		SDL_LockSurface(c);
		for (j = 0; j < th; j++) {
			const Uint32 *row = (const Uint32 *)((Uint8 *)c->pixels +
							     (j * c->h / th) * c->pitch);

			for (i = 0; i < tw; i++)
				px[j * tw + i] = row[i * c->w / tw];
		}
		SDL_UnlockSurface(c);
		id = sgx2d_texture(px, tw, th);
		if (getenv("SGXSDL_DEBUG") && tw * th >= 256 * 256)
			fprintf(stderr, "sgxsdl: big texture %dx%d as %dx%d -> %d at 0x%x\n",
				c->w, c->h, tw, th, id, sgx2d_texture_addr(id));
		free(px);
		SDL_FreeSurface(c);
		if (id >= 0)
			x->id = id;
		else {
			int n;
			uint32_t hf, eu;

			sgx2d_stats(&n, &hf, &eu);
			fprintf(stderr, "sgxsdl: texture %dx%d: %d (%d textures, heap free %u KiB, "
				"blocks %u KiB)\n", s->w, s->h, id, n, hf >> 10, eu >> 10);
		}
	}
	return t;
}

void SDL_DestroyTexture(SDL_Texture *t)
{
	struct tex *x = find(t);
	REAL(SDL_DestroyTexture);

	if (x) {
		if (x->id != G.white && x->id >= 0)
			sgx2d_texture_free(x->id);
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
	REAL(SDL_SetRenderTarget);
	G.target = t;
	return real_SDL_SetRenderTarget(r, t);
}

int SDL_RenderClear(SDL_Renderer *r)
{
	SDL_BlendMode b = G.dblend;
	int w = 0, h = 0, hc = G.has_clip, hv = G.has_vp;

	(void)r;
	if (!G.on || G.target)
		return 0;
	SDL_GetRendererOutputSize(G.r, &w, &h);
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

		SDL_GetRendererOutputSize(G.r, &w, &h);
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
	if (!G.on || G.target)
		return 0;
	if (l < 0.5f) {
		fill(x1, y1, 1, 1);
		return 0;
	}
	nx = -dy / l * 0.5f; ny = dx / l * 0.5f;	/* a one-pixel-wide quad */
	c[0] = x1 + nx; c[1] = y1 + ny; c[2] = x2 + nx; c[3] = y2 + ny;
	c[4] = x2 - nx; c[5] = y2 - ny; c[6] = x1 - nx; c[7] = y1 - ny;
	corners_to_fb(c, f);
	sgx2d_color(G.dr / 255.f, G.dg / 255.f, G.db / 255.f,
		    G.dblend == SDL_BLENDMODE_NONE ? 1 : G.da / 255.f);
	sgx2d_blend(mode_of(G.dblend));
	sgx2d_fill4(f);
	return 0;
}

static int copy(SDL_Texture *t, const SDL_Rect *src, const SDL_Rect *dst,
		double angle, const SDL_Point *center, SDL_RendererFlip flip)
{
	struct tex *x = find(t);
	float dx, dy, dw, dh, uv[4], c[8], f[8], u[8];
	int w = 0, h = 0;

	if (getenv("SGXSDL_TRACE") && x && G.frames % 200 == 100)
		fprintf(stderr, "copy %p id %d %dx%d src %d,%d %dx%d dst %d,%d %dx%d mod %d %d %d %d "
			"blend %d target %p angle %.1f vp %d sc %.2f\n", (void *)t, x->id, x->w, x->h,
			src ? src->x : -1, src ? src->y : -1, src ? src->w : -1, src ? src->h : -1,
			dst ? dst->x : -1, dst ? dst->y : -1, dst ? dst->w : -1, dst ? dst->h : -1,
			x->r, x->g, x->b, x->a, x->blend, (void *)G.target, angle, G.has_vp, G.sx);
	if (!G.on || G.target || !x)
		return 0;
	if (dst) {
		dx = dst->x; dy = dst->y; dw = dst->w; dh = dst->h;
	} else {
		SDL_GetRendererOutputSize(G.r, &w, &h);
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
	sgx2d_color(x->r / 255.f, x->g / 255.f, x->b / 255.f, x->a / 255.f);
	sgx2d_blend(mode_of(x->blend));
	sgx2d_quad4(x->id, f, u);
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

void SDL_RenderPresent(SDL_Renderer *r)
{
	int ret;

	(void)r;
	if (!G.on)
		return;
	if ((ret = sgx2d_end()))
		fprintf(stderr, "sgxsdl: frame %u: %d\n", G.frames, ret);
	if (getenv("SGXSDL_FPS") && G.frames % 100 == 50) {
		int q, d, x;

		sgx2d_frame_stats(&q, &d, &x);
		fprintf(stderr, "sgxsdl: frame %u: %d quads, %d draws, %d dropped\n", G.frames, q, d, x);
	}
	sgx2d_begin(G.white);
	G.frames++;
	if (getenv("SGXSDL_FPS") && G.frames % 100 == 0) {
		static Uint32 last;
		Uint32 now = SDL_GetTicks();

		if (last)
			fprintf(stderr, "sgxsdl: %.1f fps\n", 100000.0 / (now - last));
		last = now;
	}
}
