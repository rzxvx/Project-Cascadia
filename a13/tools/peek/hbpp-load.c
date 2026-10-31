// SPDX-License-Identifier: GPL-2.0
/*
 * hbpp-load - load a multitouch controller's firmware through its HBPP
 * bootloader from userspace (spidev), the way Linux's apple_z2.c does it,
 * printing every answer. For bring-up on the iPhone 11 (N104).
 *
 *   hbpp-load [-c CAL] [-k CHUNK] [-w MS] [-n] [-z] [-A] [-E] DEV FW
 *
 *   DEV    /dev/spidev0.0
 *   FW     a "Z2FW" file (tools/mtfw.py)
 *   -c     calibration blob for SEND_CALIBRATION (else the step is skipped)
 *   -k     split firmware blobs into pieces of at most CHUNK payload bytes
 *          (default 0x3f0, as iOS sends calibration on the A5)
 *   -w     wait after each packet, ms (default 20; apple_z2 waits <= 20 for
 *          the interrupt)
 *   -n     print the packets, send nothing
 *   -A     after EXECUTE, clock nothing more (leave the first packet to spihid)
 *   -E     no ATN_ACK after the EXECUTE write (0x10003400): the firmware is
 *          already running by then (iOS on the A5 just waits 40 ms)
 *   -z     Touch Bar style (apple_z2): no 18 e1 before data packets, ATN_ACK in
 *          the same chip-select window. Default is the A5 bootloader's way
 *          (MTSPIBootloader_Z2): data packets start with 18 e1, and every
 *          packet is followed, after the wait, by ATN_ACK in a window of its
 *          own, retried up to 5 times until 4bc1 (data) / 4ad1 (register).
 *
 * The status word is what the chip clocks out during ATN_ACK (`1a a1`).
 * Blobs go out as 16-bit words (as apple_z2 sends them):
 * on the wire every byte pair of the LE file is swapped, which this does by
 * hand (the controller driver does 8-bit words only).
 */
#include <errno.h>
#include <fcntl.h>
#include <linux/spi/spidev.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define CMD_INIT	0
#define CMD_BLOB	1
#define CMD_CAL		2

static int fd = -1, dry, wait_ms = 20, quiet_after, no_exec_ack;
static size_t chunk = 0x3f0;
static int z2style;
static uint8_t tx[4096], rx[4096];

static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t le16(const uint8_t *p) { return p[0] | p[1] << 8; }
static void put16(uint8_t *p, uint16_t v) { p[0] = v; p[1] = v >> 8; }
static void put32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }

static void hex(const char *what, const uint8_t *p, size_t n)
{
	size_t i;

	printf("%s", what);
	for (i = 0; i < n; i++)
		printf(" %02x", p[i]);
	printf("\n");
}

static void msleep(int ms)
{
	struct timespec t = { ms / 1000, (ms % 1000) * 1000000L };

	nanosleep(&t, NULL);
}

/* one transfer: len bytes of tx; returns rx */
static int xfer(size_t len)
{
	struct spi_ioc_transfer t;

	if (dry)
		return 0;
	memset(&t, 0, sizeof(t));
	t.tx_buf = (uintptr_t)tx;
	t.rx_buf = (uintptr_t)rx;
	t.len = len;
	t.bits_per_word = 8;
	if (ioctl(fd, SPI_IOC_MESSAGE(1), &t) < 0) {
		perror("SPI_IOC_MESSAGE");
		return -1;
	}
	return 0;
}

static int atn_ack(void)
{
	tx[0] = 0x1a;
	tx[1] = 0xa1;
	if (xfer(2))
		return -1;
	return rx[0] << 8 | rx[1];
}

/*
 * Send one packet (LE, as in the file) as 16-bit words, then ATN_ACK.
 * Returns the status word, or -1.
 */
