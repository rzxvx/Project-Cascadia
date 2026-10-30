// SPDX-License-Identifier: GPL-2.0
/*
 * spin - run a fixed integer loop pinned to one CPU and print how long it
 * took, to see the core clock change with the cluster P-state.
 *
 *   spin CPU [ITERATIONS]      (default 200000000)
 */
#define _GNU_SOURCE
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

int main(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: %s CPU [ITERATIONS]\n", argv[0]);
		return 2;
	}
	int cpu = atoi(argv[1]);
	uint64_t n = argc > 2 ? strtoull(argv[2], NULL, 0) : 200000000ULL;
	cpu_set_t set;

	CPU_ZERO(&set);
	CPU_SET(cpu, &set);
	if (sched_setaffinity(0, sizeof(set), &set)) {
		perror("sched_setaffinity");
		return 1;
	}
	struct timespec a, b;
	volatile uint64_t x = 0;

	clock_gettime(CLOCK_MONOTONIC, &a);
	for (uint64_t i = 0; i < n; i++)
		x += i;
	clock_gettime(CLOCK_MONOTONIC, &b);
	double s = (b.tv_sec - a.tv_sec) + (b.tv_nsec - a.tv_nsec) / 1e9;
	printf("cpu%d: %llu iterations in %.3f s (%.0f M/s)\n", cpu, (unsigned long long)n, s,
	       n / s / 1e6);
	return 0;
}
