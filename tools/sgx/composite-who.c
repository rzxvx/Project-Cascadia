/* composite-who.so -- what a process composites with pixman, by kind
 * (docs/research/p105-mesa.md, M45): an LD_PRELOAD pixman_image_composite32
 * that counts calls and pixels by the operator, the source's, mask's and
 * destination's formats and sizes and the caller, and prints the kinds
 * that took most pixels -- in the X server, the software compositing glamor
 * leaves to fb (its caller fbComposite) and why: a format the GPU does not
 * draw (a1), a pixmap kept in memory (glyphs, SHM).
 *
 *   cc -O2 -shared -fPIC composite-who.c -ldl -lpthread -o composite-who.so
 *   LD_PRELOAD=/root/composite-who.so PROGRAM ...   (COMPOSITE_WHO_TOP=N lines)
 *
 * `touch /tmp/composite-who-dump`: the report so far, and the counts from
 * zero again (a second's delay) -- the X server's, around what it is asked
 * to do.  A source or mask with no bits (a solid fill, a gradient) is
 * "solid".  Its LD_PRELOAD is not handed on (xkbcomp, which the X server
 * runs, has no pixman).
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define SLOTS 4096

struct key {
	int op;
	unsigned sf, mf, df;		/* pixman_format_code_t; 0 none, 1 no bits */
	int sw, sh, mw, mh, dw, dh;
	uintptr_t caller;
};

struct slot {
	struct key k;
	int used;
	unsigned long long calls, px;
	int maxw, maxh;			/* the biggest composite of the kind */
};

static struct slot table[SLOTS];
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

/* (looked up, not linked against: the process may have no pixman) */
static unsigned (*get_format)(void *);
static int (*get_width)(void *);
static int (*get_height)(void *);

static void dims(void *img, unsigned *f, int *w, int *h)
{
	*f = img ? get_format(img) : 0;
	*w = img && *f ? get_width(img) : 0;
	*h = img && *f ? get_height(img) : 0;
	if (img && !*f)
		*f = 1;
}

void pixman_image_composite32(int op, void *src, void *mask, void *dst, int32_t sx, int32_t sy,
			      int32_t mx, int32_t my, int32_t dx, int32_t dy, int32_t w, int32_t h)
{
	static void (*real)(int, void *, void *, void *, int32_t, int32_t, int32_t, int32_t,
			    int32_t, int32_t, int32_t, int32_t);
	struct key k;
	unsigned i;

	if (!real) {
		real = dlsym(RTLD_NEXT, "pixman_image_composite32");
		get_format = dlsym(RTLD_DEFAULT, "pixman_image_get_format");
		get_width = dlsym(RTLD_DEFAULT, "pixman_image_get_width");
		get_height = dlsym(RTLD_DEFAULT, "pixman_image_get_height");
	}
	memset(&k, 0, sizeof(k));
	k.op = op;
	k.caller = (uintptr_t)__builtin_return_address(0);
	dims(src, &k.sf, &k.sw, &k.sh);
	dims(mask, &k.mf, &k.mw, &k.mh);
	dims(dst, &k.df, &k.dw, &k.dh);
	pthread_mutex_lock(&lock);
	i = (unsigned)(k.op * 31 + k.sf * 7 + k.mf * 13 + k.df + k.sw * 3 + k.sh * 5 + k.dw * 11 +
		       k.dh * 17 + k.mw * 19) * 2654435761u % SLOTS;
	for (unsigned n = 0; n < SLOTS; n++, i = (i + 1) % SLOTS)
		if (!table[i].used || !memcmp(&table[i].k, &k, sizeof(k))) {
			table[i].used = 1;
			table[i].k = k;
			table[i].calls++;
			table[i].px += (unsigned long long)w * h;
			if ((long long)w * h > (long long)table[i].maxw * table[i].maxh)
				table[i].maxw = w, table[i].maxh = h;
			break;
		}
	pthread_mutex_unlock(&lock);
	real(op, src, mask, dst, sx, sy, mx, my, dx, dy, w, h);
}

static const char *format_name(unsigned f)
{
	static char buf[8][16];
	static int n;
	char *b = buf[n++ % 8];

	switch (f) {
	case 0: return "-";
	case 1: return "solid";
	case 0x20028888: return "a8r8g8b8";
	case 0x20020888: return "x8r8g8b8";
	case 0x08018000: return "a8";
	case 0x01011000: return "a1";
	case 0x10020565: return "r5g6b5";
	}
	snprintf(b, 16, "%08x", f);
	return b;
}

static int by_px(const void *a, const void *b)
{
	const struct slot *x = a, *y = b;

	return x->px < y->px ? 1 : x->px > y->px ? -1 : 0;
}

static void report(void)
{
	static struct slot s[SLOTS];
	unsigned long long calls = 0, px = 0;
	const char *e = getenv("COMPOSITE_WHO_TOP");
	int n = 0, top = e ? atoi(e) : 25;

	pthread_mutex_lock(&lock);
	for (int i = 0; i < SLOTS; i++)
		if (table[i].used)
			s[n++] = table[i];
	memset(table, 0, sizeof(table));
	pthread_mutex_unlock(&lock);
	qsort(s, n, sizeof(*s), by_px);
	for (int i = 0; i < n; i++)
		calls += s[i].calls, px += s[i].px;
	fprintf(stderr, "composite-who: %d kinds, %llu calls, %llu pixels\n", n, calls, px);
	for (int i = 0; i < n && i < top; i++) {
		Dl_info d;
		const char *sym = "?";

		if (dladdr((void *)s[i].k.caller, &d) && d.dli_sname)
			sym = d.dli_sname;
		fprintf(stderr, "composite-who: %6llu calls %9llu px (max %dx%d) op %2d  src %s %dx%d  "
			"mask %s %dx%d  dst %s %dx%d  %s\n", s[i].calls, s[i].px, s[i].maxw,
			s[i].maxh, s[i].k.op, format_name(s[i].k.sf), s[i].k.sw, s[i].k.sh,
			format_name(s[i].k.mf), s[i].k.mw, s[i].k.mh, format_name(s[i].k.df),
			s[i].k.dw, s[i].k.dh, sym);
	}
}

/* the report on demand: /tmp/composite-who-dump appearing */
static void *watch(void *arg)
{
	(void)arg;
	for (;;) {
		sleep(1);
		if (access("/tmp/composite-who-dump", F_OK) == 0) {
			report();
			unlink("/tmp/composite-who-dump");
		}
	}
	return NULL;
}

__attribute__((constructor)) static void start(void)
{
	pthread_t th;

	unsetenv("LD_PRELOAD");
	pthread_create(&th, NULL, watch, NULL);
	pthread_detach(th);
}

__attribute__((destructor)) static void stop(void)
{
	report();
}
