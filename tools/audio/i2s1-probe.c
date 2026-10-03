/* i2s1-probe -- bring I2S1 up as a clock master by hand and time its TX FIFO.
 *
 *   i2s1-probe [AUDIO_CLK] [NCO_HZ] [FREF_GUESS] [TXCON] [WORDS] [ACS] [TXCOM] [CLKCON]
 *
 * Research tool for docs/research/p105-audio.md, not part of the image.
 * In order, each step from what iOS does (12H321):
 *   1. I2S1's PMGR power state on (0x3f1010a4, the gate recipe)
 *   2. ACS (0x341a0014) = ACS, if given: what enableAE2 writes (1) moves
 *      I2S1 onto AE2's clock, and with AE2's own gates off the block then
 *      reads 0x0015006b everywhere and drops writes -- 0 is the default
 *   3. AUDIO-CLK (0x3f100058) = AUDIO_CLK
 *   4. NCO1 (0x3f100120..) = NCO_HZ, by setNCOFrequency's sequence, with
 *      FREF_GUESS as its reference -- the real one is unknown, and the output
 *      comes out at NCO_HZ * Fref / FREF_GUESS
 *   5. I2S1: STATUS = 1, TXCON, RXCON = 0, TXCOM = RXCOM = 0, CLKCON = 1,
 *      then TXCOM = TX on + interface on, no DMA
 *   6. WORDS zero words into the TX FIFO (+0x10), then STATUS sampled for
 *      ~20 ms; every change is printed with its time in microseconds
 * Bus errors are caught, as in /bin/peek.  Nothing here touches the PMU.
 */
#include <fcntl.h>
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

static sigjmp_buf jb;
static void fault(int sig) { siglongjmp(jb, sig); }

static volatile uint32_t *map(int fd, uint32_t phys)
{
    void *p = mmap(0, 0x1000, PROT_READ | PROT_WRITE, MAP_SHARED, fd, phys & ~0xfffu);
    if (p == MAP_FAILED) { perror("mmap"); exit(1); }
    return (volatile uint32_t *)p;
}

static uint64_t now_us(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000;
}

static int wait_clear(volatile uint32_t *r, uint32_t bits, const char *what)
{
    uint64_t t0 = now_us();
    while (*r & bits)
        if (now_us() - t0 > 100000) {
            printf("  %s: still %08x after 100 ms\n", what, *r);
            return -1;
        }
    return 0;
}

int main(int argc, char **argv)
{
    uint32_t audioclk = argc > 1 ? strtoul(argv[1], 0, 0) : 0x80000001;
    uint32_t hz = argc > 2 ? strtoul(argv[2], 0, 0) : 6144000;
    uint32_t fref = argc > 3 ? strtoul(argv[3], 0, 0) : 24000000;
    uint32_t txcon = argc > 4 ? strtoul(argv[4], 0, 0) : 0x01100301;
    int words = argc > 5 ? atoi(argv[5]) : 64;
    int acsv = argc > 6 ? atoi(argv[6]) : -1;
    uint32_t txcom = argc > 7 ? strtoul(argv[7], 0, 0) : 0x4;
    uint32_t clkcon = argc > 8 ? strtoul(argv[8], 0, 0) : 1;
    int fd = open("/dev/mem", O_RDWR | O_SYNC);
    volatile uint32_t *pmgr, *pmgr1, *acs, *i2s;
    uint32_t last, v;
    uint64_t t0;
    int sig, i, n;

    if (fd < 0) { perror("/dev/mem"); return 1; }
    signal(SIGBUS, fault);
    signal(SIGSEGV, fault);
    pmgr = map(fd, 0x3f100000);       /* clocks, NCOs */
    pmgr1 = map(fd, 0x3f101000);      /* power states */
    acs = map(fd, 0x341a0000);
    i2s = map(fd, 0x34191000);
    if ((sig = sigsetjmp(jb, 1))) { printf("bus error (signal %d)\n", sig); return 1; }

    v = pmgr1[0xa4 / 4];
    pmgr1[0xa4 / 4] = (v & ~0x10fu) | 0xf;
    t0 = now_us();
    while (((pmgr1[0xa4 / 4] >> 4) & 0xf) != (pmgr1[0xa4 / 4] & 0xf) && now_us() - t0 < 100000)
        ;
    printf("1. I2S1 power state %08x -> %08x\n", v, pmgr1[0xa4 / 4]);

    if (acsv >= 0)
        acs[0x14 / 4] = acsv;
    printf("2. ACS +0x14 = %08x\n", acs[0x14 / 4]);

    pmgr[0x58 / 4] = audioclk;
    printf("3. AUDIO-CLK = %08x\n", pmgr[0x58 / 4]);

    volatile uint32_t *nco = &pmgr[0x120 / 4];
    nco[0] = 0x90000000;
    wait_clear(&nco[0], 1u << 30, "NCO ctrl bit 30");
    nco[1] = 2 * hz;
    wait_clear(&nco[0], 1u << 9, "NCO bit 9 (inc1)");
    nco[2] = 2 * hz - fref;
    wait_clear(&nco[0], 1u << 9, "NCO bit 9 (inc2)");
    nco[0] = 0x90000400;
    wait_clear(&nco[0], 1u << 9, "NCO bit 9 (0x400)");
    nco[0] = 0x90000c00;
    wait_clear(&nco[0], 1u << 9, "NCO bit 9 (0xc00)");
    printf("4. NCO1 %u Hz (Fref guess %u): %08x %08x %08x %08x\n", hz, fref, nco[0], nco[1], nco[2], nco[3]);

    i2s[0x3c / 4] = 1;
    i2s[0x04 / 4] = txcon;
    i2s[0x30 / 4] = 0;
    i2s[0x08 / 4] = 0;
    i2s[0x34 / 4] = 0;
    i2s[0x00 / 4] = clkcon;
    i2s[0x08 / 4] = txcom;
    printf("5. I2S1: CLKCON %08x TXCON %08x TXCOM %08x STATUS %08x\n",
           i2s[0], i2s[1], i2s[2], i2s[0x3c / 4]);

    last = i2s[0x3c / 4];
    printf("6. filling %d words; STATUS before %08x\n", words, last);
    for (i = 0; i < words; i++) {
        i2s[0x10 / 4] = 0;
        v = i2s[0x3c / 4];
        if (v != last) { printf("   after word %2d: STATUS %08x\n", i + 1, v); last = v; }
    }
    t0 = now_us();
    n = 0;
    while (now_us() - t0 < 20000 && n < 12) {
        v = i2s[0x3c / 4];
        if (v != last) { printf("   +%6llu us: STATUS %08x\n", (unsigned long long)(now_us() - t0), v); last = v; n++; }
    }
    printf("   end: STATUS %08x, %d changes\n", i2s[0x3c / 4], n);
    return 0;
}
