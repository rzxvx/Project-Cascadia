/*
 * app/pcless.c on the computer, against the Python tools:
 *
 *   cc -O2 -Iapp -o build/test-pcless tests/test-pcless.c app/pcless.c
 *   build/test-pcless KERNELCACHE BUNDLE LOADER ZIMAGE_DTB
 *
 * KERNELCACHE from scripts/make-pcless.py of BUNDLE, BUNDLE from
 * scripts/assemble.py of LOADER + ZIMAGE_DTB + the layer.  Unwrapping must give
 * BUNDLE back, reassembling its layer with LOADER + ZIMAGE_DTB must give BUNDLE,
 * and wrapping BUNDLE with KERNELCACHE as the template must give KERNELCACHE:
 * every step byte for byte.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pcless.h"

static uint8_t *slurp(const char *p, size_t *n)
{
	FILE *f = fopen(p, "rb");
	uint8_t *b;
	long l;

	if (!f || fseek(f, 0, SEEK_END) || (l = ftell(f)) < 0 || fseek(f, 0, SEEK_SET)) {
		perror(p);
		exit(2);
	}
	b = malloc((size_t)l + 1);
	if (!b || fread(b, 1, (size_t)l, f) != (size_t)l) {
		perror(p);
		exit(2);
	}
	fclose(f);
	*n = (size_t)l;
	return b;
}

static int same(const char *what, const uint8_t *a, size_t an, const uint8_t *b, size_t bn)
{
	size_t i;

	if (an == bn && !memcmp(a, b, an)) {
		printf("ok   %s (%zu bytes)\n", what, an);
		return 1;
	}
	for (i = 0; i < an && i < bn && a[i] == b[i]; i++)
		;
	printf("FAIL %s: %zu vs %zu bytes, first difference at %#zx\n", what, an, bn, i);
	return 0;
}

int main(int argc, char **argv)
{
	size_t kn, bn, ln, zn, un, on, oo, an, wn;
	uint8_t *k, *b, *l, *z, *u, *a, *w;
	int e, ok = 1;

	if (argc != 5) {
		fprintf(stderr, "usage: %s KERNELCACHE BUNDLE LOADER ZIMAGE_DTB\n", argv[0]);
		return 2;
	}
	k = slurp(argv[1], &kn);
	b = slurp(argv[2], &bn);
	l = slurp(argv[3], &ln);
	z = slurp(argv[4], &zn);

	if ((e = pcless_unwrap(k, kn, &u, &un))) {
		printf("FAIL unwrap: %s\n", pcless_error(e));
		return 1;
	}
	ok &= same("unwrap(kernelcache) == bundle", u, un, b, bn);
	if ((e = pcless_overlay(u, un, &oo, &on))) {
		printf("FAIL overlay: %s\n", pcless_error(e));
		return 1;
	}
	printf("     layer at %#zx, %zu bytes\n", oo, on);
	if ((e = pcless_assemble(l, ln, z, zn, u + oo, on, &a, &an))) {
		printf("FAIL assemble: %s\n", pcless_error(e));
		return 1;
	}
	ok &= same("assemble(loader, zImage-dtb, layer) == bundle", a, an, b, bn);
	if ((e = pcless_wrap(k, kn, a, an, &w, &wn))) {
		printf("FAIL wrap: %s\n", pcless_error(e));
		return 1;
	}
	ok &= same("wrap(bundle) == kernelcache", w, wn, k, kn);
	puts(ok ? "PASS" : "FAIL");
	return !ok;
}
