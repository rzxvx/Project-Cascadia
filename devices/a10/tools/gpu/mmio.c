// SPDX-License-Identifier: GPL-2.0
/*
 * mmio: read or write 32-bit MMIO words through /dev/mem (the iPad's Alpine
 * busybox has no devmem). Built statically by tools/gpu/mmio.sh.
 *
 *   mmio ADDR            print the word at ADDR
 *   mmio ADDR VALUE      write VALUE, then print the word read back
 *   mmio -n N ADDR       print N consecutive words
 *
 * Each access prints its line and flushes before the next one, so a read
 * that hangs the bus leaves everything before it on the console.
 */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static volatile uint32_t *map(int fd, uint64_t addr, void **base, size_t *len)
{
	uint64_t page = addr & ~0x3fffull;

	*len = 0x8000;
	*base = mmap(NULL, *len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, (off_t)page);
	if (*base == MAP_FAILED) {
		perror("mmap");
		exit(1);
	}
	return (volatile uint32_t *)((char *)*base + (addr - page));
}

int main(int argc, char **argv)
{
	unsigned long n = 1;
	int i = 1;

	if (argc > 2 && !strcmp(argv[1], "-n")) {
		n = strtoul(argv[2], NULL, 0);
		i = 3;
	}
	if (argc - i < 1 || argc - i > 2 || n == 0 || n > 0x1000) {
		fprintf(stderr, "usage: mmio [-n N] ADDR [VALUE]\n");
		return 2;
	}
	uint64_t addr = strtoull(argv[i], NULL, 0);
	int fd = open("/dev/mem", O_RDWR | O_SYNC);

	if (fd < 0) {
		perror("/dev/mem");
		return 1;
	}
	for (unsigned long k = 0; k < n; k++) {
		void *base;
		size_t len;
		volatile uint32_t *p = map(fd, addr + 4 * k, &base, &len);

		if (argc - i == 2) {
			*p = (uint32_t)strtoul(argv[i + 1], NULL, 0);
			__asm__ volatile("dsb sy" ::: "memory");
		}
		printf("%#llx: %#010x\n", (unsigned long long)(addr + 4 * k), *p);
		fflush(stdout);
		munmap(base, len);
	}
	close(fd);
	return 0;
}
