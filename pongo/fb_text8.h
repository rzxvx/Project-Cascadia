/* Framebuffer text using font8x8 (same as gpio-poll / fb-text). */
#ifndef FB_TEXT8_H
#define FB_TEXT8_H

#include "font8x8.h"

#ifndef FB_TEXT_W
#define FB_TEXT_W 768
#endif
#ifndef FB_TEXT_H
#define FB_TEXT_H 1024
#endif
#ifndef FB_TEXT_STRIDE
#define FB_TEXT_STRIDE 3072
#endif
#ifndef FB_TEXT_SCANOUT
#define FB_TEXT_SCANOUT 0x9F6FC000u
#endif

typedef unsigned int u32;

static inline void fb_text_pixel(int x, int y, u32 c) {
	if ((u32)x >= FB_TEXT_W || (u32)y >= FB_TEXT_H)
		return;
	*(volatile u32 *)(FB_TEXT_SCANOUT + y * FB_TEXT_STRIDE + x * 4) = c;
}

static inline void fb_text_fill(int x, int y, int w, int h, u32 c) {
	int i, j;
	for (j = 0; j < h; j++)
		for (i = 0; i < w; i++)
			fb_text_pixel(x + i, y + j, c);
}

/* bit0 = leftmost pixel; row 0 = top (matches gpio-poll / fb-text.S). */
static inline void draw_char8(int x, int y, int ch, u32 c, int scale) {
	const unsigned char *g;
	int row, col, dy, dx;
	if (ch < FONT8_FIRST || ch > FONT8_LAST)
		ch = FONT8_FIRST;
	g = &font8x8[(ch - FONT8_FIRST) * FONT8_H];
	for (row = 0; row < FONT8_H; row++) {
		unsigned char bits = g[row];
		for (col = 0; col < FONT8_W; col++) {
			if (bits & 1u) {
				for (dy = 0; dy < scale; dy++)
					for (dx = 0; dx < scale; dx++)
						fb_text_pixel(x + col * scale + dx,
						               y + row * scale + dy, c);
			}
			bits >>= 1;
		}
	}
}

static inline void draw_u32_hex8(int x, int y, u32 v, u32 c, int scale) {
	static const char hexd[] = "0123456789ABCDEF";
	int i, step = FONT8_W * scale;
	for (i = 0; i < 8; i++)
		draw_char8(x + i * step, y, hexd[(v >> (28 - i * 4)) & 15], c, scale);
}

static inline void draw_string8(int x, int y, const char *s, u32 c, int scale) {
	int step = FONT8_W * scale;
	while (*s) {
		draw_char8(x, y, (unsigned char)*s++, c, scale);
		x += step;
	}
}

#endif
