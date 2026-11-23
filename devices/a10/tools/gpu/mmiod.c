// SPDX-License-Identifier: GPL-2.0
/*
 * mmiod: the iPad side of the Linux GPU backend (tools/gpu/linuxproxy.py).
 * Started over ssh; reads binary requests on stdin, answers on stdout, and
 * touches physical memory through /dev/mem (O_SYNC: Device memory, no CPU
 * caches in the way -- the GPU's coprocessor sees what we write).
 *
 * Request: u8 op, u64 addr, u64 arg (little endian, 17 bytes).
 *   1 R32            -> u32
 *   2 W32  arg=value -> u8 0
 *   3 R64            -> u64
 *   4 W64  arg=value -> u8 0
 *   5 READ arg=len   -> len bytes      (addr, len multiples of 4)
 *   6 WRITE arg=len, then len bytes -> u8 0
 *   7 TIME           -> u64 CNTVCT_EL0
 *   8 PING           -> u8 0
 * Every access is a 32- or 64-bit load / store of an aligned word.
 */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define PAGE 0x4000ull

static int fd;

struct map {
	uint64_t page;
	size_t len;
	volatile uint8_t *va;
};
static struct map cache[64];
static unsigned next;

static volatile uint8_t *at(uint64_t addr, size_t len)
{
	uint64_t page = addr & ~(PAGE - 1);
	size_t need = ((addr + len + PAGE - 1) & ~(PAGE - 1)) - page;

	for (unsigned i = 0; i < 64; i++)
		if (cache[i].va && cache[i].page <= addr && addr + len <= cache[i].page + cache[i].len)
			return cache[i].va + (addr - cache[i].page);
	struct map *m = &cache[next++ % 64];
	if (m->va)
		munmap((void *)m->va, m->len);
	m->va = mmap(NULL, need, PROT_READ | PROT_WRITE, MAP_SHARED, fd, (off_t)page);
	if (m->va == MAP_FAILED) {
		perror("mmap");
		exit(1);
	}
	m->page = page;
	m->len = need;
	return m->va + (addr - page);
}

static void rd(void *b, size_t n)
{
	uint8_t *p = b;

	while (n) {
		ssize_t r = read(0, p, n);
		if (r <= 0)
			exit(0);
		p += r;
		n -= r;
	}
}

static void wr(const void *b, size_t n)
{
	const uint8_t *p = b;

	while (n) {
		ssize_t r = write(1, p, n);
		if (r <= 0)
			exit(0);
		p += r;
		n -= r;
	}
}

int main(void)
{
	static uint32_t buf[0x40000];	/* 1 MiB of words per READ / WRITE chunk */
	uint8_t req[17], ok = 0;

	fd = open("/dev/mem", O_RDWR | O_SYNC);
	if (fd < 0) {
		perror("/dev/mem");
		return 1;
	}
	for (;;) {
		uint64_t addr, arg;

		rd(req, sizeof(req));
		memcpy(&addr, req + 1, 8);
		memcpy(&arg, req + 9, 8);
		switch (req[0]) {
		case 1: {
			uint32_t v = *(volatile uint32_t *)at(addr, 4);
			wr(&v, 4);
			break;
		}
		case 2:
			*(volatile uint32_t *)at(addr, 4) = (uint32_t)arg;
			__asm__ volatile("dsb sy" ::: "memory");
			wr(&ok, 1);
			break;
		case 3: {
			uint64_t v = *(volatile uint64_t *)at(addr, 8);
			wr(&v, 8);
			break;
		}
		case 4:
			*(volatile uint64_t *)at(addr, 8) = arg;
			__asm__ volatile("dsb sy" ::: "memory");
			wr(&ok, 1);
			break;
		case 5:
		case 6:
			while (arg) {
				size_t n = arg > sizeof(buf) ? sizeof(buf) : arg;
				volatile uint32_t *p = (volatile uint32_t *)at(addr, n);

				if (req[0] == 5) {
					for (size_t i = 0; i < n / 4; i++)
						buf[i] = p[i];
					wr(buf, n);
				} else {
					rd(buf, n);
					for (size_t i = 0; i < n / 4; i++)
						p[i] = buf[i];
					__asm__ volatile("dsb sy" ::: "memory");
				}
				addr += n;
				arg -= n;
			}
			if (req[0] == 6)
				wr(&ok, 1);
			break;
		case 7: {
			uint64_t t;
			__asm__ volatile("isb; mrs %0, cntvct_el0" : "=r"(t));
			wr(&t, 8);
			break;
		}
		case 8:
			wr(&ok, 1);
			break;
		default:
			fprintf(stderr, "mmiod: bad op %u\n", req[0]);
			return 1;
		}
	}
}
