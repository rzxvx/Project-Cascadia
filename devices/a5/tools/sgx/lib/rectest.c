/* rectest: is the rectangle layout right?  A 64x16 and a 16x64 texture whose
 * texels encode their own (x, y), drawn 1:1; prints mismatches. */
#include "sgx2d.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main(int argc, char **argv)
{
	static uint32_t a[64 * 16], fb[64];
	int fd, t[2], k, x, y, bad = 0;
	int dims[2][2] = { { 64, 16 }, { 16, 64 } };

	if (sgx2d_open(argv[1])) return 1;
	sgx2d_begin(-1);
	for (k = 0; k < 2; k++) {
		int w = dims[k][0], h = dims[k][1];
		for (y = 0; y < h; y++)
			for (x = 0; x < w; x++)
				a[y * w + x] = 0xff000000 | (k << 22) | (y << 8) | x;
		t[k] = sgx2d_texture(a, w, h);
		sgx2d_quad(t[k], 100 + k * 200, 100, w, h);
	}
	sgx2d_end();
	sgx2d_finish();
	fd = open("/dev/fb0", O_RDONLY);
	for (k = 0; k < 2; k++) {
		int w = dims[k][0], h = dims[k][1];
		for (y = 0; y < h; y++) {
			pread(fd, fb, w * 4, ((100 + y) * 768 + 100 + k * 200) * 4);
			for (x = 0; x < w; x++) {	/* fb is BGRA: our R (x) lands in bits 16-23 */
				uint32_t v = fb[x], gx = (v >> 16) & 0xff, gy = (v >> 8) & 0xff;
				if (gx != (uint32_t)x || gy != (uint32_t)y) {
					if (bad++ < 6) printf("%dx%d at (%d,%d): got (%u,%u)\n", w, h, x, y, gx, gy);
				}
			}
		}
	}
	printf("mismatches: %d of %d\n", bad, 2 * 64 * 16);
	return 0;
}
