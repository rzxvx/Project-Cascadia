// SPDX-License-Identifier: GPL-2.0
/*
 * peek - read (and, with a value, write) a physical address through /dev/mem.
 * Outside RAM, arm64 maps /dev/mem as Device-nGnRnE, which Apple's MMIO needs.
 *
 *   peek ADDR [32|64]           read
 *   peek ADDR 32|64 VALUE       write, then read back
 */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

int main(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: %s ADDR [32|64] [VALUE]\n", argv[0]);
		return 2;
	}
	uint64_t addr = strtoull(argv[1], NULL, 0);
	int width = argc > 2 ? atoi(argv[2]) : 32;
	long pg = sysconf(_SC_PAGESIZE);
	int fd = open("/dev/mem", O_RDWR | O_SYNC);
	if (fd < 0) {
		perror("/dev/mem");
		return 1;
	}
	uint8_t *map = mmap(NULL, pg, PROT_READ | PROT_WRITE, MAP_SHARED, fd, addr & ~(uint64_t)(pg - 1));
	if (map == MAP_FAILED) {
		perror("mmap");
		return 1;
	}
	volatile void *p = map + (addr & (pg - 1));
	if (argc > 3) {
		uint64_t v = strtoull(argv[3], NULL, 0);
		if (width == 64)
			*(volatile uint64_t *)p = v;
		else
			*(volatile uint32_t *)p = (uint32_t)v;
	}
	if (width == 64)
		printf("0x%016llx\n", (unsigned long long)*(volatile uint64_t *)p);
	else
		printf("0x%08x\n", *(volatile uint32_t *)p);
	return 0;
}
