// SPDX-License-Identifier: GPL-2.0-only
/*
 * cascadia-hfs: Data's HFS on the running iOS 8.4.1 of the iPad mini 1, for
 * creating and removing the Linux volume (spec 6.4 step 1, 6.5 step 4).
 *
 *   cascadia-hfs info            HFS header fields of Data, key=value
 *   cascadia-hfs resize BLOCKS   resize the mounted Data volume to BLOCKS
 *
 * The resize is Apple's HFS resize request (HFSIOC_RESIZE_VOLUME, as in
 * Apple's published HFS sources) on /private/var, the volume staying mounted.
 * Run as root.  Nothing else is written.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

int fsctl(const char *path, unsigned long request, void *data, unsigned int options);

#define HFSIOC_RESIZE_VOLUME	0x80086802UL	/* _IOW('h', 2, u_int64_t) */
#define DATA_DEV		"/dev/rdisk0s1s2"
#define DATA_MNT		"/private/var"

static uint32_t be16(const uint8_t *p) { return (uint32_t)p[0] << 8 | p[1]; }
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

static uint8_t blk[4096];

/* The 512-byte HFS header at byte off of the partition (off % 4096 == 1024 or 3072). */
static const uint8_t *header_at(int fd, uint64_t off)
{
	uint64_t base = off & ~(uint64_t)4095;

	if (pread(fd, blk, sizeof(blk), (off_t)base) != (ssize_t)sizeof(blk))
		return NULL;
	return blk + (off - base);
}

static int info(void)
{
	const uint8_t *h;
	uint32_t sig, bs, total;
	int fd = open(DATA_DEV, O_RDONLY);

	if (fd < 0) {
		printf("error=open %s: %d\n", DATA_DEV, errno);
		return 1;
	}
	h = header_at(fd, 1024);
	if (!h) {
		printf("error=read: %d\n", errno);
		close(fd);
		return 1;
	}
	sig = be16(h);
	bs = be32(h + 40);
	total = be32(h + 44);
	printf("signature=%c%c\nversion=%u\nattributes=0x%08x\nblocksize=%u\ntotal=%u\nfree=%u\n",
	       sig >> 8, sig & 0xff, be16(h + 2), be32(h + 4), bs, total, be32(h + 48));
	if (bs >= 4096 && total) {
		h = header_at(fd, (uint64_t)total * bs - 1024);
		if (h)
			printf("alt_signature=%c%c\nalt_total=%u\n", be16(h) >> 8, be16(h) & 0xff,
			       be32(h + 44));
	}
	close(fd);
	return 0;
}

int main(int argc, char **argv)
{
	uint64_t bytes;
	unsigned long blocks;
	time_t t0;
	int r, e;

	if (argc == 2 && !strcmp(argv[1], "info"))
		return info();
	if (argc != 3 || strcmp(argv[1], "resize")) {
		printf("usage: cascadia-hfs info | resize BLOCKS\n");
		return 2;
	}
	blocks = strtoul(argv[2], NULL, 10);
	if (blocks < 524288) {
		printf("error=refusing %lu blocks (under 2 GiB)\n", blocks);
		return 2;
	}
	bytes = (uint64_t)blocks * 4096;
	t0 = time(NULL);
	r = fsctl(DATA_MNT, HFSIOC_RESIZE_VOLUME, &bytes, 0);
	e = r ? errno : 0;
	printf("fsctl=%d\nerrno=%d\nseconds=%ld\n", r, e, (long)(time(NULL) - t0));
	info();
	return r ? 1 : 0;
}
