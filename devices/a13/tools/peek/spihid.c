// SPDX-License-Identifier: GPL-2.0
/*
 * spihid - talk Apple's HID-over-SPI transport from userspace (spidev), the
 * protocol of Asahi's spi-hid-apple (M1 MacBook keyboard/trackpad), for
 * bring-up of the iPhone 11's touch controller once hbpp-load has started
 * its firmware.
 *
 *   spihid [-L LEN] [-P PAD] [-p] [-g CHIP:LINE] [-t SECONDS] [-o FILE] [-m] [-H] DEV
 *
 *   -L   transfer packet length (default 512, the plist's "Transfer Length")
 *   -P   bytes in front of each packet the controller sends (default 4,
 *        "Padding Length"; it sends ef be ad de)
 *   -p   put the same padding in front of the packets we send
 *   -g   the interrupt line, /dev/gpiochipN:LINE (default 1:34 = AOP 34),
 *        active low: while it is low there is a packet to read
 *   -t   after the requests, read input packets for this long (default 5)
 *   -o   append every packet read (raw, LEN+PAD bytes each) to FILE
 *   -C   chip-select setup delay, us, before the first clock (default 300: the
 *        plist's "CS Delay" 300000, ns; the bootloader's is 0). Done as an
 *        empty transfer with a delay at the head of each message, CS held.
 *   -S   no status stage
 *   -K   always answer OK in the status stage (even on a CRC mismatch)
 *   -x   TTDD:HEX  send a raw packet (type TT, device DD, payload HEX) before
 *        the requests, e.g. -x 10d0:a0200000 (a resume message); repeatable
 *   -u   only print/save a packet that differs from the one before
 *   -m   only watch the interrupt line for -t seconds (no SPI), print changes
 *   -H   the interrupt is active high (read while it is high)
 *
 * Reads clock out 0xff: an all-zero packet has a valid CRC and kills the
 * firmware (back to the HBPP bootloader).
 *
 * A packet: flags (0x20 read / 0x40 write), device, offset, remain, length
 * (LE16), data, crc16 over everything before it. A message in the data:
 * u0 u1 u2 id rsplen(LE16) length(LE16) payload crc16. Requests go to the
 * info device 0xd0: device info (20 01 d0), interface info (20 02 n), HID
 * report descriptor (20 10 n). A write is followed, Inter Stage Us later,
 * by a 4-byte status, ac 27 68 d5 when taken.
 */
#include <errno.h>
#include <fcntl.h>
#include <linux/gpio.h>
#include <linux/spi/spidev.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

static int fd, gfd = -1, plen = 512, pad = 4, txpad = 1, active_high, csdelay = 300;
static FILE *out;
static uint8_t rxb[8192], txb[8192];

static uint16_t crc16(const uint8_t *p, size_t n)
{
	uint16_t c = 0;
	int i;

	while (n--) {
		c ^= *p++;
		for (i = 0; i < 8; i++)
			c = c & 1 ? (c >> 1) ^ 0xa001 : c >> 1;
	}
	return c;
}

static uint16_t le16(const uint8_t *p) { return p[0] | p[1] << 8; }
static void put16(uint8_t *p, uint16_t v) { p[0] = v; p[1] = v >> 8; }

static double now(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec / 1e9;
}

static void hexdump(const char *pfx, const uint8_t *p, size_t n)
{
	size_t i;

	for (i = 0; i < n; i++) {
		if (i % 32 == 0)
			printf("%s%s%04zx:", i ? "\n" : "", pfx, i);
		printf(" %02x", p[i]);
	}
	printf("\n");
}

static int irq_low(void)
{
	struct gpio_v2_line_values v = { .mask = 1 };

	if (gfd < 0)
		return 1;
	if (ioctl(gfd, GPIO_V2_LINE_GET_VALUES_IOCTL, &v) < 0)
		return 1;
	return active_high ? (v.bits & 1) : !(v.bits & 1);
}

static int nostatus, always_ok, uniq;
static uint8_t lastpk[4096];

