/* usbctl -- one control transfer to a USB device through usbfs, bytes out.
 *
 *   usbctl DEV TYPE REQ VALUE INDEX LEN        (numbers in hex)
 *   usbctl DEV TYPE REQ VALUE INDEX LEN HEX    OUT request, HEX its data
 *   usbctl DEV reset                           USBDEVFS_RESET: a port reset
 *   usbctl DEV read EP LEN MS                  one bulk/interrupt IN, interface 0
 *   usbctl DEV cmd HEX                         BCDC: claim interface 0, listen on
 *                                              the interrupt endpoint 81, send HEX
 *                                              (SEND_ENCAPSULATED_COMMAND), read
 *                                              the response and what 81 said
 *   usbctl DEV rdl EP FILE                     boot-loader download the way iOS does
 *                                              it: GETVER, the image in 1500-byte
 *                                              chunks to bulk OUT EP, GETSTATE after
 *                                              the first and the last, then DL_GO --
 *                                              every command with wValue 1, no DL_START
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
    if (argc == 5 && !strcmp(argv[2], "rdl")) {
        static unsigned char img[1 << 20];
        struct usbdevfs_bulktransfer b;
        unsigned int ifc = 0, st[6];
        FILE *f = fopen(argv[4], "rb");
        int size, off, ep = strtoul(argv[3], NULL, 16);

        if (!f) {
            perror(argv[4]);
            return 1;
        }
        size = fread(img, 1, sizeof(img), f);
        fclose(f);
        fd = open(argv[1], O_RDWR);
        if (fd < 0 || ioctl(fd, USBDEVFS_CLAIMINTERFACE, &ifc) < 0) {
            perror("claim");
            return 1;
        }
#define RDL(req, what) do { \
            memset(st, 0, sizeof(st)); \
            c.bRequestType = 0xc1; c.bRequest = (req); c.wValue = 1; c.wIndex = 0; \
            c.wLength = 8; c.timeout = 2000; c.data = st; \
            n = ioctl(fd, USBDEVFS_CONTROL, &c); \
            printf("%-8s %s: %08x %08x\n", what, n < 0 ? strerror(errno) : "ok", st[0], st[1]); \
        } while (0)
        RDL(5, "GETVER");
        for (off = 0; off < size; off += 1500) {
            b.ep = ep;
            b.len = size - off < 1500 ? size - off : 1500;
            b.timeout = 2000;
            b.data = img + off;
            if (ioctl(fd, USBDEVFS_BULK, &b) < 0) {
                printf("bulk at %d: %s\n", off, strerror(errno));
                return 1;
            }
            if (off == 0)
                RDL(0, "GETSTATE");
        }
        printf("sent %d bytes\n", size);
        RDL(0, "GETSTATE");
        if (st[0] != 4) {
            printf("not runnable\n");
            return 1;
        }
        RDL(2, "GO");
        return 0;
    }
    if (argc == 4 && !strcmp(argv[2], "cmd")) {
        static unsigned char ibuf[64];
        struct usbdevfs_urb u, *done;
        unsigned int ifc = 0;
        const char *h = argv[3];
        int len = 0, k;

        fd = open(argv[1], O_RDWR);
        if (fd < 0 || ioctl(fd, USBDEVFS_CLAIMINTERFACE, &ifc) < 0) {
            perror("claim");
            return 1;
        }
        memset(&u, 0, sizeof(u));
        u.type = USBDEVFS_URB_TYPE_INTERRUPT;
        u.endpoint = 0x81;
        u.buffer = ibuf;
        u.buffer_length = sizeof(ibuf);
        if (ioctl(fd, USBDEVFS_SUBMITURB, &u) < 0)
            perror("submit 81");
        usleep(200000);
        memset(buf, 0, sizeof(buf));
        for (i = 0; h[i] && h[i + 1]; i += 2) {
            char b2[3] = { h[i], h[i + 1], 0 };
            buf[len++] = strtoul(b2, NULL, 16);
        }
        c.bRequestType = 0x21; c.bRequest = 0; c.wValue = 0; c.wIndex = 0;
        c.wLength = len; c.timeout = 1000; c.data = buf;
        n = ioctl(fd, USBDEVFS_CONTROL, &c);
        printf("sent %d: %s\n", len, n < 0 ? strerror(errno) : "ok");
        for (k = 0; k < 10; k++) {
            usleep(100000);
            if (ioctl(fd, USBDEVFS_REAPURBNDELAY, &done) == 0) {
                printf("ep 81: status %d, %d bytes:", done->status, done->actual_length);
                for (i = 0; i < done->actual_length; i++)
                    printf(" %02x", ibuf[i]);
                printf("\n");
                break;
            }
        }
        c.bRequestType = 0xa1; c.bRequest = 1; c.wLength = 0x200; c.data = buf;
        n = ioctl(fd, USBDEVFS_CONTROL, &c);
        if (n < 0)
            printf("response: %s\n", strerror(errno));
        else {
            printf("response %d:", n);
            for (i = 0; i < n && i < 64; i++)
                printf(" %02x", buf[i]);
            printf("\n");
        }
        ioctl(fd, USBDEVFS_DISCARDURB, &u);
        return 0;
    }
    if (argc == 6 && !strcmp(argv[2], "read")) {
        struct usbdevfs_bulktransfer b;
        unsigned int ifc = 0;

        fd = open(argv[1], O_RDWR);
        if (fd < 0 || ioctl(fd, USBDEVFS_CLAIMINTERFACE, &ifc) < 0) {
            perror("claim");
            return 1;
        }
        b.ep = strtoul(argv[3], NULL, 16);
        b.len = strtoul(argv[4], NULL, 16);
        if (b.len > sizeof(buf))
            b.len = sizeof(buf);
        b.timeout = strtoul(argv[5], NULL, 10);
        b.data = buf;
        n = ioctl(fd, USBDEVFS_BULK, &b);
        if (n < 0) {
            printf("ep %02x: %s\n", b.ep, strerror(errno));
            return 1;
        }
        printf("ep %02x: %d bytes:", b.ep, n);
        for (i = 0; i < n; i++)
            printf(" %02x", buf[i]);
        printf("\n");
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
