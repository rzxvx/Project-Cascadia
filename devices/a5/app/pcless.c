/*
 * The PC-less boot file on the iPad itself: what scripts/assemble.py and
 * scripts/make-pcless.py do on the computer, so the app can put a new kernel
 * from a release into /kernelcache without one.
 *
 * /kernelcache is a plaintext img3 (krnl) holding "complzss" (an LZSS stream
 * of literals only) of a Mach-O whose one segment is the boot bundle: the
 * loader padded to 0x8000, zImage + DTB, padding to 4K, and this machine's
 * layer (a newc cpio: its firmware from the IPSW, its keys) with an "OVL1"
 * header at 0x7F00.  An update keeps the layer and replaces loader and kernel.
 *
 * Plain C, no platform calls: tests/test-pcless.c runs it on the computer
 * against the Python tools' output, byte for byte.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "pcless.h"

#define IMG3_HEADER	20
#define LOAD		0x80000000u
#define KERNEL_OFF	0x8000u
#define HDR_OFF		0x7F00u
#define OVL_MAGIC	0x314C564Fu	/* "OVL1" */
#define OVERLAY_PHYS	0x88000000u
#define OVERLAY_MAX	0x08000000u
#define SEG_FILEOFF	0x1000u
#define COMP_DATA_OFF	0x180u
#define LOAD_MAX	0x3F00000u
#define HEAP_ROOM	(40u << 20)
#define MACHO_END_LIMIT	0x84000000u

static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static void put_le32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
static void put_be32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }

static uint32_t adler32(const uint8_t *d, size_t n)
{
	uint32_t a = 1, b = 0;

	while (n) {
		size_t k = n < 5552 ? n : 5552;

		n -= k;
		while (k--) {
			a += *d++;
			b += a;
		}
		a %= 65521;
		b %= 65521;
	}

	return b << 16 | a;
}

/* --------------------------------------------------------------- img3 --- */

/* ATAD's data (its real length) in a plaintext img3; NULL if there is none */
static const uint8_t *img3_data(const uint8_t *img, size_t n, size_t *len)
{
	size_t off = IMG3_HEADER;

	if (n < IMG3_HEADER || memcmp(img, "3gmI", 4))
		return NULL;
	while (off + 12 <= n) {
		uint32_t total = le32(img + off + 4), dlen = le32(img + off + 8);

		if (total < 12 || off + total > n)
			return NULL;
		if (!memcmp(img + off, "GABK", 4))
			return NULL;	/* encrypted */
		if (!memcmp(img + off, "ATAD", 4)) {
			if (12 + dlen > total)
				return NULL;
			*len = dlen;
			return img + off + 12;
		}
		off += total;
	}

	return NULL;
}

/* scripts/img3pack.py pack_plain: the template's tags, ATAD replaced, no KBAG */
static uint8_t *img3_pack(const uint8_t *tpl, size_t tn, const uint8_t *plain, size_t pn, size_t *outn)
{
	size_t off = IMG3_HEADER, n = IMG3_HEADER, pad = (16 - pn % 16) % 16, w;
	uint8_t *out;

	if (tn < IMG3_HEADER || memcmp(tpl, "3gmI", 4))
		return NULL;
	while (off + 12 <= tn) {	/* size first */
		uint32_t total = le32(tpl + off + 4);

		if (total < 12 || off + total > tn)
			return NULL;
		if (!memcmp(tpl + off, "ATAD", 4))
			n += 12 + pn + pad;
		else if (memcmp(tpl + off, "GABK", 4))
			n += total;
		off += total;
	}
	out = calloc(1, n);
	if (!out)
		return NULL;
	for (off = IMG3_HEADER, w = IMG3_HEADER; off + 12 <= tn; off += le32(tpl + off + 4)) {
		uint32_t total = le32(tpl + off + 4);

		if (!memcmp(tpl + off, "GABK", 4))
			continue;
		if (!memcmp(tpl + off, "ATAD", 4)) {
			memcpy(out + w, "ATAD", 4);
			put_le32(out + w + 4, 12 + pn + pad);
			put_le32(out + w + 8, pn);
			memcpy(out + w + 12, plain, pn);
			w += 12 + pn + pad;
		} else {
			memcpy(out + w, tpl + off, total);
			w += total;
		}
	}
	memcpy(out, "3gmI", 4);
	put_le32(out + 4, n);
	put_le32(out + 8, n - IMG3_HEADER);
	put_le32(out + 12, 0);
	memcpy(out + 16, tpl + 16, 4);
	*outn = n;

	return out;
}

