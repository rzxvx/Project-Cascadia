// SPDX-License-Identifier: GPL-2.0
/*
 * memdump - copy a physical range to stdout through /dev/mem, 64 bits at a
 * time.  Outside RAM, arm64 maps /dev/mem as Device-nGnRnE (read() of it
 * fails there with EFAULT; mmap works), which Apple's carve-outs and MMIO
 * need.  Used for the GPU firmware's __TEXT (a13/docs/research/gpu.md, G3).
 *
 *   memdump ADDR LEN > out.bin       ADDR and LEN 16 KiB aligned
 *
 * Built like peek: static, in docker alpine:3.20 arm64.
 */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

int main(int argc, char **argv)
{
	if (argc != 3) {
		fprintf(stderr, "usage: %s ADDR LEN > out.bin\n", argv[0]);
		return 2;
	}
	uint64_t addr = strtoull(argv[1], NULL, 0), len = strtoull(argv[2], NULL, 0);
	if ((addr | len) & 0x3fff) {
		fprintf(stderr, "memdump: ADDR and LEN must be 16 KiB aligned\n");
		return 2;
	}
	int fd = open("/dev/mem", O_RDONLY | O_SYNC);
	if (fd < 0) {
		perror("/dev/mem");
		return 1;
	}
	volatile uint64_t *map = mmap(NULL, len, PROT_READ, MAP_SHARED, fd, addr);
	if (map == MAP_FAILED) {
		perror("mmap");
		return 1;
	}
	static uint64_t buf[2048];
	for (uint64_t off = 0; off < len / 8; off += 2048) {
		for (int i = 0; i < 2048; i++)
			buf[i] = map[off + i];
		if (fwrite(buf, 8, 2048, stdout) != 2048) {
			perror("write");
			return 1;
		}
	}
	return 0;
}
