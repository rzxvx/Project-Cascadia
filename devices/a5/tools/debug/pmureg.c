/* pmureg -- read or write one D1946 PMU register over /dev/i2c-0, past the
 * kernel driver that owns the address (I2C_SLAVE_FORCE).  The device rootfs
 * has no i2c-tools and usually no network to apk add them.
 *
 *   pmureg REG          print the register
 *   pmureg REG VAL      write it, then print what reads back
 *
 * Some registers power the iPad off when written (0x29, 0x42 -- see the
 * charging notes); this tool does not stop you.  Built like usbwatch:
 * arm-linux-gnueabihf-gcc -static -Os in the build image. */
#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <linux/i2c.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define PMU_ADDR 0x3c

static int xfer(int fd, unsigned char reg, unsigned char *val, int write)
{
	unsigned char wbuf[2] = { reg, write ? *val : 0 };
	struct i2c_msg msgs[2] = {
		{ .addr = PMU_ADDR, .flags = 0, .len = write ? 2 : 1, .buf = wbuf },
		{ .addr = PMU_ADDR, .flags = I2C_M_RD, .len = 1, .buf = val },
	};
	struct i2c_rdwr_ioctl_data d = { .msgs = msgs, .nmsgs = write ? 1 : 2 };

	return ioctl(fd, I2C_RDWR, &d) < 0 ? -1 : 0;
}

int main(int argc, char **argv)
{
	unsigned char reg, val;
	int fd;

	if (argc < 2 || argc > 3) {
		fprintf(stderr, "usage: pmureg REG [VAL]\n");
		return 2;
	}
	fd = open("/dev/i2c-0", O_RDWR);
	if (fd < 0) {
		perror("/dev/i2c-0");
		return 1;
	}
	reg = strtoul(argv[1], NULL, 16);
	if (argc == 3) {
		val = strtoul(argv[2], NULL, 16);
		if (xfer(fd, reg, &val, 1)) {
			perror("write");
			return 1;
		}
	}
	if (xfer(fd, reg, &val, 0)) {
		perror("read");
		return 1;
	}
	printf("%02x: %02x\n", reg, val);
	return 0;
}
