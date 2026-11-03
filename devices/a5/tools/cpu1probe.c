/* cpu1probe -- does CPU1 start from the first page of DRAM when it is
 * enabled the way iOS enables it?
 *
 * iOS 6.1's AppleS5L8940XPerformanceController (the ADT's function-enable_core
 * is its 'Core' function, arg = core mask: cpu0 1, cpu1 2) enables a core by
 * writing the mask to PMGR+0x1214, then PMGR+0x1220, then 0 to PMGR+0x1204,
 * and disables it with the mask to PMGR+0x1210.  XNU's cpu_start() has put
 * its exception vectors at physBase, the first page of DRAM, before that.
 *
 *   cpu1probe look     read PMGR+0x1200..0x1224 and the beacon
 *   cpu1probe enable   plant the stub, enable CPU1 as iOS does
 *   cpu1probe cycle    plant the stub, disable CPU1, then enable it
 *   cpu1probe release  plant the stub, only +0x1214 and +0x1220 -- not the
 *                      0 to +0x1204, whose other bits are unknown
 *   cpu1probe run FILE STEP
 *                      stop CPU1 (+0x1210), put FILE (tools/cpu1stub) at the
 *                      start of the page with STEP at +0xfe0, start CPU1 and
 *                      follow the stub's beacon: +0xff0 step, +0xff4 ACTLR,
 *                      +0xff8 SCTLR; NOSTOP=1 skips the stop, FLAGS=
 *                      sets the stub's +0xfdc
 *
 * Needs the reserved, unmapped page at 0x80000000 (dts: cpu-reset@80000000).
 * Every one of the eight ARM vectors there branches to a stub that writes
 * 0x100 + vector number and its lr to the page's last 16 bytes and spins, so
 * the beacon says both that the core ran and how it came in.
 * Build: arm-linux-gnueabihf-gcc -static -Os -o cpu1probe tools/cpu1probe.c
 */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define RESET_PAGE  0x80000000u
#define BEACON      0xff0              /* beacon word, lr after it */
#define PMGR_PAGE   0x3f101000u
#define CPU1_MASK   2                  /* never 1: that is CPU0 */

static volatile uint32_t *page, *pmgr;

static void plant(void)
{
    int n;

    for (n = 0; n < 8; n++) {
        uint32_t stub = 0x40 + n * 0x20;
        uint32_t *s = (uint32_t *)page + stub / 4;

        /* vector n: b stub */
        page[n] = 0xea000000 | (((stub - (n * 4 + 8)) >> 2) & 0xffffff);
        s[0] = 0xe59f000c;                     /* ldr r0, [pc, #12]      */
        s[1] = 0xe3001100 | n;                 /* movw r1, #0x100 + n    */
        s[2] = 0xe5801000;                     /* str r1, [r0]           */
        s[3] = 0xe580e004;                     /* str lr, [r0, #4]       */
        s[4] = 0xeafffffe;                     /* b .                    */
        s[5] = RESET_PAGE + BEACON;            /* literal                */
    }
    page[BEACON / 4] = 0;
    page[BEACON / 4 + 1] = 0;
}

static void regs(const char *when)
{
    int o;

    printf("%s:", when);
    for (o = 0x200; o <= 0x224; o += 4)
        printf(" %03x=%08x", 0x1000 + o, pmgr[o / 4]);
    printf("\n");
}

static void wr(uint32_t off, uint32_t v)
{
    pmgr[(off - 0x1000) / 4] = v;
    printf("  PMGR+%x <- %x\n", off, v);
}

static void watch(const char *what)
{
    int i;

    for (i = 0; i < 20; i++) {
        if (page[BEACON / 4]) {
            printf("  %s: BEACON %08x lr %08x after %d ms\n", what,
                   page[BEACON / 4], page[BEACON / 4 + 1], i * 10);
            return;
        }
        usleep(10000);
    }
    printf("  %s: no beacon\n", what);
}

int main(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "look";
    int fd = open("/dev/mem", O_RDWR | O_SYNC);

    if (fd < 0) {
        perror("/dev/mem");
        return 1;
    }
    page = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, RESET_PAGE);
    pmgr = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, PMGR_PAGE);
    if (page == MAP_FAILED || pmgr == MAP_FAILED) {
        perror("mmap");
        return 1;
    }
    regs("PMGR");
    printf("beacon %08x lr %08x\n", page[BEACON / 4], page[BEACON / 4 + 1]);
    if (!strcmp(mode, "look"))
        return 0;

    if (!strcmp(mode, "run") && argc == 4) {
        static uint32_t img[0xfe0 / 4];
        FILE *f = fopen(argv[2], "rb");
        size_t n = f ? fread(img, 1, sizeof(img), f) : 0;
        uint32_t last[3] = { 0, 0, 0 };
        int i, k;

        if (!n) {
            perror(argv[2]);
            return 1;
        }
        if (!getenv("NOSTOP")) {
            wr(0x1210, CPU1_MASK);
            usleep(2000);
        }
        for (i = 0; i < (int)((n + 3) / 4); i++)
            page[i] = img[i];
        page[0xfe0 / 4] = strtoul(argv[3], NULL, 0);
        page[0xfdc / 4] = getenv("FLAGS") ? strtoul(getenv("FLAGS"), NULL, 0) : 0xf;
        page[BEACON / 4] = page[BEACON / 4 + 1] = page[BEACON / 4 + 2] = 0;
        page[0xfd8 / 4] = page[0xfec / 4] = 0;
        __asm__ volatile("dsb" ::: "memory");
        wr(0x1214, CPU1_MASK);
        wr(0x1220, CPU1_MASK);
        for (i = 0; i < 50; i++) {
            uint32_t now[3] = { page[BEACON / 4], page[BEACON / 4 + 1],
                                page[BEACON / 4 + 2] };
            if (memcmp(now, last, sizeof(now))) {
                printf("  %3d ms: beacon %08x  ACTLR/lr %08x  SCTLR %08x\n",
                       i * 10, now[0], now[1], now[2]);
                fflush(stdout);
                memcpy(last, now, sizeof(now));
            }
            usleep(10000);
        }
        (void)k;
        if ((page[BEACON / 4] & 0xff0) == 0xee0)
            printf("  exception %u: lr %08x DFSR %08x IFSR %08x DFAR %08x IFAR %08x\n",
                   page[BEACON / 4] & 0xf, page[BEACON / 4 + 1], page[BEACON / 4 + 2],
                   page[BEACON / 4 + 3], page[0xfe4 / 4], page[0xfe8 / 4]);
        printf("CPU0 still here after 500 ms; CPU1 started %u times, woke %u times\n",
               page[0xfd8 / 4], page[0xfec / 4]);
        return 0;
    }

    plant();
    printf("stub planted: %08x %08x ...\n", page[0], page[0x40 / 4]);
    if (!strcmp(mode, "cycle")) {
        wr(0x1210, CPU1_MASK);
        usleep(1000);
        regs("after disable");
        watch("after disable");
    }
    wr(0x1214, CPU1_MASK);
    wr(0x1220, CPU1_MASK);
    if (strcmp(mode, "release"))
        wr(0x1204, 0);
    regs("after enable");
    watch("after enable");
    __asm__ volatile("dsb; sev" ::: "memory");
    watch("after sev");
    return 0;
}