static int send_packet(const char *what, const uint8_t *pkt, size_t len, int words16)
{
	int pre = !z2style && words16 && len >= 2 && pkt[0] == 0x01 && pkt[1] == 0x30;
	int want = pkt[0] == 0x33 && pkt[1] == 0x1e ? 0x4ad1 : 0x4bc1;
	uint8_t *o = tx + (pre ? 2 : 0);
	size_t i, n = len + (pre ? 2 : 0);
	int st = -1, tries;

	if (n + 2 > sizeof(tx) || (words16 && (len & 1))) {
		fprintf(stderr, "%s: bad packet length %zu\n", what, len);
		return -1;
	}
	for (tries = 0; tries < (z2style ? 1 : 5); tries++) {
		tx[0] = 0x18;
		tx[1] = 0xe1;
		for (i = 0; i < len; i++)
			o[i] = words16 ? pkt[i ^ 1] : pkt[i];
		if (z2style) {
			tx[n] = 0x1a;
			tx[n + 1] = 0xa1;
		}
		if (dry) {
			printf("  %-28s %5zu B:", what, n);
			hex("", tx, n < 16 ? n : 16);
			return 0;
		}
		if (xfer(n + (z2style ? 2 : 0)))
			return -1;
		if (z2style) {
			st = rx[n] << 8 | rx[n + 1];
			msleep(wait_ms);
			break;
		}
		if (no_exec_ack && pkt[0] == 0x33 && pkt[1] == 0x1e &&
		    pkt[2] == 0x00 && pkt[3] == 0x34 && pkt[4] == 0x00 && pkt[5] == 0x10) {
			printf("  %-28s %5zu B  EXECUTE, no ATN_ACK\n", what, n);
			msleep(40);
			return want;
		}
		msleep(wait_ms);
		st = atn_ack();
		if (st == want || (pkt[0] == 0x01 && pkt[1] == 0x1f))
			break;
		printf("  %-28s status %04x, retrying\n", what, st);
	}
	printf("  %-28s %5zu B  rx head %02x %02x %02x %02x  status %04x%s\n",
	       what, n, rx[0], rx[1], rx[2], rx[3], st,
	       z2style || st == want || (pkt[0] == 0x01 && pkt[1] == 0x1f) ? "" : "  NOT OK");
	return st;
}

/* A 0x3001 blob: header (8) + csum16 + payload + sum32; resent in pieces. */
static int send_blob(const uint8_t *b, size_t len)
{
	uint8_t pkt[4096];
	uint32_t addr = le32(b + 4), sum;
	size_t plen = len - 14, off, n, i;
	char what[64];

	if (len < 14 || le16(b) != 0x3001) {
		fprintf(stderr, "not a 0x3001 blob\n");
		return -1;
	}
	for (off = 0; off < plen; off += n) {
		n = plen - off < chunk ? plen - off : chunk;
		put16(pkt, 0x3001);
		put16(pkt + 2, (n + 3) / 4);
		put32(pkt + 4, addr + off);
		put16(pkt + 8, pkt[2] + pkt[3] + pkt[4] + pkt[5] + pkt[6] + pkt[7]);
		memcpy(pkt + 10, b + 10 + off, n);
		for (sum = 0, i = 0; i < n; i++)
			sum += b[10 + off + i];
		put32(pkt + 10 + n, sum);
		snprintf(what, sizeof(what), "data %08x +%zu", addr + (uint32_t)off, n);
		if (send_packet(what, pkt, 14 + n, 1) < 0)
			return -1;
	}
	return 0;
}

static int hbpp_check(void)
{
	int i;

	tx[0] = 0x1a; tx[1] = 0xa1;
	for (i = 2; i < 16; i += 2) {
		tx[i] = 0x18;
		tx[i + 1] = 0xe1;
	}
	if (dry || xfer(16))
		return 0;
	hex("  HBPP check ->", rx, 16);
	return 0;
}