/* ---------------------------------------------------- complzss / Mach-O --- */

int pcless_unwrap(const uint8_t *img, size_t n, uint8_t **bundle, size_t *blen)
{
	size_t clen, plen, body, r, w;
	const uint8_t *c = img3_data(img, n, &clen), *in;
	uint8_t *plain;
	uint32_t filesize;

	if (!c)
		return PCLESS_E_IMG3;
	if (memcmp(img + 16, "lnrk", 4))	/* ident "krnl", reversed */
		return PCLESS_E_IMG3;
	if (clen < COMP_DATA_OFF || memcmp(c, "comp", 4) || memcmp(c + 4, "lzss", 4))
		return PCLESS_E_COMP;
	plen = be32(c + 12);
	body = be32(c + 16);
	if (COMP_DATA_OFF + body > clen || plen > LOAD_MAX)
		return PCLESS_E_COMP;
	plain = malloc(plen ? plen : 1);
	if (!plain)
		return PCLESS_E_MEM;
	/* only the literal runs this tool writes: 0xff, then up to 8 bytes */
	for (in = c + COMP_DATA_OFF, r = 0, w = 0; r < body && w < plen; ) {
		size_t k = plen - w < 8 ? plen - w : 8;

		if (in[r] != 0xFF || r + 1 + k > body) {
			free(plain);
			return PCLESS_E_COMP;
		}
		memcpy(plain + w, in + r + 1, k);
		r += 1 + k;
		w += k;
	}
	if (w != plen || adler32(plain, plen) != be32(c + 8)) {
		free(plain);
		return PCLESS_E_COMP;
	}
	/* the Mach-O: header, LC_SEGMENT at 28, the bundle at its fileoff */
	if (plen < SEG_FILEOFF || le32(plain) != 0xFEEDFACE || le32(plain + 28) != 1 ||
	    le32(plain + 52) != LOAD || le32(plain + 60) != SEG_FILEOFF) {
		free(plain);
		return PCLESS_E_MACHO;
	}
	filesize = le32(plain + 64);
	if (SEG_FILEOFF + (size_t)filesize > plen) {
		free(plain);
		return PCLESS_E_MACHO;
	}
	memmove(plain, plain + SEG_FILEOFF, filesize);
	*bundle = plain;
	*blen = filesize;

	return 0;
}

static uint8_t *macho(const uint8_t *bundle, size_t blen, size_t *outn)
{
	size_t n = SEG_FILEOFF + blen;
	uint8_t *m = calloc(1, n), *p;
	uint32_t vmsize = (uint32_t)((blen + 0xFFF) & ~(size_t)0xFFF);

	if (!m)
		return NULL;
	put_le32(m, 0xFEEDFACE);
	put_le32(m + 4, 12);		/* CPU_TYPE_ARM */
	put_le32(m + 8, 9);		/* ARMv7 */
	put_le32(m + 12, 2);		/* MH_EXECUTE */
	put_le32(m + 16, 2);		/* ncmds */
	put_le32(m + 20, 56 + 84);
	put_le32(m + 24, 1);		/* MH_NOUNDEFS */
	p = m + 28;
	put_le32(p, 1);			/* LC_SEGMENT */
	put_le32(p + 4, 56);
	memcpy(p + 8, "__TEXT", 6);
	put_le32(p + 24, LOAD);
	put_le32(p + 28, vmsize);
	put_le32(p + 32, SEG_FILEOFF);
	put_le32(p + 36, (uint32_t)blen);
	put_le32(p + 40, 7);
	put_le32(p + 44, 5);
	p += 56;
	put_le32(p, 5);			/* LC_UNIXTHREAD */
	put_le32(p + 4, 84);
	put_le32(p + 8, 1);		/* ARM_THREAD_STATE */
	put_le32(p + 12, 17);
	put_le32(p + 16 + 15 * 4, LOAD);	/* pc */
	put_le32(p + 16 + 16 * 4, 0x1D3);	/* cpsr: SVC, IRQ/FIQ masked */
	memcpy(m + SEG_FILEOFF, bundle, blen);
	*outn = n;

	return m;
}

