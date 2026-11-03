// SPDX-License-Identifier: GPL-2.0
/*
 * Early console for the iPhone 11 (t8030/N104) bring-up: the kernel log on
 * the whole screen and on the debug UART, from parse_early_param() on.
 *
 * Screen: the framebuffer iBoot left (base 0x8fa298000, stride 0xd00,
 * 828x1792, 30bpp x2r10g10b10), text in the 8x16 font between the sensor
 * notch and the rounded bottom corners. Lines wrap around to the top; the
 * newest line is the one above the blank one. Before ioremap exists only the
 * current text line is mapped (one early_ioremap slot, remapped on each new
 * line); from core_initcall the whole framebuffer is. The screen is left to
 * simpledrm/fbcon the moment a driver binds to the simple-framebuffer device
 * (or at device_initcall_sync, if none does), so the two never draw at once
 * and fbcon's boot logo survives.
 *
 * UART: the s5l UART0 (0x235200000) that m1n1 also prints to. Apple wants
 * nGnRnE mappings and 32-bit accesses for it, as in samsung_tty's s5l
 * earlycon. The TX wait is bounded: a UART that never drains is dropped,
 * not waited on.
 *
 * Use: earlycon=applefb keep_bootcon      options: ,nofb ,nos5l
 * Needs CONFIG_FONTS + CONFIG_FONT_8x16.
 */
#include <linux/console.h>
#include <linux/font.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/serial_core.h>
#include <linux/string.h>
#include <asm/early_ioremap.h>
#include <asm/fixmap.h>

#define FB_PHYS		0x8fa298000UL
#define FB_STRIDE	0xd00		/* bytes per scanline */
#define FB_WIDTH	828
#define FB_HEIGHT	1792
#define TOP_ROW		96		/* below the notch */
#define BOTTOM_ROW	1744		/* above the rounded bottom corners */
#define FONT_W		8
#define FONT_H		16
#define LINE_BYTES	(FONT_H * FB_STRIDE)
#define WHITE		0x3fffffff	/* x2r10g10b10 all-ones */

#define S5L_PHYS	0x235200000UL
#define UTRSTAT		0x10
#define UTRSTAT_TXBE	BIT(1)
#define UTXH		0x20
#define S5L_SPINS	100000

static const struct font_desc *font;
static void __iomem *line_map;	/* early: the band of text line line_mapped */
static int line_mapped = -1;
static void __iomem *full_map;	/* from core_initcall: the whole framebuffer */
static bool fb_on, s5l_on;
static void __iomem *s5l;
static int cols, rows, cx, cy;

static void map_line(int line)
{
	if (full_map || line == line_mapped)
		return;
	if (line_map)
		early_iounmap(line_map, LINE_BYTES);
	line_map = early_ioremap(FB_PHYS + (size_t)(TOP_ROW + line * FONT_H) * FB_STRIDE,
				 LINE_BYTES);
	line_mapped = line_map ? line : -1;
}

/* pixel row y (0..FONT_H-1) of the current text line, or NULL */
static void __iomem *line_row(int y)
{
	if (full_map)
		return full_map + (size_t)(TOP_ROW + cy * FONT_H + y) * FB_STRIDE;
	return line_map ? line_map + (size_t)y * FB_STRIDE : NULL;
}

static void fb_clear_line(void)
{
	int y;

	for (y = 0; y < FONT_H; y++) {
		void __iomem *row = line_row(y);

		if (row)
			memset_io(row, 0, FB_WIDTH * 4);
	}
}

static void fb_newline(void)
{
	cx = 0;
	cy = (cy + 1) % rows;
	map_line(cy);
	fb_clear_line();
}

