// SPDX-License-Identifier: MIT
/*
 * kmem - kernel and physical memory on the iPhone's jailbroken iOS 16.4, for
 * watching the GPU driver (a13/docs/research/gpu.md, G1/G2).  Root only.
 * The primitives come from Dopamine's libjailbreak (dlopen'ed, no headers).
 *
 *   kmem info                     kernel slide and base, DRAM range
 *   kmem kr VA [LEN]              hexdump kernel virtual memory (LEN 0x100)
 *   kmem pr PA [LEN]              hexdump physical memory (DRAM only)
 *   kmem kd VA LEN FILE           kernel virtual memory to a file
 *   kmem pd PA LEN FILE           physical memory to a file (DRAM only)
 *   kmem q VA                     one 64-bit word, as a number (for scripts)
 *   kmem k2p VA | p2k PA          translate
 *   kmem kw VA 32|64 VALUE        write kernel virtual, then read back
 *   kmem pw PA 32|64 VALUE        write physical (DRAM only), then read back
 *   kmem kcr VA [LEN]             hexdump kernel virtual memory through the
 *                                 kernel's own mapping (a kernel call per 8
 *                                 bytes): for what XNU maps outside the
 *                                 physmap, non-cacheable (the GPU's regions);
 *                                 needs Dopamine's kcall, which this phone
 *                                 does not get (no PAC bypass)
 *   kmem obj CLASS                kernel address of each IOService of CLASS
 *   kmem obj name:NAME            ... of each IOService named NAME (an ADT
 *                                 node), and of its children
 *   kmem ptrs VA LEN              for each kernel pointer in VA..VA+LEN: what
 *                                 it points at (its first word: a vtable for
 *                                 C++ objects); physmap reads only
 *   kmem ptscan PA LEN            kernel virtual mappings of a physical range,
 *                                 from XNU's own page tables (only reads those)
 *   kmem mmio PA [LEN]            device registers: the nonzero 32-bit words
 *                                 of PA..PA+LEN (LEN 0x100, at most 0x20000)
 *   kmem mmiowatch SECS PA...     sample 32-bit registers for SECS seconds,
 *                                 print each change with its time
 *
 * Physical access goes through a normal cacheable mapping: device registers
 * (MMIO) read that way can stop the SoC, so anything outside the DRAM that
 * /chosen describes is refused there.  mmio/mmiowatch map the registers
 * into this process uncached instead, the way kmap maps (an IOSurface whose
 * memory descriptor Dopamine points at the physical range), with the cache
 * mode kIOMapInhibitCache = device memory, and read them 32 bits at a time.
 * Writing PTEs into this process directly (pmap_map_in_with_flags) read one
 * PMGR register fine and then took the phone down, at exit most likely: the
 * kernel did not know the mapping it tore down (2026-11-02).
 */
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <dlfcn.h>
#include <mach/mach.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
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
static int (*jb_kcall)(uint64_t *, uint64_t, int, const uint64_t *);
static int (*jb_get_kcall)(void);
static uint64_t (*jb_task_self)(void);
static uint64_t (*jb_port_kobject)(uint64_t, mach_port_t);
static int (*jb_iosurface_map_cache)(uint64_t, uint64_t, void **, uint32_t);
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
	jb_kcall = sym(h, "kcall");
	jb_get_kcall = sym(h, "jbclient_get_fugu14_kcall");
	jb_task_self = sym(h, "task_self");
	jb_port_kobject = sym(h, "task_get_ipc_port_kobject");
	/* kmap is IOSurface_map = this with cache mode 0 */
	jb_iosurface_map_cache = sym(h, "IOSurface_map_withCacheMode");
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
 * them) are mapped into this process with kmap instead (kmap refuses 0x20000
 * and more), only the 16 KiB pages asked for: the mapping is cacheable, and
 * the CPU may fetch anywhere in it ahead of time.
 * Carve-outs are not all readable: one 64-byte read from the GPU firmware's
 * __DATA (0x801684000) reset the phone at once, no panic log (2026-11-01,
 * twice), and so did one from gpu-region (0x8ffff8000, through a 64 KiB window
 * that also covered its neighbours).  So carve-outs need KMEM_CARVEOUT=1, and
 * gfx-asc's segments other than __TEXT and sgx's gpu-region,
 * gfx-shared-region and gfx-handoff are always refused.
 */
static uint8_t *win;
static uint64_t win_pa, win_len;
static int carveout;