static uint8_t *complzss(const uint8_t *plain, size_t pn, size_t *outn)
{
	size_t body = pn + (pn + 7) / 8, r, w = COMP_DATA_OFF;
	uint8_t *c = calloc(1, COMP_DATA_OFF + body);

	if (!c)
		return NULL;
	memcpy(c, "complzss", 8);
	put_be32(c + 8, adler32(plain, pn));
	put_be32(c + 12, (uint32_t)pn);
	put_be32(c + 16, (uint32_t)body);
	for (r = 0; r < pn; r += 8) {
		size_t k = pn - r < 8 ? pn - r : 8;

		c[w] = 0xFF;
		memcpy(c + w + 1, plain + r, k);
		w += 1 + k;
	}
	*outn = w;

	return c;
}

int pcless_wrap(const uint8_t *tpl, size_t tn, const uint8_t *bundle, size_t blen,
		uint8_t **img, size_t *imgn)
{
	size_t mn, cn;
	uint8_t *m, *c;

	if (LOAD + ((blen + 0xFFF) & ~(size_t)0xFFF) > MACHO_END_LIMIT)
		return PCLESS_E_LIMIT;
	m = macho(bundle, blen, &mn);
	if (!m)
		return PCLESS_E_MEM;
	c = complzss(m, mn, &cn);
	free(m);
	if (!c)
		return PCLESS_E_MEM;
	if (mn > LOAD_MAX || cn > LOAD_MAX || cn - COMP_DATA_OFF > HEAP_ROOM) {
		free(c);
		return PCLESS_E_LIMIT;
	}
	*img = img3_pack(tpl, tn, c, cn, imgn);
	free(c);

	return *img ? 0 : PCLESS_E_IMG3;
}

/* ------------------------------------------------------------- bundle --- */

int pcless_overlay(const uint8_t *bundle, size_t blen, size_t *off, size_t *len)
{
	uint32_t o, l;

	if (blen < KERNEL_OFF || le32(bundle + HDR_OFF) != OVL_MAGIC ||
	    le32(bundle + HDR_OFF + 12) != OVERLAY_PHYS)
		return PCLESS_E_LAYER;
	o = le32(bundle + HDR_OFF + 4);
	l = le32(bundle + HDR_OFF + 8);
	if (o < KERNEL_OFF || (size_t)o + l > blen || l > OVERLAY_MAX)
		return PCLESS_E_LAYER;
	*off = o;
	*len = l;

	return 0;
}

