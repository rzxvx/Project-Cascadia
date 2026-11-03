// SPDX-License-Identifier: GPL-2.0
/*
 * spix - one full-duplex transfer through spidev, for bring-up.
 *
 *   spix DEV HEXBYTES [HZ]       e.g. spix /dev/spidev0.0 1aa118e118e1
 *   spix DEV +N [HZ]             clock N zero bytes, print what comes back
 *
 * Prints the received bytes in hex. Mode 0, 8-bit words, CS held for the
 * whole transfer.
 */
#include <fcntl.h>
#include <linux/spi/spidev.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

static int hexval(int c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

int main(int argc, char **argv)
{
	static uint8_t tx[65536], rx[65536];
	struct spi_ioc_transfer t;
	size_t n = 0;
	const char *s;
	int fd, i;

	if (argc < 3) {
		fprintf(stderr, "usage: spix DEV HEXBYTES|+N [HZ]\n");
		return 2;
	}
	s = argv[2];
	if (s[0] == '+') {
		n = strtoul(s + 1, NULL, 0);
		if (n > sizeof(tx))
			n = sizeof(tx);
	} else {
		for (; s[0] && n < sizeof(tx); ) {
			int hi, lo;

			if (s[0] == ' ' || s[0] == ':') { s++; continue; }
			hi = hexval(s[0]);
			lo = s[1] ? hexval(s[1]) : -1;
			if (hi < 0 || lo < 0) {
				fprintf(stderr, "bad hex at '%s'\n", s);
				return 2;
			}
			tx[n++] = hi << 4 | lo;
			s += 2;
		}
	}

	fd = open(argv[1], O_RDWR);
	if (fd < 0) {
		perror(argv[1]);
		return 1;
	}
	memset(&t, 0, sizeof(t));
	t.tx_buf = (uintptr_t)tx;
	t.rx_buf = (uintptr_t)rx;
	t.len = n;
	t.bits_per_word = 8;
	if (argc > 3)
		t.speed_hz = strtoul(argv[3], NULL, 0);
	if (ioctl(fd, SPI_IOC_MESSAGE(1), &t) < 0) {
		perror("SPI_IOC_MESSAGE");
		return 1;
	}
	for (i = 0; i < (int)n; i++)
		printf("%02x%s", rx[i], (i % 32 == 31 || i == (int)n - 1) ? "\n" : " ");
	return 0;
}