static void deny(uint64_t a, uint64_t len, uint64_t pa, uint64_t sz, const char *what)
{
	if (sz && a < pa + sz && pa < a + len) {
		fprintf(stderr, "kmem: 0x%llx+0x%llx touches %s (0x%llx+0x%llx): reading it reset the phone\n",
			a, len, what, pa, sz);
		exit(1);
	}
}

static void deny_gfx(uint64_t a, uint64_t len)
{
	io_registry_entry_t e = IORegistryEntryFromPath(kIOMainPortDefault, "IODeviceTree:/arm-io/gfx-asc");
	if (e != MACH_PORT_NULL) {
		CFTypeRef d = IORegistryEntryCreateCFProperty(e, CFSTR("segment-ranges"), kCFAllocatorDefault, 0);
		IOObjectRelease(e);
		if (d) {
			/* entries of 32 bytes: phys, fw VA, phys again, u32 size, u32 flags */
			const uint8_t *b = CFDataGetBytePtr(d);
			CFIndex n = CFDataGetLength(d) / 32;
			for (CFIndex i = 1; i < n; i++) {
				uint64_t pa;
				uint32_t sz;
				memcpy(&pa, b + i * 32, 8);
				memcpy(&sz, b + i * 32 + 24, 4);
				deny(a, len, pa, sz, "a gfx-asc segment past __TEXT");
			}
			CFRelease(d);
		}
	}
	e = IORegistryEntryFromPath(kIOMainPortDefault, "IODeviceTree:/arm-io/sgx");
	if (e != MACH_PORT_NULL) {
		deny(a, len, dt_u64(e, CFSTR("gpu-region-base")), dt_u64(e, CFSTR("gpu-region-size")), "gpu-region");
		deny(a, len, dt_u64(e, CFSTR("gfx-shared-region-base")), dt_u64(e, CFSTR("gfx-shared-region-size")),
		     "gfx-shared-region");
		deny(a, len, dt_u64(e, CFSTR("gfx-handoff-base")), dt_u64(e, CFSTR("gfx-handoff-size")), "gfx-handoff");
		IOObjectRelease(e);
	}
}

