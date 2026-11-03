/* audmmio -- a small MMIO script runner for the audio bring-up.
 *
 *   audmmio 'w 3f100058 80000001; p 3f100058 40000000 0; r 34191000 16'
 *   audmmio < script
 *
 * Research tool for docs/research/p105-audio.md, not part of the image.
 * Commands (hex numbers, ';' or newlines between them):
 *   r ADDR [N]            read N words (default 1) and print them
 *   w ADDR VAL            write a word
 *   m ADDR MASK VAL       read-modify-write: (old & ~MASK) | VAL
 *   p ADDR MASK VAL [US]  poll until (word & MASK) == VAL, at most US
 *                         microseconds (default 100000); prints how long
 *   d US                  busy-wait US microseconds
 *   f ADDR N VAL          write VAL to ADDR N times (a FIFO)
 *   t ADDR US [MAX]       sample ADDR for US microseconds, print each change
 *                         with its time (at most MAX changes, default 32)
 *   c ADDR US             count changes of ADDR over US microseconds, and
 *                         how many samples were taken
 *   q ADDR FRAMES HZ AMP  write a square wave of HZ into memory at ADDR:
 *                         FRAMES stereo frames of two 32-bit words, 48 kHz
 *   s ADDR STAT MASK N HZ US
 *                         PIO tone: keep the FIFO at ADDR fed with a square
 *                         wave of HZ (32-bit stereo frames, 48 kHz assumed)
 *                         for US microseconds, writing whenever
 *                         (STAT & MASK) shifted down is below N
 * Bus errors are caught, as in /bin/peek.
 */
#include <fcntl.h>
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

static sigjmp_buf jb;
static void fault(int sig) { siglongjmp(jb, sig); }
static int fd;

#define NMAPS 32
static struct { uint32_t base; volatile uint32_t *p; } maps[NMAPS];

static volatile uint32_t *reg(uint32_t a)
{
    uint32_t base = a & ~0xfffu;
    int i;
    for (i = 0; i < NMAPS && maps[i].p; i++)
        if (maps[i].base == base)
            return maps[i].p + (a & 0xfff) / 4;
    if (i == NMAPS) { fprintf(stderr, "too many pages\n"); exit(1); }
    void *p = mmap(0, 0x1000, PROT_READ | PROT_WRITE, MAP_SHARED, fd, base);
    if (p == MAP_FAILED) { perror("mmap"); exit(1); }
    maps[i].base = base;
    maps[i].p = p;
    return maps[i].p + (a & 0xfff) / 4;
}

static uint64_t now_us(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000;
}

static uint32_t num(char **s)
{
    char *t = strtok_r(NULL, " \t", s);
    if (!t) { fprintf(stderr, "missing argument\n"); exit(1); }
    return strtoul(t, 0, 16);
}

static uint32_t opt(char **s, uint32_t def)
{
    char *t = strtok_r(NULL, " \t", s);
    return t ? strtoul(t, 0, 16) : def;
}

