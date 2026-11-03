/* 3x5 hex digits — same FB coords as fb-text (y grows downward). */
#ifndef FB_HEX_INCLUDED
#define FB_HEX_INCLUDED

typedef unsigned int u32;
typedef unsigned short u16;

#ifndef FB_HEX_WIDTH
#define FB_HEX_WIDTH 768
#endif
#ifndef FB_HEX_HEIGHT
#define FB_HEX_HEIGHT 1024
#endif
#ifndef FB_HEX_STRIDE
#define FB_HEX_STRIDE 3072
#endif
#ifndef FB_HEX_SCANOUT
#define FB_HEX_SCANOUT 0x9F6FC000u
#endif

static inline void fb_hex_pixel(int x, int y, u32 c) {
	if ((u32)x >= FB_HEX_WIDTH || (u32)y >= FB_HEX_HEIGHT)
		return;
	*(volatile u32 *)(FB_HEX_SCANOUT + y * FB_HEX_STRIDE + x * 4) = c;
}

static inline void fb_hex_fill(int x, int y, int w, int h, u32 c) {
	int i, j;
	for (j = 0; j < h; j++)
		for (i = 0; i < w; i++)
			fb_hex_pixel(x + i, y + j, c);
}

/* Font row 0 = bottom of digit; flip rows so glyphs match fb-text orientation. */
static inline void draw_hex_digit(int x, int y, int d, u32 c, int scale) {
	static const u16 dig[16] = {
		0x7B6F, 0x2492, 0x73E7, 0x79E7, 0x5BE4, 0x79CF, 0x7BCF, 0x7249,
		0x7BEF, 0x79EF, 0x7BED, 0x3BCF, 0x724F, 0x3B6F, 0x73CF, 0x724F
	};
	u16 m = dig[d & 15];
	int row, col;
	for (row = 0; row < 5; row++) {
		int py = y + (4 - row) * scale;
		for (col = 0; col < 3; col++) {
			if (m & (1u << (row * 3 + col)))
				fb_hex_fill(x + col * scale, py, scale, scale, c);
		}
	}
}

static inline void draw_u32_hex(int x, int y, u32 v, u32 c, int scale) {
	int i, step = scale * 4;
	for (i = 0; i < 8; i++)
		draw_hex_digit(x + i * step, y, (v >> (28 - i * 4)) & 15, c, scale);
}

#endif
