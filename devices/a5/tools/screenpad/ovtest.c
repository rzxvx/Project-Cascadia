/*
 * ovtest PATTERN X Y W H SECONDS -- the screen's overlay checked by hand:
 * one rectangle of PATTERN set as apple-sgx's overlay for SECONDS, then
 * taken away (the file closes).  PATTERN: clear (alpha 0: the screen must
 * not change by a pixel), red (alpha 128: every pixel the blend of red
 * and what was there, by half), solid, grid.  The overlay is the plane's
 * size, which ovtest asks for; with nothing on KMS it says 0 x 0 and shows
 * nothing.  Read the screen back with tools/desktop/xshot.py --screen
 * (docs/research/p105-screenpad.md has the comparison).
 *
 * Built like screenpad (tools/screenpad/build.sh), by hand:
 *   arm-linux-gnueabihf-gcc -static -O2 -I patches/files/include/uapi/drm \
 *       -I /usr/arm-linux-gnueabihf/include/drm -o ovtest tools/screenpad/ovtest.c
 */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <drm/drm.h>
#include "apple_sgx_drm.h"

int main(int argc, char **argv)
{
	struct drm_apple_sgx_gem_create g = { .size = 4096 * 1024 };
	struct drm_apple_sgx_gem_mmap_offset m = { 0 };
	struct drm_apple_sgx_overlay o = { .flags = APPLE_SGX_OVERLAY_QUERY };
	uint32_t *p;
	int fd;

	if (argc != 7) {
		fprintf(stderr, "usage: ovtest clear|red|solid|grid X Y W H SECONDS\n");
		return 2;
	}
	fd = open("/dev/dri/renderD128", O_RDWR);
	if (fd < 0 || ioctl(fd, DRM_IOCTL_APPLE_SGX_OVERLAY, &o)) {
		perror("ovtest: the overlay");
		return 1;
	}
	printf("plane %ux%u, rotation %#x\n", o.out_width, o.out_height, o.out_rotation);
	if (ioctl(fd, DRM_IOCTL_APPLE_SGX_GEM_CREATE, &g))
		return perror("ovtest: a buffer"), 1;
	m.handle = g.handle;
	if (ioctl(fd, DRM_IOCTL_APPLE_SGX_GEM_MMAP_OFFSET, &m))
		return perror("ovtest: mmap offset"), 1;
	p = mmap(NULL, g.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, m.offset);
	if (p == MAP_FAILED)
		return perror("ovtest: mmap"), 1;
	for (int j = 0; j < 1024; j++)
		for (int i = 0; i < 1024; i++) {
			uint32_t px = 0;

			if (!strcmp(argv[1], "red"))
				px = 0x80ff0000;
			else if (!strcmp(argv[1], "solid"))
				px = 0xffff0000;
			else if (!strcmp(argv[1], "grid"))
				px = ((i / 16 + j / 16) & 1) ? 0xff00ff00 : 0xff0000ff;
			p[j * 1024 + i] = px;
		}
	o = (struct drm_apple_sgx_overlay){ .handle = g.handle, .pitch = 4096,
					    .width = o.out_width, .height = o.out_height };
	o.nrects = 1;
	o.rects[0].x = atoi(argv[2]);
	o.rects[0].y = atoi(argv[3]);
	o.rects[0].w = atoi(argv[4]);
	o.rects[0].h = atoi(argv[5]);
	if (ioctl(fd, DRM_IOCTL_APPLE_SGX_OVERLAY, &o))
		return perror("ovtest: setting it"), 1;
	sleep(atoi(argv[6]));
	return 0;
}
