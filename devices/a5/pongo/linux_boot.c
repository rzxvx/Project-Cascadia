/*
 * linux_boot — iBEC ACE trampoline @ 0x80000000.
 *
 * No color bars. Status via fb_text8 @ scanout 0x9F6FC000 (768x1024).
 * Bundle layout: [this loader][pad to 0x8000][zImage-dtb @ 0x80008000]
 *                [pad to 4K][overlay cpio]
 *
 * The overlay is the image's local layer (scripts/assemble.py): firmware
 * from the user's IPSW, ssh keys, the NFS root's address -- what cannot be
 * in a prebuilt kernel.  Its header sits in the pad at 0x80007F00; the
 * overlay is copied to where the DTB's linux,initrd-start points (the
 * assembler patched both), out of the way of the kernel's decompression,
 * and the kernel unpacks it over its built-in initramfs.  No header: no
 * overlay, as before.
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

#define BUNDLE_BASE	0x80000000u
#define OVL_MAGIC	0x314C564Fu	/* "OVL1" */

struct overlay_hdr {
	u32 magic;
	u32 offset;		/* in the bundle */
	u32 size;		/* bytes */
	u32 dest;		/* physical; = the DTB's linux,initrd-start */
};


static void line(int row, const char *s, u32 c)
{
	draw_string8(8, 40 + row * LINE, s, c, SCALE);
}

static void copy_overlay(void)
{
	const volatile struct overlay_hdr *h = (const void *)(BUNDLE_BASE + 0x7F00u);
	const u32 *src;
	u32 *dst;
	u32 n;

	if (h->magic != OVL_MAGIC || !h->size)
		return;
	src = (const u32 *)(BUNDLE_BASE + h->offset);
	dst = (u32 *)h->dest;
	for (n = (h->size + 3) / 4; n; n--)
		*dst++ = *src++;
	line(7, "OVL ", DIM);
	draw_u32(8 + 4 * 8 * SCALE, 40 + 7 * LINE, h->size, FG);
	draw_u32(8 + 13 * 8 * SCALE, 40 + 7 * LINE, h->dest, FG);
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

	/* before the jump, which cleans the caches the copy went through */
	copy_overlay();

	line(8, "jumping...", ACC);

	return DTB_PHYS;
}
