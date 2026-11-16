/* pcless.c: /kernelcache apart and back together on the iPad. */
#ifndef PCLESS_H
#define PCLESS_H
#include <stddef.h>
#include <stdint.h>

enum {
	PCLESS_E_IMG3 = 1,
	PCLESS_E_COMP,
	PCLESS_E_MACHO,
	PCLESS_E_LAYER,
	PCLESS_E_INPUT,
	PCLESS_E_LIMIT,
	PCLESS_E_MEM,
};

/* img3 (krnl) -> the boot bundle (malloc'd) */
int pcless_unwrap(const uint8_t *img, size_t n, uint8_t **bundle, size_t *blen);
/* where this machine's layer sits in a bundle */
int pcless_overlay(const uint8_t *bundle, size_t blen, size_t *off, size_t *len);
/* loader + zImage-dtb + layer -> a bundle (malloc'd), as scripts/assemble.py */
int pcless_assemble(const uint8_t *loader, size_t ln, const uint8_t *zimage, size_t zn,
		    const uint8_t *ovl, size_t on, uint8_t **out, size_t *outn);
/* a bundle -> img3 (malloc'd), the template's tags kept, as scripts/make-pcless.py */
int pcless_wrap(const uint8_t *tpl, size_t tn, const uint8_t *bundle, size_t blen,
		uint8_t **img, size_t *imgn);
const char *pcless_error(int e);

#endif
