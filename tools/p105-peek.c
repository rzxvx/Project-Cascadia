/* p105-peek -- MMIO poke tool for P105AP Linux bring-up.
 *
 * Built statically and dropped into the initramfs so that, once there is an
 * interactive shell (CDC ACM over Lightning), SoC registers can be explored
 * without another flash cycle.  busybox on this rootfs has no devmem applet.
 *
 * Two things make this different from a stock devmem:
 *
 *  1. SIGBUS/SIGSEGV are caught.  Unpowered blocks on this SoC (SPI1 0x321*,
 *     grape/PWM 0x335*) abort the bus on access; that kills a normal devmem
 *     but here it just prints "--------" and carries on, which is exactly
 *     what probing for powered blocks needs.
 *
 *  2. "scan" samples a window twice and reports only the words that changed.
 *     With no clockevent registered, jiffies are frozen and sleep()/usleep()
 *     never return -- so the delay is a busy loop on purpose.  Do not swap it
 *     for nanosleep, it will hang the shell.
 *
 * Usage:
 *   peek r    ADDR [WORDS]           dump 32-bit words
 *   peek w    ADDR VAL               write one 32-bit word
 *   peek scan ADDR WORDS [SPIN]      find free-running counters in a window
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <sys/mman.h>

static sigjmp_buf jb;
static volatile sig_atomic_t in_access;

static void fault_handler(int sig)
{
	(void)sig;
	if (in_access)
		siglongjmp(jb, 1);
	_exit(2);
}

static int memfd = -1;
static void *map_base;
static unsigned long map_len;

static volatile uint32_t *win(unsigned long phys, unsigned long words)
{
	unsigned long pagesz = (unsigned long)sysconf(_SC_PAGESIZE);
	unsigned long base = phys & ~(pagesz - 1);
	unsigned long off = phys - base;
	unsigned long len = off + words * 4;

	len = (len + pagesz - 1) & ~(pagesz - 1);
	map_base = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, memfd,
			(off_t)base);
	if (map_base == MAP_FAILED) {
		perror("mmap");
		return NULL;
	}
	map_len = len;
	return (volatile uint32_t *)((char *)map_base + off);
}

/* returns 0 and stores the value, or -1 if the access aborted */
static int rd(volatile uint32_t *p, uint32_t *out)
{
	in_access = 1;
	if (sigsetjmp(jb, 1) == 0) {
		*out = *p;
		in_access = 0;
		return 0;
	}
	in_access = 0;
	return -1;
}

static int wr(volatile uint32_t *p, uint32_t v)
{
	in_access = 1;
	if (sigsetjmp(jb, 1) == 0) {
		*p = v;
		in_access = 0;
		return 0;
	}
	in_access = 0;
	return -1;
}

static void spin(unsigned long n)
{
	volatile unsigned long i;

	for (i = 0; i < n; i++)
		;
}

static void usage(void)
{
	fputs("usage:\n"
	      "  peek r    ADDR [WORDS]        dump 32-bit words\n"
	      "  peek w    ADDR VAL            write one 32-bit word\n"
	      "  peek scan ADDR WORDS [SPIN]   report words that change between two samples\n"
	      "addresses and values are hex, with or without 0x\n", stderr);
}

int main(int argc, char **argv)
{
	unsigned long addr, words = 1, spincount = 20000000UL;
	volatile uint32_t *p;
	const char *cmd;

	if (argc < 3) {
		usage();
		return 1;
	}
	cmd = argv[1];
	addr = strtoul(argv[2], NULL, 16);

	signal(SIGBUS, fault_handler);
	signal(SIGSEGV, fault_handler);

	memfd = open("/dev/mem", O_RDWR | O_SYNC);
	if (memfd < 0) {
		perror("open /dev/mem");
		return 1;
	}

	if (!strcmp(cmd, "r")) {
		unsigned long i;

		if (argc > 3)
			words = strtoul(argv[3], NULL, 0);
		p = win(addr, words);
		if (!p)
			return 1;
		for (i = 0; i < words; i++) {
			uint32_t v;

			if ((i % 4) == 0)
				printf("%08lx:", addr + i * 4);
			if (rd(p + i, &v) == 0)
				printf(" %08x", v);
			else
				printf(" --------");
			if ((i % 4) == 3 || i + 1 == words)
				putchar('\n');
		}
	} else if (!strcmp(cmd, "w")) {
		uint32_t v;

		if (argc < 4) {
			usage();
			return 1;
		}
		v = (uint32_t)strtoul(argv[3], NULL, 16);
		p = win(addr, 1);
		if (!p)
			return 1;
		if (wr(p, v) < 0) {
			printf("%08lx <= %08x : BUS ABORT\n", addr, v);
			return 2;
		}
		printf("%08lx <= %08x\n", addr, v);
	} else if (!strcmp(cmd, "scan")) {
		uint32_t *a, *b;
		unsigned long i;
		int moved = 0;

		if (argc < 4) {
			usage();
			return 1;
		}
		words = strtoul(argv[3], NULL, 0);
		if (argc > 4)
			spincount = strtoul(argv[4], NULL, 0);
		p = win(addr, words);
		if (!p)
			return 1;
		a = calloc(words, 4);
		b = calloc(words, 4);
		if (!a || !b)
			return 1;
		for (i = 0; i < words; i++)
			if (rd(p + i, &a[i]) < 0)
				a[i] = 0xdeadbeef;
		spin(spincount);
		for (i = 0; i < words; i++)
			if (rd(p + i, &b[i]) < 0)
				b[i] = 0xdeadbeef;
		for (i = 0; i < words; i++) {
			if (a[i] == 0xdeadbeef || a[i] == b[i])
				continue;
			printf("%08lx: %08x -> %08x  (delta %+ld)\n",
			       addr + i * 4, a[i], b[i],
			       (long)((int32_t)(b[i] - a[i])));
			moved++;
		}
		printf("scan: %lu words, %d moving\n", words, moved);
	} else {
		usage();
		return 1;
	}

	if (map_base)
		munmap(map_base, map_len);
	close(memfd);
	return 0;
}