/*
 * One SPI message. hold: leave CS asserted afterwards (the status stage
 * follows in the same chip-select window); cont: this continues a window
 * that is already open (no CS setup delay).
 */
static int spi_msg(struct spi_ioc_transfer *t, int n, int hold, int cont)
{
	struct spi_ioc_transfer x[4];
	int k = 0;

	memset(x, 0, sizeof(x));
	if (!cont)
		x[k++].delay_usecs = csdelay;	/* CS setup: an empty transfer that waits */
	memcpy(x + k, t, n * sizeof(*t));
	k += n;
	x[k - 1].cs_change = hold;
	if (ioctl(fd, SPI_IOC_MESSAGE(k), x) < 0) {
		perror("SPI_IOC_MESSAGE");
		return -1;
	}
	if (!hold)
		usleep(250);	/* Inter Transaction Us */
	return 0;
}

static int spi(struct spi_ioc_transfer *t, int n)
{
	return spi_msg(t, n, 0, 0);
}

/*
 * Status stage, Inter Stage Us after every packet exchange (iOS's
 * HSMTransferDevice): [padding][4-byte status] both ways. Ours says what we
 * made of the packet we just received; the device's says what it made of
 * ours. Without our OK the device sends the same packet again and again.
 */
#define ST_OK	0xd56827ac
#define ST_BUSY	0xf8e5179c
#define ST_NAK	0x0e4fb139
#define ST_ERR	0xb36d5245

static const char *stname(uint32_t s)
{
	return s == ST_OK ? "OK" : s == ST_BUSY ? "BUSY" : s == ST_NAK ? "NAK" :
	       s == ST_ERR ? "ERROR" : "?";
}

/* the status stage, in the chip-select window the packet left open */
static uint32_t status_stage(uint32_t ours)
{
	struct spi_ioc_transfer t;
	uint8_t tx[16], rx[16];
	uint32_t theirs;

	if (nostatus)
		return 0;
	if (always_ok)
		ours = ST_OK;
	usleep(200);	/* Inter Stage Us */
	memset(tx, 0xff, sizeof(tx));
	memcpy(tx + pad, &ours, 4);
	memset(&t, 0, sizeof(t));
	t.tx_buf = (uintptr_t)tx;
	t.rx_buf = (uintptr_t)rx;
	t.len = pad + 4;
	if (spi_msg(&t, 1, 0, 1))
		return 0;
	memcpy(&theirs, rx + pad, 4);
	return theirs;
}

/* read one packet; print it; return its device (or -1) and the message */
static int read_packet(int verbose)
{
	struct spi_ioc_transfer t;
	uint8_t *pk = rxb + pad;
	int len, flags, dev, off, rem, ok;

	memset(&t, 0, sizeof(t));
	/* clock out ones: an all-zero packet has a valid CRC (crc16 of zeros
	 * is 0), and the firmware takes it as a command and drops back to its
	 * bootloader */
	memset(txb, 0xff, pad + plen);
	t.tx_buf = (uintptr_t)txb;
	t.rx_buf = (uintptr_t)rxb;
	t.len = pad + plen;
	if (spi_msg(&t, 1, !nostatus, 0))
		return -1;
	if (uniq && !memcmp(lastpk, rxb, pad + plen)) {
		status_stage(ST_OK);
		return -1;
	}
	memcpy(lastpk, rxb, pad + plen);
	if (out) {
		fwrite(rxb, 1, pad + plen, out);
		fflush(out);
	}

	flags = pk[0];
	dev = pk[1];
	off = le16(pk + 2);
	rem = le16(pk + 4);
	len = le16(pk + 6);
	ok = crc16(pk, plen - 2) == le16(pk + plen - 2);
	{
		uint32_t st = status_stage(ok ? ST_OK : ST_NAK);

		printf("R %8.3f pad %02x%02x%02x%02x flags %02x dev %02x off %u rem %u len %u crc %s  dev-status %08x %s\n",
		       now(), rxb[0], rxb[1], rxb[2], rxb[3], flags, dev, off, rem, len,
		       ok ? "ok" : "BAD", st, stname(st));
	}
	if (!ok) {
		int L;

		/* which packet size would the crc fit? */
		for (L = 16; L <= plen; L += 2)
			if (crc16(pk, L - 2) == le16(pk + L - 2) && L > 10) {
				printf("  crc fits a %d-byte packet\n", L);
				break;
			}
		if (verbose)
			hexdump("  ", rxb, pad + 64);
		return -1;
	}
	if (len >= 10 && len <= plen - 10) {
		const uint8_t *m = pk + 8;
		int mlen = le16(m + 6);

		printf("  msg %02x %02x %02x id %u rsplen %u len %u%s\n", m[0], m[1], m[2], m[3],
		       le16(m + 4), mlen,
		       mlen + 10 <= len && crc16(m, 8 + mlen) == le16(m + 8 + mlen) ? "" : "  (msg crc BAD)");
		if (verbose)
			hexdump("    ", m + 8, mlen < 256 ? mlen : 256);
	} else if (len) {
		hexdump("  data ", pk + 8, len < 64 ? len : 64);
	}
	return dev;
}