static void need_window(int phys, uint64_t a, uint64_t len)
{
	uint8_t probe;
	if (!phys)
		return;
	deny_gfx(a, len);
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

/*
 * "ldr x0, [x0]; ret" in the 20E247 kernel (static address; checked against
 * the running kernel's bytes before every use).  A kernel call to it loads
 * through XNU's own page tables, with the attributes XNU chose.
 */
#define LOAD_GADGET 0xfffffff007dda86cULL

static uint64_t load_gadget(void)
{
	static uint64_t g;
	uint32_t ins[2];
	if (g)
		return g;
	g = LOAD_GADGET + kc->slide;
	if (jb_kreadbuf(g, ins, sizeof(ins)) || ins[0] != 0xf9400000 || ins[1] != 0xd65f03c0) {
		fprintf(stderr, "kmem: no 'ldr x0,[x0]; ret' at 0x%llx -- not the 20E247 kernel?\n", g);
		exit(1);
	}
	/* on arm64e, kernel calls need a thread jailbreakd signs */
	int r = jb_get_kcall();
	if (r) {
		fprintf(stderr, "kmem: jbclient_get_fugu14_kcall failed (%d)\n", r);
		exit(1);
	}
	return g;
}

static int kcall_read(uint64_t va, void *buf, size_t len)
{
	uint64_t g = load_gadget();
	uint8_t *out = buf;
	if ((va | len) & 7)
		return -1;
	for (size_t i = 0; i < len; i += 8) {
		uint64_t arg = va + i, v = 0;
		if (jb_kcall(&v, g, 1, &arg))
			return -1;
		memcpy(out + i, &v, 8);
	}
	return 0;
}

static int via_kcall;

static int rd(int phys, uint64_t a, void *buf, size_t len)
{
	if (via_kcall)
		return kcall_read(a, buf, len);
	if (!phys)
		return jb_kreadbuf(a, buf, len);
	if (!carveout)
		return jb_physreadbuf(a, buf, len);
	while (len) {
		if (!win || a < win_pa || a >= win_pa + win_len) {
			uint64_t pg = a & ~(uint64_t)(CHUNK - 1);
			uint64_t sz = (a + len - pg + CHUNK - 1) & ~(uint64_t)(CHUNK - 1);
			void *u = NULL;
			if (sz > WIN)
				sz = WIN;
			if (jb_kmap(pg, sz, &u) || !u) {
				fprintf(stderr, "kmem: kmap of 0x%llx+0x%llx failed\n", pg, sz);
				return -1;
			}
			win = u;
			win_pa = pg;
			win_len = sz;
		}
		size_t n = win_pa + win_len - a < len ? win_pa + win_len - a : len;
		memcpy(buf, win + (a - win_pa), n);
		buf = (uint8_t *)buf + n;
		a += n;
		len -= n;
	}
	return 0;
}

/*
 * XNU's kernel page tables: 16 KiB granule, T1SZ 25 (VA 0xffffff8000000000
 * up), L1 = VA[38:36], L2 = VA[35:25] (32 MiB blocks), L3 = VA[24:14].  The
 * root is the one gSystemInfo field that translates the kernel base the way
 * kvtophys does.
 */
#define OA(d) ((d) & 0x0000fffffffc000ULL)
#define KVA(i, j, k) (0xffffff8000000000ULL | (uint64_t)(i) << 36 | (uint64_t)(j) << 25 | (uint64_t)(k) << 14)

static int walk(uint64_t root, uint64_t va, uint64_t *pa)
{
	uint64_t d;
	if (jb_physreadbuf(root + ((va >> 36) & 7) * 8, &d, 8) || (d & 3) != 3)
		return -1;
	if (jb_physreadbuf(OA(d) + ((va >> 25) & 0x7ff) * 8, &d, 8))
		return -1;
	if ((d & 3) == 1) {
		*pa = (d & 0x0000fffffe000000ULL) | (va & 0x1ffffff);
		return 0;
	}
	if ((d & 3) != 3 || jb_physreadbuf(OA(d) + ((va >> 14) & 0x7ff) * 8, &d, 8) || (d & 3) != 3)
		return -1;
	*pa = OA(d) | (va & 0x3fff);
	return 0;
}

static uint64_t kernel_root(void)
{
	uint64_t kbase = kc->staticBase + kc->slide, want = jb_kvtophys(kbase), pa;
	for (int i = 2; i < 40; i++) {
		uint64_t c = ((uint64_t *)kc)[i];
		if (c >= dram_base && c - dram_base < dram_size && !(c & 0x3fff) && !walk(c, kbase, &pa) && pa == want)
			return c;
	}
	fprintf(stderr, "kmem: kernel page table root not found in gSystemInfo\n");
	exit(1);
}

static void show_map(uint64_t va, uint64_t pa, uint64_t sz, uint64_t d, int level)
{
	printf("VA 0x%llx -> PA 0x%llx +0x%llx  L%d desc 0x%016llx  attr %llu AP %llu SH %llu PXN %llu UXN %llu\n",
	       va, pa, sz, level, d, (d >> 2) & 7, (d >> 6) & 3, (d >> 8) & 3, (d >> 53) & 1, (d >> 54) & 1);
}

/*
 * Kernel pointers carry PAC bits on arm64e; bit 55 selects the kernel half,
 * which has VA[63:39] set once stripped.  Heap and image live above
 * 0xffffffd000000000.
 */
#define KSTRIP(p) ((p) | 0xffffff8000000000ULL)
#define ISKPTR(p) (((p) >> 55 & 1) && !((p) & 7) && KSTRIP(p) >= 0xffffffd000000000ULL)

static void objs(const char *cls)
{
	io_iterator_t it;
	io_service_t o;
	int by_name = !strncmp(cls, "name:", 5);
	CFMutableDictionaryRef match = by_name ? IOServiceNameMatching(cls + 5) : IOServiceMatching(cls);
	if (IOServiceGetMatchingServices(kIOMainPortDefault, match, &it)) {
		fprintf(stderr, "kmem: no %s\n", cls);
		exit(1);
	}
	uint64_t task = jb_task_self();
	while ((o = IOIteratorNext(it))) {
		uint64_t k = jb_port_kobject(task, o), vt = 0;
		io_name_t name = "";
		IORegistryEntryGetName(o, name);
		if (k)
			jb_kreadbuf(KSTRIP(k), &vt, 8);
		printf("%s 0x%llx vtable 0x%llx (raw 0x%llx)\n", name, KSTRIP(k), KSTRIP(vt), vt);
		io_iterator_t ci;
		io_registry_entry_t c;
		if (by_name && !IORegistryEntryGetChildIterator(o, kIOServicePlane, &ci)) {
			while ((c = IOIteratorNext(ci))) {
				io_name_t cn = "";
				uint64_t ck = jb_port_kobject(task, c), cvt = 0;
				IORegistryEntryGetName(c, cn);
				if (ck)
					jb_kreadbuf(KSTRIP(ck), &cvt, 8);
				printf("  child %s 0x%llx vtable 0x%llx\n", cn, KSTRIP(ck), KSTRIP(cvt));
				IOObjectRelease(c);
			}
			IOObjectRelease(ci);
		}
		IOObjectRelease(o);
	}
	IOObjectRelease(it);
}

static void ptrs(uint64_t va, uint64_t len)
{
	static uint64_t buf[CHUNK / 8];
	if (len > CHUNK * 64)
		len = CHUNK * 64;
	for (uint64_t off = 0; off < len; off += CHUNK) {
		size_t n = len - off < CHUNK ? len - off : CHUNK;
		if (jb_kreadbuf(va + off, buf, n)) {
			fprintf(stderr, "kmem: read failed at 0x%llx\n", va + off);
			exit(1);
		}
		for (size_t i = 0; i < n / 8; i++) {
			uint64_t q = buf[i], v;
			if (!ISKPTR(q))
				continue;
			uint64_t p = KSTRIP(q);
			if (jb_kreadbuf(p, &v, 8))
				printf("+0x%04llx 0x%016llx -> 0x%llx (unreadable)\n", off + i * 8, q, p);
			else
				printf("+0x%04llx 0x%016llx -> 0x%llx [0x%llx]\n", off + i * 8, q, p, KSTRIP(v));
		}
	}
}

static void ptscan(uint64_t lo, uint64_t len)
{
	static uint64_t t2[2048], t3[2048];
	uint64_t root = kernel_root(), t1[8], hits = 0;
	printf("kernel root 0x%llx\n", root);
	if (jb_physreadbuf(root, t1, sizeof(t1)))
		exit(1);
	for (int i = 0; i < 8; i++) {
		if ((t1[i] & 3) != 3)
			continue;
		if (jb_physreadbuf(OA(t1[i]), t2, sizeof(t2))) {
			printf("L2 table 0x%llx unreadable\n", OA(t1[i]));
			continue;
		}
		for (int j = 0; j < 2048; j++) {
			uint64_t d = t2[j];
			if ((d & 3) == 1) {
				uint64_t pa = d & 0x0000fffffe000000ULL;
				if (pa < lo + len && lo < pa + 0x2000000) {
					show_map(KVA(i, j, 0), pa, 0x2000000, d, 2);
					hits++;
				}
				continue;
			}
			if ((d & 3) != 3)
				continue;
			if (jb_physreadbuf(OA(d), t3, sizeof(t3))) {
				printf("L3 table 0x%llx unreadable (VA 0x%llx)\n", OA(d), KVA(i, j, 0));
				continue;
			}
			for (int k = 0; k < 2048; k++) {
				uint64_t e = t3[k], pa = OA(e);
				if ((e & 3) == 3 && pa < lo + len && lo < pa + 0x4000) {
					show_map(KVA(i, j, k), pa, 0x4000, e, 3);
					hits++;
				}
			}
		}
	}
	printf("%llu mappings\n", hits);
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

/* Device registers, mapped uncached (see the top) */
#define kIOMapInhibitCache 0x100
#define MMIO_MAX 0x10000

static volatile uint32_t *mmio_map(uint64_t pa, uint64_t len)
{
	if (pa & 3 || !len || len > MMIO_MAX) {
		fprintf(stderr, "kmem: mmio wants a 4-byte aligned PA and 0 < LEN <= 0x%x\n", MMIO_MAX);
		exit(1);
	}
	if (pa < dram_base + dram_size && dram_base < pa + len) {
		fprintf(stderr, "kmem: 0x%llx+0x%llx is DRAM: use pr\n", pa, len);
		exit(1);
	}
	/* PMGR reg 19 (inside the PMP's ASC block): one bulk read through m1n1's
	 * proxy killed it (2026-11-01) */
	if (pa < 0x23bc1c000ULL + 0x40000 && 0x23bc1c000ULL < pa + len) {
		fprintf(stderr, "kmem: 0x%llx+0x%llx touches PMGR 0x23bc1c000+0x40000 (killed m1n1 once)\n", pa, len);
		exit(1);
	}
	/* PMGR reg 0 past the first four PLLs (0x23b000000-0x23b00ffff read
	 * fine): a 64 KiB read from 0x23b010000 took iOS down (2026-11-02);
	 * PLLs that are off may not answer */
	if (pa < 0x23b040000ULL && 0x23b010000ULL < pa + len) {
		fprintf(stderr, "kmem: 0x%llx+0x%llx touches PMGR 0x23b010000-0x23b03ffff (took iOS down once)\n", pa, len);
		exit(1);
	}
	uint64_t pg = pa & ~(uint64_t)(CHUNK - 1);
	uint64_t sz = (pa + len - pg + CHUNK - 1) & ~(uint64_t)(CHUNK - 1);
	void *u = NULL;
	if (jb_iosurface_map_cache(pg, sz, &u, kIOMapInhibitCache) || !u) {
		fprintf(stderr, "kmem: uncached map of 0x%llx+0x%llx failed\n", pg, sz);
		exit(1);
	}
	return (volatile uint32_t *)((uint8_t *)u + (pa - pg));
}

static void mmio_dump(uint64_t pa, uint64_t len)
{
	volatile uint32_t *m = mmio_map(pa, len);
	for (uint64_t o = 0; o < len; o += 4) {
		uint32_t v = m[o / 4];
		if (v)
			printf("0x%llx = 0x%08x\n", pa + o, v);
	}
}

static double now(void)
{
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return tv.tv_sec + tv.tv_usec / 1e6;
}

static void mmio_watch(double secs, int n, char **pas)
{
	volatile uint32_t *m[16];
	uint32_t last[16];
	uint64_t pa[16];
	if (n > 16)
		n = 16;
	for (int i = 0; i < n; i++) {
		pa[i] = num(pas[i]);
		m[i] = mmio_map(pa[i], 4);
		last[i] = *m[i];
		printf("%9.6f 0x%llx = 0x%08x\n", 0.0, pa[i], last[i]);
	}
	fflush(stdout);
	double t0 = now();
	unsigned long samples = 0;
	while (now() - t0 < secs) {
		for (int i = 0; i < n; i++) {
			uint32_t v = *m[i];
			if (v != last[i]) {
				printf("%9.6f 0x%llx = 0x%08x (was 0x%08x)\n", now() - t0, pa[i], v, last[i]);
				fflush(stdout);
				last[i] = v;
			}
		}
		samples++;
	}
	printf("%lu samples in %.1f s\n", samples, secs);
}

static int usage(void)
{
	fprintf(stderr,
		"usage: kmem info\n"
		"       kmem kr VA [LEN] | pr PA [LEN]\n"
		"       kmem kd VA LEN FILE | pd PA LEN FILE\n"
		"       kmem q VA | k2p VA | p2k PA\n"
		"       kmem kw VA 32|64 VALUE | pw PA 32|64 VALUE\n"
		"       kmem kcr VA [LEN]\n"
		"       kmem obj CLASS | ptrs VA LEN\n"
		"       kmem ptscan PA LEN\n"
		"       kmem mmio PA [LEN] | mmiowatch SECS PA...\n");
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
	if (!strcmp(c, "q") && argc == 3) {
		uint64_t v;
		if (jb_kreadbuf(num(argv[2]), &v, 8)) {
			fprintf(stderr, "kmem: read failed at %s\n", argv[2]);
			return 1;
		}
		printf("0x%llx\n", v);
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
	if (!strcmp(c, "kcr") && (argc == 3 || argc == 4)) {
		uint64_t a = num(argv[2]), len = argc == 4 ? num(argv[3]) : 0x100;
		via_kcall = 1;
		hexdump(0, a, len);
		return 0;
	}
	if (!strcmp(c, "obj") && argc == 3) {
		objs(argv[2]);
		return 0;
	}
	if (!strcmp(c, "ptrs") && argc == 4) {
		ptrs(num(argv[2]), num(argv[3]));
		return 0;
	}
	if (!strcmp(c, "ptscan") && argc == 4) {
		ptscan(num(argv[2]), num(argv[3]));
		return 0;
	}
	if (!strcmp(c, "mmio") && (argc == 3 || argc == 4)) {
		mmio_dump(num(argv[2]), argc == 4 ? num(argv[3]) : 0x100);
		return 0;
	}
	if (!strcmp(c, "mmiowatch") && argc >= 4) {
		mmio_watch(atof(argv[2]), argc - 3, argv + 3);
		return 0;
	}
	return usage();
}
