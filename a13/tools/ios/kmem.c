// SPDX-License-Identifier: MIT
/*
 * kmem - kernel and physical memory on the iPhone's jailbroken iOS 16.4, for
 * watching the GPU driver (a13/docs/research/gpu.md, step G1).  Root only.
 * The primitives come from Dopamine's libjailbreak (dlopen'ed, no headers).
 *
 *   kmem info                     kernel slide and base, DRAM range
 *   kmem kr VA [LEN]              hexdump kernel virtual memory (LEN 0x100)
 *   kmem pr PA [LEN]              hexdump physical memory (DRAM only)
 *   kmem kd VA LEN FILE           kernel virtual memory to a file
 *   kmem pd PA LEN FILE           physical memory to a file (DRAM only)
 *   kmem k2p VA | p2k PA          translate
 *   kmem kw VA 32|64 VALUE        write kernel virtual, then read back
 *   kmem pw PA 32|64 VALUE        write physical (DRAM only), then read back
 *
 * Physical access goes through a normal cacheable mapping: device registers
 * (MMIO) read that way can stop the SoC, so anything outside the DRAM that
 * /chosen describes is refused here.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <dlfcn.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define LIBJB "/var/jb/basebin/libjailbreak.dylib"
#define CHUNK 0x4000
#define WIN 0x10000

/*
 * Dopamine's struct system_info starts with the kernel slide and the static
 * kernel base; the fields after them moved between releases (3.0.12 differs
 * from the 2.x header), so only these two are used.
 */
struct kconst {
	uint64_t slide, staticBase;
};

static int (*jb_init)(void);
static int (*jb_kreadbuf)(uint64_t, void *, size_t);
static int (*jb_kwritebuf)(uint64_t, const void *, size_t);
static int (*jb_physreadbuf)(uint64_t, void *, size_t);
static int (*jb_physwritebuf)(uint64_t, const void *, size_t);
static uint64_t (*jb_kvtophys)(uint64_t);
static uint64_t (*jb_phystokv)(uint64_t);
static int (*jb_kmap)(uint64_t, uint64_t, void **);
static struct kconst *kc;
static uint64_t dram_base, dram_size;

static void *sym(void *h, const char *name)
{
	void *p = dlsym(h, name);
	if (!p) {
		fprintf(stderr, "kmem: %s missing in " LIBJB "\n", name);
		exit(1);
	}
	return p;
}

static void jb_open(void)
{
	void *h = dlopen(LIBJB, RTLD_NOW);
	if (!h) {
		fprintf(stderr, "kmem: %s\n", dlerror());
		exit(1);
	}
	jb_init = sym(h, "jbclient_initialize_primitives");
	jb_kreadbuf = sym(h, "kreadbuf");
	jb_kwritebuf = sym(h, "kwritebuf");
	jb_physreadbuf = sym(h, "physreadbuf");
	jb_physwritebuf = sym(h, "physwritebuf");
	jb_kvtophys = sym(h, "kvtophys");
	jb_phystokv = sym(h, "phystokv");
	jb_kmap = sym(h, "kmap");
	kc = sym(h, "gSystemInfo");
	if (getuid() != 0) {
		fprintf(stderr, "kmem: run as root\n");
		exit(1);
	}
	int r = jb_init();
	if (r != 0) {
		fprintf(stderr, "kmem: jbclient_initialize_primitives failed (%d)\n", r);
		exit(1);
	}
}

static uint64_t dt_u64(io_registry_entry_t e, CFStringRef key)
{
	uint64_t v = 0;
	CFTypeRef d = IORegistryEntryCreateCFProperty(e, key, kCFAllocatorDefault, 0);
	if (d && CFGetTypeID(d) == CFDataGetTypeID()) {
		CFIndex n = CFDataGetLength(d);
		CFDataGetBytes(d, CFRangeMake(0, n < 8 ? n : 8), (UInt8 *)&v);
	}
	if (d)
		CFRelease(d);
	return v;
}

static void dram_range(void)
{
	io_registry_entry_t c = IORegistryEntryFromPath(kIOMainPortDefault, "IODeviceTree:/chosen");
	if (c == MACH_PORT_NULL) {
		fprintf(stderr, "kmem: no IODeviceTree:/chosen\n");
		exit(1);
	}
	dram_base = dt_u64(c, CFSTR("dram-base"));
	dram_size = dt_u64(c, CFSTR("dram-size"));
	IOObjectRelease(c);
	if (!dram_size) {
		fprintf(stderr, "kmem: /chosen has no dram-size\n");
		exit(1);
	}
}

