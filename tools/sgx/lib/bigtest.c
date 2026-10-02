/* bigtest: large textures (1024x512, 512x512) side by side, then read the
 * screen back at a few points */
#include "sgx2d.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main(int argc, char **argv)
{
	int dims[3][2] = { { 1024, 512 }, { 512, 512 }, { 256, 128 } }, k, x, y, fd;
	uint32_t *a = malloc(1024 * 1024 * 4), v;

	if (sgx2d_open(argv[1])) return 1;
	sgx2d_begin(-1);
	for (k = 0; k < 3; k++) {
		int w = dims[k][0], h = dims[k][1], id;
		for (y = 0; y < h; y++)
			for (x = 0; x < w; x++)
				a[y * w + x] = 0xff000000 | ((x * 255 / w) & 0xff) | ((y * 255 / h) << 8) | (k * 100 << 16);
		id = sgx2d_texture(a, w, h);
		printf("%dx%d -> id %d\n", w, h, id);
		sgx2d_quad(id, 0, k * 300, 700, 280);
	}
	sgx2d_end();
	sgx2d_finish();
	fd = open("/dev/fb0", O_RDONLY);
	for (k = 0; k < 3; k++)
		for (x = 50; x < 700; x += 300) {
			pread(fd, &v, 4, ((k * 300 + 140) * 768 + x) * 4);
			printf("tex %d at x %d: fb %08x\n", k, x, v);
		}
	return 0;
}