int main(int argc, char **argv)
{
	uint8_t *fw, *cal = NULL;
	size_t fwlen, callen = 0, off;
	struct stat sb;
	int c, f;

	while ((c = getopt(argc, argv, "c:k:w:nzAE")) != -1) {
		switch (c) {
		case 'c':
			f = open(optarg, O_RDONLY);
			if (f < 0 || fstat(f, &sb)) { perror(optarg); return 1; }
			callen = sb.st_size;
			cal = malloc(callen);
			if (read(f, cal, callen) != (ssize_t)callen) { perror(optarg); return 1; }
			close(f);
			break;
		case 'k': chunk = strtoul(optarg, NULL, 0) & ~3UL; break;
		case 'w': wait_ms = atoi(optarg); break;
		case 'n': dry = 1; break;
		case 'z': z2style = 1; break;
		case 'A': quiet_after = 1; break;
		case 'E': no_exec_ack = 1; break;
		default: goto usage;
		}
	}
	if (argc - optind != 2 || !chunk || chunk > sizeof(tx) - 16) {
usage:
		fprintf(stderr, "usage: hbpp-load [-c CAL] [-k CHUNK] [-w MS] [-n] [-z] [-A] [-E] DEV FW\n");
		return 2;
	}

	f = open(argv[optind + 1], O_RDONLY);
	if (f < 0 || fstat(f, &sb)) { perror(argv[optind + 1]); return 1; }
	fwlen = sb.st_size;
	fw = malloc(fwlen);
	if (read(f, fw, fwlen) != (ssize_t)fwlen) { perror("read"); return 1; }
	close(f);
	if (fwlen < 8 || memcmp(fw, "Z2FW", 4) || le32(fw + 4) != 1) {
		fprintf(stderr, "%s: not a Z2FW v1 file\n", argv[optind + 1]);
		return 1;
	}

	if (!dry) {
		fd = open(argv[optind], O_RDWR);
		if (fd < 0) { perror(argv[optind]); return 1; }
	}

	printf("before:\n");
	hbpp_check();
	hbpp_check();

	printf("upload:\n");
	for (off = 8; off + 8 <= fwlen; off = (off + 3) & ~3UL) {
		uint32_t cmd = le32(fw + off), arg = le32(fw + off + 4);

		off += 8;
		if (cmd == CMD_INIT || cmd == CMD_BLOB) {
			if (off + arg > fwlen) { fprintf(stderr, "truncated\n"); return 1; }
			if (cmd == CMD_BLOB && arg >= 14 && le16(fw + off) == 0x3001) {
				if (send_blob(fw + off, arg)) return 1;
			} else {
				const char *what = cmd == CMD_INIT ? "init payload" :
					le16(fw + off) == 0x1e33 ? "register write" :
					le16(fw + off) == 0x1f01 ? "request calibration" : "blob";
				char buf[64];

				if (le16(fw + off) == 0x1e33)
					snprintf(buf, sizeof(buf), "%s %08x=%x", what,
						 le32(fw + off + 2), le32(fw + off + 10));
				else
					snprintf(buf, sizeof(buf), "%s", what);
				if (send_packet(buf, fw + off, arg, cmd != CMD_INIT) < 0) return 1;
			}
			off += arg;
		} else if (cmd == CMD_CAL) {
			uint8_t *b;
			uint32_t sum = 0;
			size_t i;

			if (!cal) {
				printf("  calibration @%08x: no -c, skipped\n", arg);
				continue;
			}
			b = calloc(1, 14 + callen);
			put16(b, 0x3001);
			put16(b + 2, (callen + 3) / 4);
			put32(b + 4, arg);
			put16(b + 8, b[2] + b[3] + b[4] + b[5] + b[6] + b[7]);
			memcpy(b + 10, cal, callen);
			for (i = 0; i < callen; i++)
				sum += cal[i];
			put32(b + 10 + callen, sum);
			printf("  calibration @%08x: %zu bytes\n", arg, callen);
			if (send_blob(b, 14 + callen)) return 1;
			free(b);
		} else {
			fprintf(stderr, "unknown load command %u at %#zx\n", cmd, off - 8);
			return 1;
		}
	}

	if (quiet_after)
		return 0;
	printf("after:\n");
	msleep(100);
	hbpp_check();
	if (!dry) {
		memset(tx, 0, 64);
		xfer(64);
		hex("  64 zero bytes ->", rx, 64);
	}
	return 0;
}