static void fb_glyph(unsigned char c)
{
	const u8 *g = (const u8 *)font->data + (size_t)c * FONT_H;
	int y, x;

	for (y = 0; y < FONT_H; y++) {
		void __iomem *row = line_row(y);

		if (!row)
			return;
		row += cx * FONT_W * 4;
		for (x = 0; x < FONT_W; x++)
			writel_relaxed((g[y] & (0x80 >> x)) ? WHITE : 0, row + x * 4);
	}
}

static void fb_putc(unsigned char c)
{
	if (c == '\n') {
		fb_newline();
		return;
	}
	if (c == '\r') {
		cx = 0;
		return;
	}
	if (c < 0x20 || c > 0x7e)
		c = '?';
	if (cx >= cols)
		fb_newline();
	fb_glyph(c);
	cx++;
}

static void s5l_putc(unsigned char c)
{
	int i;

	for (i = 0; i < S5L_SPINS; i++)
		if (readl(s5l + UTRSTAT) & UTRSTAT_TXBE)
			break;
	if (i == S5L_SPINS) {
		s5l_on = false;		/* never drains: stop trying */
		return;
	}
	writel(c, s5l + UTXH);
}

static void applefb_putc(struct uart_port *port, unsigned char c)
{
	if (fb_on)
		fb_putc(c);
	if (s5l_on)
		s5l_putc(c);
}

static void applefb_write(struct console *con, const char *s, unsigned int n)
{
	struct earlycon_device *dev = con->data;

	uart_console_write(&dev->port, s, n, applefb_putc);
	wmb();
}

static int __init applefb_setup(struct earlycon_device *dev, const char *opt)
{
	fb_on = !(opt && strstr(opt, "nofb"));
	s5l_on = !(opt && strstr(opt, "nos5l"));

	if (s5l_on) {
		__set_fixmap(FIX_EARLYCON_MEM_BASE, S5L_PHYS & PAGE_MASK,
			     __pgprot(PROT_DEVICE_nGnRnE));
		s5l = (void __iomem *)__fix_to_virt(FIX_EARLYCON_MEM_BASE) +
		      (S5L_PHYS & ~PAGE_MASK);
	}

	if (fb_on) {
		font = &font_vga_8x16;
		cols = FB_WIDTH / FONT_W;
		rows = (BOTTOM_ROW - TOP_ROW) / FONT_H;
		cx = cy = 0;
		map_line(0);
		fb_clear_line();
	}

	dev->con->write = applefb_write;
	return 0;
}
EARLYCON_DECLARE(applefb, applefb_setup);

static void applefb_stop_screen(void)
{
	console_lock();
	fb_on = false;
	console_unlock();
}

/* simpledrm is about to probe the framebuffer: it and fbcon own the screen now */
static int applefb_bus_notify(struct notifier_block *nb, unsigned long action, void *data)
{
	struct device *dev = data;

	if (action == BUS_NOTIFY_BIND_DRIVER && fb_on && dev->of_node &&
	    of_device_is_compatible(dev->of_node, "simple-framebuffer"))
		applefb_stop_screen();
	return NOTIFY_DONE;
}

static struct notifier_block applefb_nb = {
	.notifier_call = applefb_bus_notify,
};

/* ioremap works now: map the whole framebuffer, drop the per-line window */
static int __init applefb_map_full(void)
{
	void __iomem *m;

	if (!fb_on)
		return 0;
	bus_register_notifier(&platform_bus_type, &applefb_nb);
	m = ioremap_wc(FB_PHYS, (size_t)FB_STRIDE * FB_HEIGHT);
	if (!m)
		return 0;
	console_lock();
	full_map = m;
	if (line_map) {
		early_iounmap(line_map, LINE_BYTES);
		line_map = NULL;
		line_mapped = -1;
	}
	console_unlock();
	return 0;
}
core_initcall(applefb_map_full);

/* no driver took the framebuffer: hand the screen over anyway; the UART keeps going */
static int __init applefb_screen_handover(void)
{
	if (fb_on)
		applefb_stop_screen();
	return 0;
}
device_initcall_sync(applefb_screen_handover);
