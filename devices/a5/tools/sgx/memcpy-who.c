/* memcpy-who.so -- who copies how much (docs/research/p105-mesa.md, M42):
 * an LD_PRELOAD memcpy and memmove that count bytes and calls by their
 * caller's address, and at exit print the callers that copied most, named
 * through dladdr (library + symbol, or library + offset for addr2line);
 * memset the same, apart (who clears how much).
 * perf's call graphs do not work on this userland (no frame pointers, and
 * uprobes break Thumb code), and musl's memcpy is a leaf.
 *
 *   cc -O2 -shared -fPIC -fno-builtin -U_FORTIFY_SOURCE memcpy-who.c -ldl -lpthread -o memcpy-who.so
 *   LD_PRELOAD=/root/memcpy-who.so PROGRAM ...     (MEMCPY_WHO_TOP=N lines)
 *
 * Only calls through the PLT are seen: a library's own inlined copies and
 * musl's internal ones are not.  `touch /tmp/memcpy-who-dump`: the report so
 * far, and the counts from zero again (a second's delay) -- one stretch of a
 * long-lived process, the X server's.
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

struct slot {
	uintptr_t caller;
	unsigned long long bytes, calls;
};

enum { COPIES, CLEARS };
static struct slot table[2][SLOTS];
static unsigned long long total_bytes[2], total_calls[2];

/* the copy itself: musl's memmove handles both (no recursion through the
 * symbols interposed here); memset's own */
static void *(*real_memmove)(void *, const void *, size_t);
static void *(*real_memset)(void *, int, size_t);

static void
count(int kind, uintptr_t caller, size_t n)
{
	unsigned h = (unsigned)(caller >> 1) * 2654435761u % SLOTS;
	struct slot *t = table[kind];

	for (unsigned i = 0; i < SLOTS; i++, h = (h + 1) % SLOTS) {
		if (t[h].caller == caller || !t[h].caller) {
			t[h].caller = caller;
			__atomic_fetch_add(&t[h].bytes, n, __ATOMIC_RELAXED);
			__atomic_fetch_add(&t[h].calls, 1, __ATOMIC_RELAXED);
			break;
		}
	}
	__atomic_fetch_add(&total_bytes[kind], n, __ATOMIC_RELAXED);
	__atomic_fetch_add(&total_calls[kind], 1, __ATOMIC_RELAXED);
}

static void *
copy(void *d, const void *s, size_t n, uintptr_t caller)
{
	unsigned char *dd = d;
	const unsigned char *ss = s;

	count(COPIES, caller, n);
	if (!real_memmove) {
		/* (before the constructor: a byte loop, never a libc call) */
		if (dd < ss)
			for (size_t i = 0; i < n; i++)
				dd[i] = ss[i];
		else
			for (size_t i = n; i--;)
				dd[i] = ss[i];
		return d;
	}
	return real_memmove(d, s, n);
}

void *
memcpy(void *d, const void *s, size_t n)
{
	return copy(d, s, n, (uintptr_t)__builtin_return_address(0));
}

void *
memmove(void *d, const void *s, size_t n)
{
	return copy(d, s, n, (uintptr_t)__builtin_return_address(0));
}

void *
memset(void *d, int c, size_t n)
{
	count(CLEARS, (uintptr_t)__builtin_return_address(0), n);
	if (!real_memset) {
		for (size_t i = 0; i < n; i++)
			((unsigned char *)d)[i] = c;
		return d;
	}
	return real_memset(d, c, n);
}

static int
by_bytes(const void *a, const void *b)
{
	const struct slot *x = a, *y = b;

	return x->bytes < y->bytes ? 1 : x->bytes > y->bytes ? -1 : 0;
}

static void report(void);

/* the report on demand: /tmp/memcpy-who-dump appearing */
static void *
watch(void *arg)
{
	(void)arg;
	for (;;) {
		sleep(1);
		if (access("/tmp/memcpy-who-dump", F_OK) == 0) {
			report();
			for (int k = 0; k < 2; k++) {
				for (int i = 0; i < SLOTS; i++)
					table[k][i].bytes = table[k][i].calls = 0;
				total_bytes[k] = total_calls[k] = 0;
			}
			unlink("/tmp/memcpy-who-dump");
		}
	}
	return NULL;
}

__attribute__((constructor)) static void
start(void)
{
	pthread_t th;

	real_memmove = (void *(*)(void *, const void *, size_t))dlsym(RTLD_NEXT, "memmove");
	real_memset = (void *(*)(void *, int, size_t))dlsym(RTLD_NEXT, "memset");
	if (!pthread_create(&th, NULL, watch, NULL))
		pthread_detach(th);
}

static void
report_kind(int kind, int top)
{
	static struct slot sorted[SLOTS];
	const char *what = kind == COPIES ? "copies" : "clears";
	int n = 0;

	for (int i = 0; i < SLOTS; i++)
		if (table[kind][i].caller && table[kind][i].calls)
			sorted[n++] = table[kind][i];
	qsort(sorted, n, sizeof(sorted[0]), by_bytes);
	fprintf(stderr, "memcpy-who: %s: %llu MB in %llu calls\n", what, total_bytes[kind] >> 20,
		total_calls[kind]);
	for (int i = 0; i < n && i < top; i++) {
		Dl_info di;
		const char *lib = "?", *sym = NULL;
		uintptr_t off = 0;

		if (dladdr((void *)sorted[i].caller, &di)) {
			lib = di.dli_fname ? strrchr(di.dli_fname, '/') ? strrchr(di.dli_fname, '/') + 1 :
			      di.dli_fname : "?";
			sym = di.dli_sname;
			off = sorted[i].caller - (uintptr_t)(sym ? di.dli_saddr : di.dli_fbase);
		}
		fprintf(stderr, "memcpy-who: %s %8llu KB %9llu calls %6llu B avg  %s %s+0x%lx\n",
			what, sorted[i].bytes >> 10, sorted[i].calls,
			sorted[i].calls ? sorted[i].bytes / sorted[i].calls : 0, lib,
			sym ? sym : "", (unsigned long)off);
	}
}

__attribute__((destructor)) static void
report(void)
{
	const char *e = getenv("MEMCPY_WHO_TOP");
	int top = e ? atoi(e) : 25;

	report_kind(COPIES, top);
	report_kind(CLEARS, top);
}
