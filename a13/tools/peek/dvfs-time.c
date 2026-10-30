// SPDX-License-Identifier: GPL-2.0
/*
 * dvfs-time - time Apple cluster P-state transitions (T8030 bring-up).
 *
 *   dvfs-time [-n REPEAT] [-q] CLUSTER_BASE PSTATE [PSTATE...]
 *
 * -n repeats the whole list (stress); -q prints only transitions that stay
 * BUSY, and a summary.
 *
 * For each P-state: read-modify-write the cluster PSTATE register (base +
 * 0x20020: DESIRED1 [4:0], DESIRED2 [15:12], SET bit 25) the way
 * apple-soc-cpufreq does, then poll BUSY (bit 31) in a tight loop and print
 * how long it stayed set, plus the STATUS register (base + 0x20050).
 * Gives up after 50 ms.
 */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define PSTATE 0x20020
#define STATUS 0x20050
#define BUSY   (1ULL << 31)
#define SET    (1ULL << 25)

static uint64_t now_ns(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return (uint64_t)t.tv_sec * 1000000000ULL + t.tv_nsec;
}

int main(int argc, char **argv)
{
	long repeat = 1, done_n = 0, stuck_n = 0;
	int quiet = 0, first = 1;

	while (first < argc && argv[first][0] == '-') {
		if (argv[first][1] == 'n' && first + 1 < argc)
			repeat = atol(argv[++first]);
		else if (argv[first][1] == 'q')
			quiet = 1;
		first++;
	}
	if (argc - first < 2) {
		fprintf(stderr, "usage: %s [-n REPEAT] [-q] CLUSTER_BASE PSTATE [PSTATE...]\n", argv[0]);
		return 2;
	}
	uint64_t base = strtoull(argv[first], NULL, 0);
	int fd = open("/dev/mem", O_RDWR | O_SYNC);
	if (fd < 0) {
		perror("/dev/mem");
		return 1;
	}
	long pg = sysconf(_SC_PAGESIZE);
	uint8_t *m = mmap(NULL, pg, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
			  (base + PSTATE) & ~(uint64_t)(pg - 1));
	if (m == MAP_FAILED) {
		perror("mmap");
		return 1;
	}
	volatile uint64_t *ps = (void *)(m + ((base + PSTATE) & (pg - 1)));
	volatile uint64_t *st = (void *)(m + ((base + STATUS) & (pg - 1)));

	for (long r = 0; r < repeat && !stuck_n; r++)
	for (int a = first + 1; a < argc; a++) {
		unsigned p = atoi(argv[a]);
		uint64_t v = *ps, polls = 0;

		v &= ~0x1fULL & ~0xf000ULL;
		v |= p | ((uint64_t)p << 12) | SET;
		uint64_t t0 = now_ns(), t;
		*ps = v;
		while ((*ps & BUSY) && (t = now_ns()) - t0 < 50000000ULL)
			polls++;
		t = now_ns();
		if (*ps & BUSY)
			stuck_n++;
		else
			done_n++;
		if (quiet && !(*ps & BUSY))
			continue;
		printf("-> %u: %s after %llu us (%llu polls), PSTATE=0x%llx STATUS=0x%llx\n", p,
		       (*ps & BUSY) ? "STILL BUSY" : "done",
		       (unsigned long long)((t - t0) / 1000), (unsigned long long)polls,
		       (unsigned long long)*ps, (unsigned long long)*st);
		if (*ps & BUSY)
			break;
	}
	if (quiet)
		printf("0x%llx: %ld done, %ld stuck\n", (unsigned long long)base, done_n, stuck_n);
	return 0;
}
