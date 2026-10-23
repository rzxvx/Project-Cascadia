/* memcpy-who.so -- who copies how much (docs/research/p105-mesa.md, M42):
 * an LD_PRELOAD memcpy and memmove that count bytes and calls by their
 * caller's address, and at exit print the callers that copied most, named
 * through dladdr (library + symbol, or library + offset for addr2line).
 * perf's call graphs do not work on this userland (no frame pointers, and
 * uprobes break Thumb code), and musl's memcpy is a leaf.
 *
 *   cc -O2 -shared -fPIC -fno-builtin -D_FORTIFY_SOURCE=0 memcpy-who.c -ldl -o memcpy-who.so
 *   LD_PRELOAD=/root/memcpy-who.so PROGRAM ...     (MEMCPY_WHO_TOP=N lines)
 *
 * Only calls through the PLT are seen: a library's own inlined copies and
 * musl's internal ones are not.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SLOTS 4096

struct slot {
	uintptr_t caller;
	unsigned long long bytes, calls;
};

static struct slot table[SLOTS];
static unsigned long long total_bytes, total_calls;

/* the copy itself: musl's memmove handles both (no recursion through the
 * symbols interposed here) */
static void *(*real_memmove)(void *, const void *, size_t);

static void
count(uintptr_t caller, size_t n)
{
	unsigned h = (unsigned)(caller >> 1) * 2654435761u % SLOTS;

	for (unsigned i = 0; i < SLOTS; i++, h = (h + 1) % SLOTS) {
		if (table[h].caller == caller || !table[h].caller) {
			table[h].caller = caller;
			__atomic_fetch_add(&table[h].bytes, n, __ATOMIC_RELAXED);
			__atomic_fetch_add(&table[h].calls, 1, __ATOMIC_RELAXED);
			break;
		}
	}
	__atomic_fetch_add(&total_bytes, n, __ATOMIC_RELAXED);
	__atomic_fetch_add(&total_calls, 1, __ATOMIC_RELAXED);
}

static void *
copy(void *d, const void *s, size_t n, uintptr_t caller)
{
	unsigned char *dd = d;
	const unsigned char *ss = s;

	count(caller, n);
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

static int
by_bytes(const void *a, const void *b)
{
	const struct slot *x = a, *y = b;

	return x->bytes < y->bytes ? 1 : x->bytes > y->bytes ? -1 : 0;
}

__attribute__((constructor)) static void
start(void)
{
	real_memmove = (void *(*)(void *, const void *, size_t))dlsym(RTLD_NEXT, "memmove");
}

__attribute__((destructor)) static void
report(void)
{
	static struct slot sorted[SLOTS];
	const char *e = getenv("MEMCPY_WHO_TOP");
	int top = e ? atoi(e) : 25, n = 0;

	for (int i = 0; i < SLOTS; i++)
		if (table[i].caller)
			sorted[n++] = table[i];
	qsort(sorted, n, sizeof(sorted[0]), by_bytes);
	fprintf(stderr, "memcpy-who: %llu MB in %llu calls\n", total_bytes >> 20, total_calls);
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
		fprintf(stderr, "memcpy-who: %8llu KB %9llu calls %6llu B avg  %s %s+0x%lx\n",
			sorted[i].bytes >> 10, sorted[i].calls,
			sorted[i].calls ? sorted[i].bytes / sorted[i].calls : 0, lib,
			sym ? sym : "", (unsigned long)off);
	}
}
