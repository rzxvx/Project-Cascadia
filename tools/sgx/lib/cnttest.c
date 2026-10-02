/* cnttest: N quads in one draw (same texture), in a grid; which of them
 * reach the screen?  Prints the first missing one. */
#include "sgx2d.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main(int argc, char **argv)
{
	int n = atoi(argv[2]), i, fd, first_missing = -1, missing = 0;
	uint32_t v;

	if (sgx2d_open(argv[1])) return 1;
	sgx2d_begin(-1);
	for (i = 0; i < n; i++) {		/* 12x12-pixel cells, 64 per row */
		sgx2d_color(0, 0, 0, 1);
		sgx2d_fill((i % 64) * 12, (i / 64) * 12, 10, 10);
	}
	sgx2d_end();
	sgx2d_finish();
	fd = open("/dev/fb0", O_RDONLY);
	for (i = 0; i < n; i++) {
		pread(fd, &v, 4, (((i / 64) * 12 + 5) * 768 + (i % 64) * 12 + 5) * 4);
		if ((v & 0xffffff) != 0) {	/* still the white background: not drawn */
			if (first_missing < 0) first_missing = i;
			missing++;
		}
	}
	printf("%d quads in one draw: %d missing, first missing %d\n", n, missing, first_missing);
	return 0;
}