static void run(char *line)
{
    char *sp, *cmd = strtok_r(line, " \t", &sp);
    uint32_t a, b, c, n, i, v, last;
    uint64_t t0;

    if (!cmd || *cmd == '#')
        return;
    switch (*cmd) {
    case 'r':
        a = num(&sp);
        n = opt(&sp, 1);
        for (i = 0; i < n; i++) {
            if (i % 4 == 0)
                printf("%s%08x:", i ? "\n" : "", a + 4 * i);
            printf(" %08x", *reg(a + 4 * i));
        }
        printf("\n");
        break;
    case 'w':
        a = num(&sp);
        *reg(a) = num(&sp);
        break;
    case 'm':
        a = num(&sp);
        b = num(&sp);
        c = num(&sp);
        *reg(a) = (*reg(a) & ~b) | c;
        break;
    case 'p':
        a = num(&sp);
        b = num(&sp);
        c = num(&sp);
        n = opt(&sp, 100000);
        t0 = now_us();
        while (((v = *reg(a)) & b) != c)
            if (now_us() - t0 > n) {
                printf("poll %08x: still %08x after %u us\n", a, v, n);
                return;
            }
        printf("poll %08x: %08x after %llu us\n", a, v, (unsigned long long)(now_us() - t0));
        break;
    case 'd':
        n = num(&sp);
        t0 = now_us();
        while (now_us() - t0 < n)
            ;
        break;
    case 'f':
        a = num(&sp);
        n = num(&sp);
        v = num(&sp);
        for (i = 0; i < n; i++)
            *reg(a) = v;
        break;
    case 't':
        a = num(&sp);
        n = num(&sp);
        c = opt(&sp, 32);
        last = *reg(a);
        printf("t %08x: %08x at start\n", a, last);
        t0 = now_us();
        i = 0;
        while (now_us() - t0 < n && i < c)
            if ((v = *reg(a)) != last) {
                printf("  +%7llu us: %08x\n", (unsigned long long)(now_us() - t0), v);
                last = v;
                i++;
            }
        printf("t %08x: %08x at end, %u changes\n", a, *reg(a), i);
        break;
    case 'c': {
        uint64_t samples = 0, changes = 0;
        a = num(&sp);
        n = num(&sp);
        last = *reg(a);
        t0 = now_us();
        while (now_us() - t0 < n) {
            v = *reg(a);
            samples++;
            if (v != last) { changes++; last = v; }
        }
        printf("c %08x: %llu changes in %llu samples over %u us, last %08x\n", a,
               (unsigned long long)changes, (unsigned long long)samples, n, last);
        break;
    }
    case 'q': {
        uint32_t frames, hz, amp, phase = 0;
        a = num(&sp);
        frames = num(&sp);
        hz = num(&sp);
        amp = num(&sp);
        for (i = 0; i < frames; i++) {
            v = phase < 24000 ? amp : -amp;
            *reg(a + 8 * i) = v;
            *reg(a + 8 * i + 4) = v;
            phase += hz;
            if (phase >= 48000)
                phase -= 48000;
        }
        break;
    }
    case 's': {
        uint32_t stat, mask, lim, hz, us, sh = 0, frames = 0, phase = 0;
        uint64_t writes = 0;
        a = num(&sp);
        stat = num(&sp);
        mask = num(&sp);
        lim = num(&sp);
        hz = num(&sp);
        us = num(&sp);
        while (!((mask >> sh) & 1))
            sh++;
        t0 = now_us();
        while (now_us() - t0 < us) {
            if (((*reg(stat) & mask) >> sh) < lim) {
                int32_t s = phase < 24000 ? 0x10000000 : -0x10000000;
                *reg(a) = s;
                *reg(a) = s;
                writes += 2;
                frames++;
                phase += hz;
                if (phase >= 48000)
                    phase -= 48000;
            }
        }
        printf("s: %llu words, %u frames in %u us = %llu frames/s\n",
               (unsigned long long)writes, frames, us,
               (unsigned long long)frames * 1000000 / us);
        break;
    }
    default:
        fprintf(stderr, "unknown command %s\n", cmd);
        exit(1);
    }
}

int main(int argc, char **argv)
{
    char buf[4096], *line, *sp;
    int sig, i;

    fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd < 0) { perror("/dev/mem"); return 1; }
    signal(SIGBUS, fault);
    signal(SIGSEGV, fault);
    if ((sig = sigsetjmp(jb, 1))) {
        printf("bus error (signal %d)\n", sig);
        return 1;
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (argc > 1) {
        for (i = 1; i < argc; i++)
            for (line = strtok_r(argv[i], ";\n", &sp); line; line = strtok_r(NULL, ";\n", &sp))
                run(line);
        return 0;
    }
    while (fgets(buf, sizeof buf, stdin))
        for (line = strtok_r(buf, ";\n", &sp); line; line = strtok_r(NULL, ";\n", &sp))
            run(line);
    return 0;
}
