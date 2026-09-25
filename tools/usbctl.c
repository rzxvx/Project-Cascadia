/* usbctl -- one control transfer to a USB device through usbfs, bytes out.
 *
 *   usbctl DEV TYPE REQ VALUE INDEX LEN        (numbers in hex)
 *   usbctl /dev/bus/usb/001/002 80 06 0100 0 12      device descriptor
 *   usbctl /dev/bus/usb/001/002 c1 00 0 0 8          Broadcom DL_GETSTATE
 *
 * For the Wi-Fi chip on HSIC port 3: what it answers now, not what the
 * kernel cached when it enumerated -- after brcmfmac starts the firmware the
 * device may answer as something else without ever re-enumerating.
 * Build: arm-linux-gnueabihf-gcc -static -Os -o usbctl tools/usbctl.c
 */
#include <fcntl.h>
#include <linux/usbdevice_fs.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    unsigned char buf[4096];
    struct usbdevfs_ctrltransfer c;
    int fd, n, i;

    if (argc != 7) {
        fprintf(stderr, "usage: %s DEV TYPE REQ VALUE INDEX LEN (hex)\n", argv[0]);
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
