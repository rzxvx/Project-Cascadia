/* pbtest: N full-screen quads (each in its own draw); the last colour should
 * be everywhere.  Prints how many of 96 sample points have it. */
#include "sgx2d.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main(int argc, char **argv)
{
	int n = atoi(argv[2]), i, fd, ok = 0, x, y, tex[2];
	uint32_t v, px[16];

	if (sgx2d_open(argv[1])) return 1;
	for (i = 0; i < 16; i++) px[i] = 0xffffffff;
	tex[0] = sgx2d_texture(px, 4, 4);
	tex[1] = sgx2d_texture(px, 4, 4);
	sgx2d_begin(-1);
	for (i = 0; i < n; i++) {
		sgx2d_color((i & 1) ? 1 : 0, (i & 2) ? 1 : 0, (i & 4) ? 1 : 0.5f, 1);
		sgx2d_quad(tex[i & 1], 0, 0, 768, 1024);	/* alternate textures: one draw each */
	}
	sgx2d_end();
	sgx2d_finish();
	fd = open("/dev/fb0", O_RDONLY);
	i = n - 1;
	for (y = 50; y < 1024; y += 100)
		for (x = 30; x < 768; x += 80) {
			uint32_t want = ((i & 1) ? 0xff0000 : 0) | ((i & 2) ? 0xff00 : 0) | ((i & 4) ? 0xff : 0x7f);
			pread(fd, &v, 4, (y * 768 + x) * 4);
			ok += (v & 0xffffff) == want || ((v & 0xffffff) ^ want) < 0x2;
		}
	printf("%d full-screen quads: last colour at %d of 100 points\n", n, ok);
	return 0;
}