static void drain(double secs, int verbose)
{
	double end = now() + secs;

	while (now() < end) {
		if (irq_low())
			read_packet(verbose);
		else
			usleep(500);
	}
}

static void request(uint8_t u0, uint8_t u1, uint8_t u2, uint16_t rsplen)
{
	struct spi_ioc_transfer t[2];
	uint8_t *pk = txb + (txpad ? pad : 0), st[4] = { 0 };
	uint8_t *m = pk + 8;
	int n = (txpad ? pad : 0) + plen;

	memset(txb, 0, sizeof(txb));
	if (txpad)
		memcpy(txb, "\xef\xbe\xad\xde", pad < 4 ? pad : 4);
	pk[0] = 0x40;
	pk[1] = 0xd0;
	put16(pk + 6, 8 + 0 + 2);
	m[0] = u0; m[1] = u1; m[2] = u2; m[3] = 0;
	put16(m + 4, rsplen);
	put16(m + 6, 0);
	put16(m + 8, crc16(m, 8));
	put16(pk + plen - 2, crc16(pk, plen - 2));

	memset(t, 0, sizeof(t));
	t[0].tx_buf = (uintptr_t)txb;
	t[0].rx_buf = (uintptr_t)rxb;
	t[0].len = n;
	if (spi_msg(t, 1, !nostatus, 0))
		return;
	{
		uint8_t *rp = rxb + pad;
		int rok = crc16(rp, plen - 2) == le16(rp + plen - 2);
		uint32_t s = status_stage(rok ? ST_OK : ST_NAK);

		memcpy(st, &s, 4);
		printf("W %8.3f %02x %02x %02x -> dev-status %08x %s; got flags %02x dev %02x len %u crc %s\n",
		       now(), u0, u1, u2, s, stname(s), rp[0], rp[1], le16(rp + 6), rok ? "ok" : "BAD");
	}

}

/* send one packet: transfer type, device, payload bytes (no message CRC added) */
static void send_raw(uint8_t type, uint8_t dev, const uint8_t *pl, int plen_)
{
	struct spi_ioc_transfer t;
	uint8_t *pk = txb + (txpad ? pad : 0);
	int n = (txpad ? pad : 0) + plen;

	memset(txb, 0, sizeof(txb));
	if (txpad)
		memcpy(txb, "\xef\xbe\xad\xde", pad < 4 ? pad : 4);
	pk[0] = type;
	pk[1] = dev;
	put16(pk + 6, plen_);
	memcpy(pk + 8, pl, plen_);
	put16(pk + plen - 2, crc16(pk, plen - 2));
	memset(&t, 0, sizeof(t));
	t.tx_buf = (uintptr_t)txb;
	t.rx_buf = (uintptr_t)rxb;
	t.len = n;
	if (spi_msg(&t, 1, !nostatus, 0))
		return;
	{
		uint8_t *rp = rxb + pad;
		int rok = crc16(rp, plen - 2) == le16(rp + plen - 2);
		uint32_t s = status_stage(rok ? ST_OK : ST_NAK);

		printf("X %8.3f type %02x dev %02x len %d -> dev-status %08x %s; got flags %02x dev %02x len %u\n",
		       now(), type, dev, plen_, s, stname(s), rp[0], rp[1], le16(rp + 6));
	}
}