static void need_dram(uint64_t pa, uint64_t len)
{
	if (pa < dram_base || len > dram_size || pa - dram_base > dram_size - len) {
		fprintf(stderr, "kmem: 0x%llx+0x%llx is outside DRAM 0x%llx+0x%llx (MMIO is not read this way)\n",
			pa, len, dram_base, dram_size);
		exit(1);
	}
}

static uint64_t num(const char *s)
{
	char *end;
	errno = 0;
	uint64_t v = strtoull(s, &end, 0);
	if (errno || *end || end == s) {
		fprintf(stderr, "kmem: bad number '%s'\n", s);
		exit(2);
	}
	return v;
}

/*
 * physreadbuf only reaches the kernel's physmap (the memory XNU manages);
 * iBoot's carve-outs below it (firmware of the coprocessors, the GPU's among
 * them) are mapped into this process with kmap instead, in windows of
 * WIN bytes (kmap refuses 0x20000 and more).
 * Carve-outs are not all readable: one load from the GPU firmware's __DATA
 * (gfx-asc segment 2, 0x801684000) resets the phone at once, no panic log
 * (2026-11-01, twice).  So carve-outs need KMEM_CARVEOUT=1, and gfx-asc's
 * segments other than __TEXT are always refused.
 */
static uint8_t *win;
static uint64_t win_pa;
static int carveout;

static void deny_gfx_data(uint64_t a, uint64_t len)
{
	io_registry_entry_t e = IORegistryEntryFromPath(kIOMainPortDefault, "IODeviceTree:/arm-io/gfx-asc");
	if (e == MACH_PORT_NULL)
		return;
	CFTypeRef d = IORegistryEntryCreateCFProperty(e, CFSTR("segment-ranges"), kCFAllocatorDefault, 0);
	IOObjectRelease(e);
	if (!d)
		return;
	/* entries of 32 bytes: phys, fw VA, phys again, u32 size, u32 flags */
	const uint8_t *b = CFDataGetBytePtr(d);
	CFIndex n = CFDataGetLength(d) / 32;
	for (CFIndex i = 1; i < n; i++) {
		uint64_t pa;
		uint32_t sz;
		memcpy(&pa, b + i * 32, 8);
		memcpy(&sz, b + i * 32 + 24, 4);
		if (a < pa + sz && pa < a + len) {
			fprintf(stderr, "kmem: 0x%llx+0x%llx touches gfx-asc segment %ld (0x%llx+0x%x): reading it resets the phone\n",
				a, len, (long)i, pa, sz);
			exit(1);
		}
	}
	CFRelease(d);
}

static void need_window(int phys, uint64_t a, uint64_t len)
{
	uint8_t probe;
	if (!phys)
		return;
	deny_gfx_data(a, len);
	if (!jb_physreadbuf(a, &probe, 1))
		return;
	const char *ok = getenv("KMEM_CARVEOUT");
	if (!ok || strcmp(ok, "1")) {
		fprintf(stderr, "kmem: 0x%llx is in an iBoot carve-out (outside the physmap); "
			"some of those reset the phone -- KMEM_CARVEOUT=1 to read anyway\n", a);
		exit(1);
	}
	carveout = 1;
}

static int rd(int phys, uint64_t a, void *buf, size_t len)
{
	if (!phys)
		return jb_kreadbuf(a, buf, len);
	if (!carveout)
		return jb_physreadbuf(a, buf, len);
	while (len) {
		uint64_t pg = a & ~(uint64_t)(WIN - 1);
		size_t n = pg + WIN - a < len ? pg + WIN - a : len;
		if (!win || win_pa != pg) {
			void *u = NULL;
			if (jb_kmap(pg, WIN, &u) || !u) {
				fprintf(stderr, "kmem: kmap of 0x%llx failed\n", pg);
				return -1;
			}
			win = u;
			win_pa = pg;
		}
		memcpy(buf, win + (a - pg), n);
		buf = (uint8_t *)buf + n;
		a += n;
		len -= n;
	}
	return 0;
}

