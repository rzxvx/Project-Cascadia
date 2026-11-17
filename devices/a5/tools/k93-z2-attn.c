/*
 * K93 Z2 bootloader liveness check through /dev/hx-touch (apple-z2).
 *
 * open() powers the digitizer (PMU GPIO0 clock, LDO0) with reset held low.
 * Default mode: HXT_IOC_RESET releases reset; a running Z2 bootloader then
 * asserts ATTN and holds it low until serviced. Mode "noreset" is the control:
 * reset stays low, so any reported edge is spurious. In both modes the raw
 * ATTN level (GPIO pin 21, 0x3fa00054 bit0) is sampled after the wait and
 * again 200 ms later, while still open. No SPI data is sent. close() reverses
 * the power-on sequence.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#define HXT_IOC_RESET           _IO('h', 2)
#define HXT_IOC_SETUP_IRQ       _IO('h', 4)
#define HXT_IOC_WAIT_IRQ        _IOW('h', 5, uint32_t)

#define GPIO_PAGE   0x3fa00000UL
#define ATTN_OFF    0x54            /* pin 21 * 4 */

static volatile uint32_t *map_gpio(void)
{
    int fd = open("/dev/mem", O_RDONLY | O_SYNC);
    void *p;

    if (fd < 0)
        return NULL;
    p = mmap(NULL, 0x1000, PROT_READ, MAP_SHARED, fd, GPIO_PAGE);
    close(fd);
    return p == MAP_FAILED ? NULL : p;
}

static int attn_level(volatile uint32_t *gpio)
{
    return gpio ? (int)(gpio[ATTN_OFF / 4] & 1) : -1;
}

int main(int argc, char **argv)
{
    unsigned long ms = argc > 1 ? strtoul(argv[1], NULL, 0) : 1000;
    int noreset = argc > 2 && !strcmp(argv[2], "noreset");
    volatile uint32_t *gpio = map_gpio();
    int fd, ret, l0, l1;

    fd = open("/dev/hx-touch", O_RDWR);
    if (fd < 0) {
        printf("open: %s\n", strerror(errno));
        return 2;
    }
    if (ioctl(fd, HXT_IOC_SETUP_IRQ) < 0 ||
        (!noreset && ioctl(fd, HXT_IOC_RESET) < 0)) {
        printf("ioctl: %s\n", strerror(errno));
        close(fd);
        return 3;
    }
    ret = ioctl(fd, HXT_IOC_WAIT_IRQ, ms);
    l0 = attn_level(gpio);
    usleep(200000);
    l1 = attn_level(gpio);
    printf("mode=%s ATTN_EDGE=%d (timeout %lu ms) ATTN_LEVEL=%d then %d (0 = asserted)\n",
           noreset ? "noreset" : "reset", ret == 0, ms, l0, l1);
    close(fd);
    return 0;
}
