/*
 * linux_boot — iBEC ACE trampoline @ 0x80000000.
 *
 * No color bars. Status via fb_text8 @ scanout 0x9F6FC000 (768x1024).
 * Bundle layout: [this loader][pad to 0x8000][zImage-dtb @ 0x80008000]
 *
 * Returns DTB phys to asm, which disables MMU/caches and jumps.
 */
typedef unsigned int u32;

#include "fb_text8.h"
#include "staging-generated.h"

#define BG		0xFF102030u
#define FG		0xFFFFFFFFu
#define ACC		0xFF00FF88u
#define DIM		0xFF808090u
#define SCALE		2
#define LINE		20

#define draw_u32(x, y, v, c) draw_u32_hex8(x, y, v, c, SCALE)

static void line(int row, const char *s, u32 c)
{
	draw_string8(8, 40 + row * LINE, s, c, SCALE);
}

/* main returns DTB physical address for linux_boot_jump. */
u32 main(void)
{
	u32 cbar, entry, magic;

	fb_text_fill(0, 0, FB_TEXT_W, FB_TEXT_H, BG);

	line(0, "P105 linux-boot", ACC);
	line(1, "ACE iBEC trampoline", FG);
	line(2, "FB 0x9F6FC000", FG);

	__asm__ volatile("mrc p15, 4, %0, c15, c0, 0" : "=r"(cbar));
	cbar &= 0xFFFFE000u;
	line(3, "CBAR", DIM);
	draw_u32(8 + 5 * 8 * SCALE, 40 + 3 * LINE, cbar, FG);

	line(4, "DTB ", DIM);
	draw_u32(8 + 4 * 8 * SCALE, 40 + 4 * LINE, DTB_PHYS, FG);

	entry = 0x80008000u;
	line(5, "ZIMG", DIM);
	draw_u32(8 + 4 * 8 * SCALE, 40 + 5 * LINE, entry, FG);

	magic = *(volatile u32 *)entry;
	line(6, "HEAD", DIM);
	draw_u32(8 + 4 * 8 * SCALE, 40 + 6 * LINE, magic, FG);

	line(7, "jumping...", ACC);

	return DTB_PHYS;
}