static void hexdump(int phys, uint64_t a, uint64_t len)
{
	uint8_t buf[CHUNK];
	for (uint64_t off = 0; off < len; off += CHUNK) {
		size_t n = len - off < CHUNK ? len - off : CHUNK;
		if (rd(phys, a + off, buf, n)) {
			fprintf(stderr, "kmem: read failed at 0x%llx\n", a + off);
			exit(1);
		}
		for (size_t i = 0; i < n; i += 16) {
			printf("%016llx:", a + off + i);
			for (size_t j = i; j < i + 16; j++)
				j < n ? printf(" %02x", buf[j]) : printf("   ");
			printf("  ");
			for (size_t j = i; j < i + 16 && j < n; j++)
				putchar(buf[j] >= 0x20 && buf[j] < 0x7f ? buf[j] : '.');
			putchar('\n');
		}
	}
}

static void dump(int phys, uint64_t a, uint64_t len, const char *path)
{
	FILE *f = fopen(path, "wb");
	if (!f) {
		perror(path);
		exit(1);
	}
	uint8_t buf[CHUNK];
	for (uint64_t off = 0; off < len; off += CHUNK) {
		size_t n = len - off < CHUNK ? len - off : CHUNK;
		if (rd(phys, a + off, buf, n)) {
			fprintf(stderr, "kmem: read failed at 0x%llx\n", a + off);
			exit(1);
		}
		fwrite(buf, 1, n, f);
	}
	fclose(f);
	printf("0x%llx bytes from %s 0x%llx -> %s\n", len, phys ? "PA" : "VA", a, path);
}

static void wr(int phys, uint64_t a, int width, uint64_t v)
{
	uint64_t back = 0;
	size_t n = width == 64 ? 8 : 4;
	uint32_t v32 = (uint32_t)v;
	const void *src = width == 64 ? (void *)&v : (void *)&v32;
	int r = phys ? jb_physwritebuf(a, src, n) : jb_kwritebuf(a, src, n);
	if (r || rd(phys, a, &back, n)) {
		fprintf(stderr, "kmem: write failed at 0x%llx\n", a);
		exit(1);
	}
	printf("%s 0x%llx = 0x%llx\n", phys ? "PA" : "VA", a, back);
}

static int usage(void)
{
	fprintf(stderr,
		"usage: kmem info\n"
		"       kmem kr VA [LEN] | pr PA [LEN]\n"
		"       kmem kd VA LEN FILE | pd PA LEN FILE\n"
		"       kmem k2p VA | p2k PA\n"
		"       kmem kw VA 32|64 VALUE | pw PA 32|64 VALUE\n");
	return 2;
}

int main(int argc, char **argv)
{
	if (argc < 2)
		return usage();
	const char *c = argv[1];
	jb_open();
	dram_range();

	if (!strcmp(c, "info")) {
		uint64_t kbase = kc->staticBase + kc->slide, magic = 0;
		jb_kreadbuf(kbase, &magic, 4);
		printf("slide  0x%llx\nkernel 0x%llx (PA 0x%llx, magic 0x%llx)\n",
		       kc->slide, kbase, jb_kvtophys(kbase), magic);
		printf("DRAM   0x%llx +0x%llx\n", dram_base, dram_size);
		return 0;
	}
	if ((!strcmp(c, "kr") || !strcmp(c, "pr")) && (argc == 3 || argc == 4)) {
		int phys = c[0] == 'p';
		uint64_t a = num(argv[2]), len = argc == 4 ? num(argv[3]) : 0x100;
		if (phys)
			need_dram(a, len);
		need_window(phys, a, len);
		hexdump(phys, a, len);
		return 0;
	}
	if ((!strcmp(c, "kd") || !strcmp(c, "pd")) && argc == 5) {
		int phys = c[0] == 'p';
		uint64_t a = num(argv[2]), len = num(argv[3]);
		if (phys)
			need_dram(a, len);
		need_window(phys, a, len);
		dump(phys, a, len, argv[4]);
		return 0;
	}
	if (!strcmp(c, "k2p") && argc == 3) {
		printf("0x%llx\n", jb_kvtophys(num(argv[2])));
		return 0;
	}
	if (!strcmp(c, "p2k") && argc == 3) {
		printf("0x%llx\n", jb_phystokv(num(argv[2])));
		return 0;
	}
	if ((!strcmp(c, "kw") || !strcmp(c, "pw")) && argc == 5) {
		int phys = c[0] == 'p';
		int width = (int)num(argv[3]);
		uint64_t a = num(argv[2]);
		if (width != 32 && width != 64)
			return usage();
		if (phys)
			need_dram(a, width / 8);
		wr(phys, a, width, num(argv[4]));
		return 0;
	}
	return usage();
}
