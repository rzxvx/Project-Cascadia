/* hsicwatch -- log every change of the HSIC Wi-Fi handshake lines and port.
 *
 *   hsicwatch SECONDS
 *
 * Polls, through /dev/mem, HOST_READY (GPIO 50), DEVICE_READY (GPIO 51) and
 * EHCI PORTSC for port 3, and prints a timestamped line whenever one of them
 * changes -- to see what the BCM4334 does with its lines around the firmware
 * download, when the kernel's own view of the port says nothing.
 * Build: arm-linux-gnueabihf-gcc -static -Os -o hsicwatch tools/hsicwatch.c
 */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define GPIO_BASE   0x3fa00000
#define EHCI_BASE   0x36400000
#define HOST_READY  (0xc8 / 4)
#define DEVICE_READY (0xcc / 4)
#define PORTSC3     (0x5c / 4)

static double now(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

int main(int argc, char **argv)
{
    volatile uint32_t *gpio, *ehci;
    uint32_t hr, dr, ps, lhr = ~0u, ldr = ~0u, lps = ~0u;
    double t0, t, end;
    int fd;

    if (argc != 2) {
        fprintf(stderr, "usage: %s SECONDS\n", argv[0]);
        return 2;
    }
    fd = open("/dev/mem", O_RDONLY | O_SYNC);
    if (fd < 0) {
        perror("/dev/mem");
        return 1;
    }
    gpio = mmap(NULL, 4096, PROT_READ, MAP_SHARED, fd, GPIO_BASE);
    ehci = mmap(NULL, 4096, PROT_READ, MAP_SHARED, fd, EHCI_BASE);
    if (gpio == MAP_FAILED || ehci == MAP_FAILED) {
        perror("mmap");
        return 1;
    }
    t0 = now();
    end = t0 + atof(argv[1]);
    setvbuf(stdout, NULL, _IOLBF, 0);
    do {
        hr = gpio[HOST_READY] & 1;
        dr = gpio[DEVICE_READY] & 1;
        ps = ehci[PORTSC3];
        if (hr != lhr || dr != ldr || ps != lps) {
            t = now();
            printf("%9.4f  HOST_READY %u  DEVICE_READY %u  PORTSC3 %08x\n",
                   t - t0, hr, dr, ps);
            lhr = hr;
            ldr = dr;
            lps = ps;
        }
    } while (now() < end);
    return 0;
}
