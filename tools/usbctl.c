/* usbctl -- one control transfer to a USB device through usbfs, bytes out.
 *
 *   usbctl DEV TYPE REQ VALUE INDEX LEN        (numbers in hex)
 *   usbctl DEV TYPE REQ VALUE INDEX LEN HEX    OUT request, HEX its data
 *   usbctl DEV reset                           USBDEVFS_RESET: a port reset
 *   usbctl DEV dump ADDR LEN OUT               chip memory -> OUT, in the
 *                                              BCM4334's CPU-less mode
 *   usbctl /dev/bus/usb/001/002 80 06 0100 0 12      device descriptor
 *   usbctl /dev/bus/usb/001/002 c1 00 0 0 8          Broadcom DL_GETSTATE
 *
 * For the Wi-Fi chip on HSIC port 3: what it answers now, not what the
 * kernel cached when it enumerated -- after brcmfmac starts the firmware the
 * device may answer as something else without ever re-enumerating.
 * Build: arm-linux-gnueabihf-gcc -static -Os -o usbctl tools/usbctl.c
 */
#include <errno.h>
#include <fcntl.h>
#include <linux/usbdevice_fs.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    unsigned char buf[4096];
    struct usbdevfs_ctrltransfer c;
    int fd, n, i;

    if (argc == 3 && !strcmp(argv[2], "reset")) {
        fd = open(argv[1], O_RDWR);
        if (fd < 0 || ioctl(fd, USBDEVFS_RESET, 0) < 0) {
            perror("USBDEVFS_RESET");
            return 1;
        }
        printf("reset done\n");
        return 0;
    }
    if (argc == 6 && !strcmp(argv[2], "dump")) {
        /* Apple's doReadSoCRAM: vendor IN to the device, bRequest 0,
         * the backplane address in wValue/wIndex, 508 bytes a time. */
        unsigned long a = strtoul(argv[3], NULL, 16);
        unsigned long end = a + strtoul(argv[4], NULL, 16);
        FILE *out = fopen(argv[5], "wb");

        fd = open(argv[1], O_RDWR);
        if (fd < 0 || !out) {
            perror("open");
            return 1;
        }
        for (; a < end; a += n) {
            c.bRequestType = 0xc0;
            c.bRequest = 0;
            c.wValue = a & 0xffff;
            c.wIndex = a >> 16;
            c.wLength = end - a < 0x1fc ? end - a : 0x1fc;
            c.timeout = 1000;
            c.data = buf;
            n = ioctl(fd, USBDEVFS_CONTROL, &c);
            if (n <= 0) {
                fprintf(stderr, "read at 0x%lx: %s\n", a, n < 0 ? strerror(errno) : "short");
                return 1;
            }
            fwrite(buf, 1, n, out);
        }
        fclose(out);
        return 0;
    }
    if (argc != 7 && argc != 8) {
        fprintf(stderr, "usage: %s DEV TYPE REQ VALUE INDEX LEN [HEXDATA] (hex)\n", argv[0]);
        return 2;
    }
    fd = open(argv[1], O_RDWR);
    if (fd < 0) {
        perror(argv[1]);
        return 1;
    }
    c.bRequestType = strtoul(argv[2], NULL, 16);
    c.bRequest = strtoul(argv[3], NULL, 16);
    c.wValue = strtoul(argv[4], NULL, 16);
    c.wIndex = strtoul(argv[5], NULL, 16);
    c.wLength = strtoul(argv[6], NULL, 16);
    if (c.wLength > sizeof(buf))
        c.wLength = sizeof(buf);
    memset(buf, 0, sizeof(buf));
    if (argc == 8) {                    /* OUT data, zero-padded to LEN */
        const char *h = argv[7];
        for (i = 0; h[i] && h[i + 1] && i / 2 < (int)sizeof(buf); i += 2) {
            char b[3] = { h[i], h[i + 1], 0 };
            buf[i / 2] = strtoul(b, NULL, 16);
        }
    }
    c.timeout = 1000;
    c.data = buf;
    n = ioctl(fd, USBDEVFS_CONTROL, &c);
    if (n < 0) {
        perror("USBDEVFS_CONTROL");
        return 1;
    }
    printf("%d bytes:", n);
    for (i = 0; i < n; i++)
        printf(" %02x", buf[i]);
    printf("\n");
    return 0;
}
