// SPDX-License-Identifier: GPL-2.0-only
/*
 * earlycon=j1fb: the boot log on iBoot's scanout, from parse_early_param on.
 *
 * iPad3,1 (J1) panel: 2048x1536, x8r8g8b8, stride 8192, at 0xbdb00000 (the
 * address the loader's framebuffer search found). Text is the 8x8 font at
 * scale 2, so 128 columns by 96 rows; it wraps to the top and clears the
 * line ahead instead of scrolling.
 *
 * This runs before the kernel has any mapping for the panel, and while
 * prepare_page_table and devicemaps_init are still clearing kernel page
 * table entries. So each write checks the twelve 1 MiB section entries at
 * J1FB_VIRT and puts back any that are empty, as shared device memory, XN.
 * An entry that has become anything else (vmalloc took the range) is left
 * alone and the text is dropped.
 *
 * Everything here is __init on purpose. printk_late_init unregisters any
 * boot console whose code is in init memory, so this hands the panel back
 * (to simplefb/fbcon and userspace) at late_initcall time.
 *
 * Without a console this slow, the kernel dies early in boot on J1; with
 * it, it boots. The underlying fault is not found yet.
 */
#include <linux/console.h>
#include <linux/init.h>
#include <linux/mm_types.h>
#include <linux/serial_core.h>
#include <asm/domain.h>
#include <asm/pgtable.h>
#include <asm/tlbflush.h>

#include "font8x8.h"

#define J1FB_PHYS	0xbdb00000UL
#define J1FB_SECTIONS	12			/* 12 MiB: 1536 rows of 8192 */
#define J1FB_VIRT	0xfd000000UL		/* high in vmalloc, idle this early */
#define J1FB_W		2048
#define J1FB_H		1536
#define J1FB_STRIDE	8192
#define J1FB_SCALE	2
#define J1FB_CELL	(FONT8_W * J1FB_SCALE)
#define J1FB_COLS	(J1FB_W / J1FB_CELL)
#define J1FB_ROWS	(J1FB_H / J1FB_CELL)
#define J1FB_FG		0x00ffffffu
#define J1FB_BG		0x00000000u

static unsigned int j1fb_col __initdata, j1fb_row __initdata;

/* Shared device, kernel RW, never executable. */
#define J1FB_SECT_PROT	(PMD_TYPE_SECT | PMD_SECT_AP_WRITE | PMD_SECT_BUFFERABLE | \
			 PMD_SECT_S | PMD_SECT_XN | PMD_DOMAIN(DOMAIN_IO))

/* Returns false if any section is owned by something else now. */
static bool __init j1fb_mapped(void)
{
	u32 *hw = (u32 *)init_mm.pgd + (J1FB_VIRT >> SECTION_SHIFT);
	bool changed = false;
	unsigned int i;

	for (i = 0; i < J1FB_SECTIONS; i++) {
		u32 want = (J1FB_PHYS + (i << SECTION_SHIFT)) | J1FB_SECT_PROT;

		if (hw[i] == want)
			continue;
		if (hw[i] != 0)
			return false;
		hw[i] = want;
		flush_pmd_entry((pmd_t *)&hw[i]);
		changed = true;
	}
	if (changed)
		local_flush_tlb_all();
	return true;
}

static void __init j1fb_fill(unsigned int row, u32 color)
{
	u8 __iomem *base = (u8 __iomem *)J1FB_VIRT +
			   row * J1FB_CELL * J1FB_STRIDE;
	unsigned int y, x;

	for (y = 0; y < J1FB_CELL; y++)
		for (x = 0; x < J1FB_W; x++)
			writel_relaxed(color, base + y * J1FB_STRIDE + x * 4);
}

static void __init j1fb_glyph(unsigned int col, unsigned int row, char ch)
{
	u8 __iomem *base = (u8 __iomem *)J1FB_VIRT +
			   row * J1FB_CELL * J1FB_STRIDE + col * J1FB_CELL * 4;
	const unsigned char *g;
	unsigned int y, x;

	if (ch < FONT8_FIRST || ch > FONT8_LAST)
		ch = '?';
	g = &font8x8[(ch - FONT8_FIRST) * FONT8_H];
	for (y = 0; y < J1FB_CELL; y++) {
		u8 bits = g[y / J1FB_SCALE];

		for (x = 0; x < J1FB_CELL; x++) {
			/* font8x8 stores the leftmost pixel in bit 0. */
			u32 c = (bits >> (x / J1FB_SCALE)) & 1 ? J1FB_FG : J1FB_BG;

			writel_relaxed(c, base + y * J1FB_STRIDE + x * 4);
		}
	}
}

static void __init j1fb_newline(void)
{
	j1fb_col = 0;
	j1fb_row = (j1fb_row + 1) % J1FB_ROWS;
	j1fb_fill(j1fb_row, J1FB_BG);
	/* A marker line under the newest text shows where the wrap is. */
	j1fb_fill((j1fb_row + 1) % J1FB_ROWS, 0x00404040u);
}

static void __init j1fb_write(struct console *con, const char *s, unsigned int n)
{
	if (!j1fb_mapped())
		return;
	while (n--) {
		char ch = *s++;

		if (ch == '\n') {
			j1fb_newline();
			continue;
		}
		if (ch == '\r')
			continue;
		if (j1fb_col >= J1FB_COLS)
			j1fb_newline();
		j1fb_glyph(j1fb_col++, j1fb_row, ch);
	}
	wmb();
}

static int __init j1fb_setup(struct earlycon_device *dev, const char *opt)
{
	if (!j1fb_mapped())
		return -EBUSY;
	j1fb_col = 0;
	j1fb_row = 0;
	j1fb_fill(0, J1FB_BG);
	j1fb_fill(1, 0x00404040u);
	dev->con->write = j1fb_write;
	return 0;
}
EARLYCON_DECLARE(j1fb, j1fb_setup);
