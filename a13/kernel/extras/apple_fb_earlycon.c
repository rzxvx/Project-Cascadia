// SPDX-License-Identifier: GPL-2.0
/*
 * Apple simple-framebuffer earlycon - iPhone (t8030/N104) Linux bring-up.
 *
 * On this device the dockchannel UART can't be driven from the booted payload
 * and the s5l UART isn't on the serial cable, so the on-screen framebuffer is
 * the only early console we have. This earlycon renders the kernel log as text
 * straight into the framebuffer using the kernel's built-in 8x8 font (drawn 2x),
 * so we can see how far early boot gets before it hangs.
 *
 * The text area sits in the MIDDLE of the panel: the top ~90 px rows of the
 * iPhone 11 panel are hidden by the sensor notch and the rounded corners (the
 * first version drew there and looked "silent").
 *
 * Geometry is hardcoded for N104 (from m1n1's boot_args):
 *   base 0x8fa298000, stride 0xd00 bytes, 828x1792, 30bpp x2r10g10b10.
 *
 * early_ioremap() maps at most 256K per slot, so the text area is mapped as
 * NCHUNK bands of CHUNK_ROWS pixel rows, one slot each (7 slots exist; one is
 * left free for other early users).
 *
 * Use: earlycon=applefb keep_bootcon      (no address needed)
 * Needs CONFIG_FONTS + CONFIG_FONT_8x8 for font_vga_8x8.
 */
#include <linux/console.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/serial_core.h>
#include <linux/font.h>
#include <asm/early_ioremap.h>

#define FB_PHYS		0x8fa298000UL
#define FB_STRIDE	0xd00		/* bytes per scanline */
#define FB_WIDTH	828
#define TOP_ROW		640		/* first pixel row of the text area */
#define CHUNK_ROWS	64		/* 64 * 0xd00 = 208K per early_ioremap slot */
#define NCHUNK		6		/* 384 pixel rows total */
#define SCALE		2
#define CELL_W		(8 * SCALE)
#define CELL_H		(8 * SCALE)
#define WHITE		0x3fffffff	/* x2r10g10b10 all-ones */

static void __iomem *band[NCHUNK];
static const struct font_desc *fbfont;
static int fb_cols, fb_rows, cx, cy;

static void __iomem *fb_row(int y)
{
	return band[y / CHUNK_ROWS] + (size_t)(y % CHUNK_ROWS) * FB_STRIDE;
}

static void fb_clear_line(int row)
{
	int y;

	for (y = row * CELL_H; y < (row + 1) * CELL_H; y++)
		memset_io(fb_row(y), 0, FB_WIDTH * 4);
}

static void fb_newline(void)
{
	cx = 0;
	if (++cy >= fb_rows)
		cy = 0;
	fb_clear_line(cy);
}

static void fb_glyph(unsigned char c)
{
	const u8 *g = (const u8 *)fbfont->data + (size_t)c * 8;
	int px = cx * CELL_W, py = cy * CELL_H, gy, gx, s, t;

	for (gy = 0; gy < 8; gy++) {
		u8 bits = g[gy];

		for (s = 0; s < SCALE; s++) {
			void __iomem *row = fb_row(py + gy * SCALE + s);

			for (gx = 0; gx < 8; gx++)
				for (t = 0; t < SCALE; t++)
					writel_relaxed((bits & (0x80 >> gx)) ? WHITE : 0,
						       row + (px + gx * SCALE + t) * 4);
		}
	}
}

static void fb_putc(struct uart_port *port, unsigned char c)
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
	if (cx >= fb_cols)
		fb_newline();
	fb_glyph(c);
	cx++;
}

static void fb_write(struct console *con, const char *s, unsigned int n)
{
	struct earlycon_device *dev = con->data;

	uart_console_write(&dev->port, s, n, fb_putc);
	wmb();
}

static int __init fb_early_setup(struct earlycon_device *dev, const char *opt)
{
	int i, y;

	fbfont = &font_vga_8x8;
	for (i = 0; i < NCHUNK; i++) {
		band[i] = early_ioremap(FB_PHYS + (size_t)(TOP_ROW + i * CHUNK_ROWS) * FB_STRIDE,
					CHUNK_ROWS * FB_STRIDE);
		if (!band[i])
			return -ENODEV;
	}

	fb_cols = FB_WIDTH / CELL_W;
	fb_rows = NCHUNK * CHUNK_ROWS / CELL_H;
	cx = cy = 0;
	for (y = 0; y < NCHUNK * CHUNK_ROWS; y++)	/* black background */
		memset_io(fb_row(y), 0, FB_WIDTH * 4);

	dev->con->write = fb_write;
	return 0;
}

EARLYCON_DECLARE(applefb, fb_early_setup);