static const char *pre[8];
static int npre;

int main(int argc, char **argv)
{
	char chip[64] = "/dev/gpiochip1";
	int line = 34, c, i, secs = 5, monitor = 0;

	while ((c = getopt(argc, argv, "L:P:pg:t:o:mHC:SKx:u")) != -1) {
		switch (c) {
		case 'L': plen = atoi(optarg); break;
		case 'P': pad = atoi(optarg); break;
		case 'p': txpad = 1; break;
		case 'g': {
			int n;

			if (sscanf(optarg, "%d:%d", &n, &line) != 2)
				goto usage;
			snprintf(chip, sizeof(chip), "/dev/gpiochip%d", n);
			break;
		}
		case 't': secs = atoi(optarg); break;
		case 'o': out = fopen(optarg, "ab"); break;
		case 'm': monitor = 1; break;
		case 'H': active_high = 1; break;
		case 'C': csdelay = atoi(optarg); break;
		case 'S': nostatus = 1; break;
		case 'K': always_ok = 1; break;
		case 'u': uniq = 1; setvbuf(stdout, NULL, _IOLBF, 0); break;
		case 'x': if (npre < 8) pre[npre++] = optarg; break;
		default: goto usage;
		}
	}
	if (optind != argc - 1 || plen < 16 || plen + pad > 4096) {
usage:
		fprintf(stderr, "usage: spihid [-L LEN] [-P PAD] [-p] [-g CHIP:LINE] [-t S] [-o FILE] [-m] [-H] [-C US] DEV\n");
		return 2;
	}
	fd = open(argv[optind], O_RDWR);
	if (fd < 0) {
		perror(argv[optind]);
		return 1;
	}

	{
		struct gpio_v2_line_request r;
		int cfd = open(chip, O_RDONLY);

		memset(&r, 0, sizeof(r));
		r.offsets[0] = line;
		r.num_lines = 1;
		r.config.flags = GPIO_V2_LINE_FLAG_INPUT;
		strcpy(r.consumer, "spihid");
		if (cfd < 0 || ioctl(cfd, GPIO_V2_GET_LINE_IOCTL, &r) < 0)
			perror("interrupt line (reading without it)");
		else
			gfd = r.fd;
	}

	printf("interrupt line %s:%d is %s\n", chip, line, gfd < 0 ? "?" : irq_low() ? "asserted" : "idle");
	if (monitor) {
		double t0 = now(), end = t0 + secs;
		int last = irq_low(), n = 0;

		while (now() < end) {
			int v = irq_low();

			if (v != last && n++ < 200)
				printf("  %9.6f %s\n", now() - t0, v ? "asserted" : "idle");
			last = v;
		}
		printf("  %d changes in %d s\n", n, secs);
		return 0;
	}
	printf("== pending\n");
	drain(0.3, 1);

	for (i = 0; i < npre; i++) {
		unsigned ty, dv;
		uint8_t pl[64];
		int n = 0;
		const char *h = strchr(pre[i], ':');

		if (sscanf(pre[i], "%2x%2x", &ty, &dv) != 2 || !h)
			continue;
		for (h++; h[0] && h[1] && n < 64; h += 2)
			sscanf(h, "%2hhx", &pl[n++]);
		printf("== raw %s\n", pre[i]);
		send_raw(ty, dv, pl, n);
		drain(0.3, 1);
	}
	printf("== device info\n");
	request(0x20, 0x01, 0xd0, 512);
	drain(0.5, 1);
	for (i = 0; i < 4; i++) {
		printf("== interface %d info\n", i);
		request(0x20, 0x02, i, 512);
		drain(0.3, 1);
	}
	for (i = 1; i < 4; i++) {
		printf("== interface %d HID report descriptor\n", i);
		request(0x20, 0x10, i, 512);
		drain(0.3, 1);
	}
	printf("== input for %d s (touch the screen)\n", secs);
	drain(secs, 0);
	return 0;
}
