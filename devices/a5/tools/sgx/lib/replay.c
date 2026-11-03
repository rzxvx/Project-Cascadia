/* replay: draw a dumped frame (SGX2D_DUMP) with every texture white */
#include "sgx2d.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct quad { int tex, mode; float xy[8], uv[8], rgba[4]; };

int main(int argc, char **argv)
{
	FILE *f = fopen(argv[2], "rb");
	int n, i, lim = argc > 3 ? atoi(argv[3]) : 1 << 30;
	struct quad *q;
	uint32_t px[16];
	int white;

	if (!f || sgx2d_open(argv[1])) return 1;
	fread(&n, sizeof(n), 1, f);
	q = malloc(n * sizeof(*q));
	fread(q, sizeof(*q), n, f);
	for (i = 0; i < 16; i++) px[i] = 0xffffffff;
	white = sgx2d_texture(px, 4, 4);
	int frame_tex = -1, frame_id = argc > 5 ? atoi(argv[5]) : -1;
	if (argc > 4) {				/* a real texture for one id */
		FILE *g = fopen(argv[4], "rb");
		uint32_t *t = malloc(1024 * 512 * 4);
		fread(t, 4, 1024 * 512, g);
		frame_tex = sgx2d_texture(t, 1024, 512);
	}
	sgx2d_begin(-1);
	const char *only = getenv("ONLY");		/* "a-b,c-d": quad ranges to keep */
	for (i = 0; i < n && i < lim; i++) {
		if (only) {
			int keep = 0, a, b;
			const char *p = only;
			while (sscanf(p, "%d-%d", &a, &b) == 2) {
				keep |= i >= a && i <= b;
				if (!(p = strchr(p, ','))) break;
				p++;
			}
			if (!keep) continue;
		}
		sgx2d_color(q[i].rgba[0], q[i].rgba[1], q[i].rgba[2], q[i].rgba[3]);
		sgx2d_blend(q[i].mode);
		sgx2d_quad4(q[i].tex == frame_id ? frame_tex : white, q[i].xy, q[i].uv);
	}
	sgx2d_end();
	sgx2d_finish();
	printf("replayed %d of %d quads\n", i, n);
	return 0;
}
