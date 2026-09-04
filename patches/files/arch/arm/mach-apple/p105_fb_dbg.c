// SPDX-License-Identifier: GPL-2.0-only
/*
 * P105AP early FB text debug (phys identity map + WC virt after map_io).
 *
 * Live Recovery scanout: 0x9F6FC000, 768x1024, stride 3072, BGRA.
 *
 * Lifetime: not __init (rest_init can race free_initmem).  Phys identity
 * writes are only safe while early maps live — call p105_fb_dbg_shutdown()
 * before free_initmem; do not stamp FB after schedule_preempt_disabled().
 */

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/sizes.h>
#include <linux/types.h>

#include "font8x8.h"
#include "p105_fb_dbg.h"

#define P105_FB_PHYS	0x9f6fc000ul
#define P105_FB_WIDTH	768
#define P105_FB_HEIGHT	1024
#define P105_FB_STRIDE	3072
#define P105_FB_VIRT	0xF0000000UL

#define P105_FG		0xffffffffu
#define P105_BG		0xff102030u
#define P105_SCALE	2
#define P105_LINE_PX	((FONT8_H * P105_SCALE) + 2)
#define P105_MARGIN_X	8
#define P105_MARGIN_Y	8

extern unsigned long p105_fb_phys;
extern unsigned long p105_fb_stride;

/* Non-zero after apple_s5l_map_io installs WC map at P105_FB_VIRT. */
static int p105_fb_use_virt;
static unsigned p105_dbg_row;
/* Cleared before free_initmem — phys identity map is not safe once idle runs. */
static bool p105_fb_dbg_alive = true;

void p105_fb_dbg_shutdown(void)
{
	p105_fb_dbg_alive = false;
}

void __init p105_fb_dbg_use_virt(void)
{
	p105_fb_use_virt = 1;
}

static unsigned long p105_dbg_base(void)
{
	phys_addr_t section;

	if (!p105_fb_use_virt)
		return P105_FB_PHYS;

	section = (phys_addr_t)p105_fb_phys & ~(SZ_1M - 1);
	if (section < 0x9f000000 || section >= 0x9ff00000)
		section = P105_FB_PHYS & ~(SZ_1M - 1);
	return P105_FB_VIRT + (p105_fb_phys - section);
}

static u32 p105_dbg_stride(void)
{
	u32 s = (u32)p105_fb_stride;

	if (s < 2048 || s > 8192 || (s & 3))
		return P105_FB_STRIDE;
	return s;
}

static void p105_px(unsigned long base, u32 stride, int x, int y, u32 c)
{
	if ((unsigned)x >= P105_FB_WIDTH || (unsigned)y >= P105_FB_HEIGHT)
		return;
	*(volatile u32 *)(base + (unsigned long)y * stride + (unsigned)x * 4) = c;
}

static void p105_fill_rect(unsigned long base, u32 stride,
			   int x, int y, int w, int h, u32 c)
{
	int i, j;

	for (j = 0; j < h; j++)
		for (i = 0; i < w; i++)
			p105_px(base, stride, x + i, y + j, c);
}

static void p105_draw_char(unsigned long base, u32 stride,
			   int x, int y, int ch, u32 c)
{
	const unsigned char *g;
	int row, col, dy, dx;

	if (ch < FONT8_FIRST || ch > FONT8_LAST)
		ch = FONT8_FIRST;
	g = &font8x8[(ch - FONT8_FIRST) * FONT8_H];
	for (row = 0; row < FONT8_H; row++) {
		unsigned char bits = g[row];

		for (col = 0; col < FONT8_W; col++) {
			if (bits & 1u) {
				for (dy = 0; dy < P105_SCALE; dy++)
					for (dx = 0; dx < P105_SCALE; dx++)
						p105_px(base, stride,
							x + col * P105_SCALE + dx,
							y + row * P105_SCALE + dy, c);
			}
			bits >>= 1;
		}
	}
}

void p105_fb_dbg_clear(void)
{
	unsigned long base = p105_dbg_base();
	u32 stride = p105_dbg_stride();
	unsigned y, x;
	u32 *row;

	if (!p105_fb_dbg_alive)
		return;

	p105_fb_phys = P105_FB_PHYS;
	p105_fb_stride = P105_FB_STRIDE;

	for (y = 0; y < P105_FB_HEIGHT; y++) {
		row = (u32 *)(base + (unsigned long)y * stride);
		for (x = 0; x < P105_FB_WIDTH; x++)
			row[x] = P105_BG;
	}
	p105_dbg_row = 0;
}

void p105_fb_dbg(const char *msg)
{
	unsigned long base = p105_dbg_base();
	u32 stride = p105_dbg_stride();
	int x, y, step = FONT8_W * P105_SCALE;
	unsigned max_rows;

	if (!p105_fb_dbg_alive || !msg)
		return;

	max_rows = (P105_FB_HEIGHT - P105_MARGIN_Y) / P105_LINE_PX;
	if (max_rows < 1)
		max_rows = 1;
	if (p105_dbg_row >= max_rows) {
		p105_fb_dbg_clear();
		base = p105_dbg_base();
		stride = p105_dbg_stride();
	}

	y = P105_MARGIN_Y + (int)p105_dbg_row * P105_LINE_PX;
	p105_fill_rect(base, stride, 0, y, P105_FB_WIDTH, P105_LINE_PX, P105_BG);

	x = P105_MARGIN_X;
	while (*msg && x + step <= P105_FB_WIDTH - P105_MARGIN_X) {
		p105_draw_char(base, stride, x, y + 1, (unsigned char)*msg++, P105_FG);
		x += step;
	}
	p105_dbg_row++;
}

void p105_fb_dbg_hex(const char *tag, unsigned long val)
{
	static const char hexd[] = "0123456789ABCDEF";
	char buf[48];
	unsigned i = 0, j;

	if (tag) {
		while (*tag && i < 24)
			buf[i++] = *tag++;
		if (i < 24)
			buf[i++] = ' ';
	}
	for (j = 0; j < 8 && i < sizeof(buf) - 1; j++)
		buf[i++] = hexd[(val >> (28 - j * 4)) & 0xf];
	buf[i] = '\0';
	p105_fb_dbg(buf);
}

void __init p105_ioremap_paint(u32 color, unsigned row0, unsigned nrows)
{
	(void)color;
	(void)row0;
	(void)nrows;
}

void __init p105_stamp_row(unsigned row, u32 color)
{
	(void)row;
	(void)color;
}

void __init p105_stamp_step(unsigned step, u32 color)
{
	char buf[12];

	(void)color;
	buf[0] = 's';
	buf[1] = 't';
	buf[2] = 'e';
	buf[3] = 'p';
	buf[4] = ' ';
	buf[5] = '0' + (step / 10) % 10;
	buf[6] = '0' + step % 10;
	buf[7] = '\0';
	p105_fb_dbg(buf);
}