/* linux,initrd-start/-end in /chosen, one cell each, in place */
static int patch_initrd(uint8_t *dtb, size_t n, uint32_t start, uint32_t end)
{
	uint32_t total, ost, ostr;
	size_t p;
	int depth = 0, chosen = 0, found = 0;

	if (n < 40 || be32(dtb) != 0xD00DFEED)
		return -1;
	total = be32(dtb + 4);
	ost = be32(dtb + 8);
	ostr = be32(dtb + 12);
	if (total > n || ost >= total || ostr >= total)
		return -1;
	for (p = ost; p + 4 <= total; ) {
		uint32_t tok = be32(dtb + p);

		p += 4;
		if (tok == 1) {		/* BEGIN_NODE */
			const char *name = (const char *)dtb + p;
			size_t l = strnlen(name, total - p);

			depth++;
			if (depth == 2)
				chosen = !strcmp(name, "chosen");
			p = (p + l + 1 + 3) & ~(size_t)3;
		} else if (tok == 2) {	/* END_NODE */
			if (depth == 2)
				chosen = 0;
			depth--;
		} else if (tok == 3) {	/* PROP */
			uint32_t len, nameoff;
			const char *name;

			if (p + 8 > total)
				return -1;
			len = be32(dtb + p);
			nameoff = be32(dtb + p + 4);
			p += 8;
			if (ostr + nameoff >= total || p + len > total)
				return -1;
			name = (const char *)dtb + ostr + nameoff;
			if (chosen && depth == 2 && len == 4) {
				if (!strcmp(name, "linux,initrd-start")) {
					put_be32(dtb + p, start);
					found |= 1;
				} else if (!strcmp(name, "linux,initrd-end")) {
					put_be32(dtb + p, end);
					found |= 2;
				}
			}
			p = (p + len + 3) & ~(size_t)3;
		} else if (tok == 4) {	/* NOP */
		} else if (tok == 9) {	/* END */
			break;
		} else {
			return -1;
		}
	}

	return found == 3 ? 0 : -1;
}

int pcless_assemble(const uint8_t *loader, size_t ln, const uint8_t *zimage, size_t zn,
		    const uint8_t *ovl, size_t on, uint8_t **out, size_t *outn)
{
	size_t dtb_off = 0, i, kend, n, ovl_off;
	uint8_t *b;
	int have = 0;

	if (ln > HDR_OFF || on > OVERLAY_MAX || zn < 8)
		return PCLESS_E_INPUT;
	for (i = zn - 4; i-- > 0; )
		if (!memcmp(zimage + i, "\xd0\x0d\xfe\xed", 4)) {
			dtb_off = i;
			have = 1;
			break;
		}
	if (!have || dtb_off + be32(zimage + dtb_off + 4) != zn)
		return PCLESS_E_INPUT;
	kend = KERNEL_OFF + zn;
	ovl_off = (kend + 4095) & ~(size_t)4095;
	n = ovl_off + on;
	if (LOAD + n > OVERLAY_PHYS)
		return PCLESS_E_LIMIT;
	b = calloc(1, n);
	if (!b)
		return PCLESS_E_MEM;
	memcpy(b, loader, ln);
	memcpy(b + KERNEL_OFF, zimage, zn);
	if (patch_initrd(b + KERNEL_OFF + dtb_off, zn - dtb_off,
			 OVERLAY_PHYS, OVERLAY_PHYS + (uint32_t)on)) {
		free(b);
		return PCLESS_E_INPUT;
	}
	if (on) {
		memcpy(b + ovl_off, ovl, on);
		put_le32(b + HDR_OFF, OVL_MAGIC);
		put_le32(b + HDR_OFF + 4, (uint32_t)ovl_off);
		put_le32(b + HDR_OFF + 8, (uint32_t)on);
		put_le32(b + HDR_OFF + 12, OVERLAY_PHYS);
	}
	*out = b;
	*outn = n;

	return 0;
}

const char *pcless_error(int e)
{
	switch (e) {
	case PCLESS_E_IMG3:	return "not a plaintext kernelcache img3";
	case PCLESS_E_COMP:	return "not the literal complzss this tool writes";
	case PCLESS_E_MACHO:	return "not the one-segment Mach-O this tool writes";
	case PCLESS_E_LAYER:	return "no machine layer (OVL1) in the boot bundle";
	case PCLESS_E_INPUT:	return "the new loader or kernel is not usable";
	case PCLESS_E_LIMIT:	return "too big for iBEC";
	case PCLESS_E_MEM:	return "out of memory";
	}
	return "error";
}
