/* cpudbg -- a halting debugger for the A5's Cortex-A9 cores, run on the
 * other core, through the memory-mapped ARMv7 debug registers.
 *
 * The ADT's arm-io/trace node gives the funnel (0x3d203000), the ETB
 * (0x3d204000) and the two PTMs (0x3d23c000/0x3d23d000); the cores' own
 * debug blocks sit where the usual A9 layout puts them: CPU0 0x3d230000,
 * CPU1 0x3d232000 (part 0xc09; PMUs at +0x1000, CTIs at 0x3d238000/9000).
 * DBGAUTHSTATUS reads 0xff: invasive debug, secure too, is enabled.
 *
 *   cpudbg CPU status          DIDR, DSCR, PRSR and a few PC samples
 *   cpudbg CPU halt            unlock, enable halting debug, halt
 *   cpudbg CPU regs            (halted) r0-r14, pc, cpsr, spsr, CP15 state
 *   cpudbg CPU xlate VA...     (halted) the core's own MMU: ATS1CPR -> PAR
 *   cpudbg CPU rd VA [N]       (halted) N words read by the core at VA
 *   cpudbg CPU park ADDR       (halted) MMU and caches off, pc = ADDR, go
 *   cpudbg CPU resume          (halted) restart where it stopped
 *   cpudbg CPU wreset          DBGPRCR.CWRR: ask for a warm reset
 *   cpudbg CPU sev             send an event (wakes a WFE), from this core
 *
 * regs and rd clobber r0/r1 of the halted core.  Build:
 *   arm-linux-gnueabihf-gcc -static -Os -o cpudbg tools/cpudbg.c
 */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define DBG_BASE(cpu)   (0x3d230000u + (cpu) * 0x2000u)

#define DIDR    0x000
#define DTRRX   0x080
#define ITR     0x084   /* write; reads are DBGPCSR in v7.0 */
#define PCSR    0x084
#define DSCR    0x088
#define DTRTX   0x08c
#define DRCR    0x090
#define OSLSR   0x304
#define PRCR    0x310
#define PRSR    0x314
#define LAR     0xfb0
#define LSR     0xfb4
#define AUTH    0xfb8

#define DSCR_HALTED     (1u << 0)
#define DSCR_RESTARTED  (1u << 1)
#define DSCR_STICKY     (7u << 6)       /* SDABORT, ADABORT, SUNDABORT */
#define DSCR_ITREN      (1u << 13)
#define DSCR_HDBGEN     (1u << 14)
#define DSCR_INSTRCOMPL (1u << 24)
#define DSCR_TXFULL     (1u << 29)

static volatile uint32_t *dbg;

static uint32_t rd(unsigned off) { return dbg[off / 4]; }
static void wr(unsigned off, uint32_t v) { dbg[off / 4] = v; }

static int wait_set(unsigned off, uint32_t bits, const char *what)
{
    int i;

    for (i = 0; i < 100000; i++)
        if ((rd(off) & bits) == bits)
            return 0;
    fprintf(stderr, "timeout waiting for %s (DSCR %08x)\n", what, rd(DSCR));
    return -1;
}

/* One instruction on the halted core. */
static int exec(uint32_t insn)
{
    if (wait_set(DSCR, DSCR_INSTRCOMPL, "InstrCompl"))
        return -1;
    wr(ITR, insn);
    if (wait_set(DSCR, DSCR_INSTRCOMPL, "InstrCompl"))
        return -1;
    if (rd(DSCR) & DSCR_STICKY) {
        fprintf(stderr, "insn %08x: sticky abort, DSCR %08x\n", insn, rd(DSCR));
        wr(DRCR, 1u << 2);              /* CSE: clear sticky exceptions */
        return -1;
    }
    return 0;
}

/* rN out through DBGDTRTX: mcr p14, 0, rN, c0, c5, 0 */
static int get_reg(int n, uint32_t *v)
{
    if (exec(0xee000e15 | (n << 12)) || wait_set(DSCR, DSCR_TXFULL, "TXfull"))
        return -1;
    *v = rd(DTRTX);
    return 0;
}

/* rN in through DBGDTRRX: mrc p14, 0, rN, c0, c5, 0 */
static int set_reg(int n, uint32_t v)
{
    wr(DTRRX, v);
    return exec(0xee100e15 | (n << 12));
}

/* Any instruction whose result lands in r0, then r0 out. */
static int via_r0(uint32_t insn, uint32_t *v)
{
    return exec(insn) || get_reg(0, v) ? -1 : 0;
}

#define MRC15(op1, crn, crm, op2) \
    (0xee100f10 | (op1) << 21 | (crn) << 16 | (op2) << 5 | (crm))
#define MCR15(op1, crn, crm, op2) \
    (0xee000f10 | (op1) << 21 | (crn) << 16 | (op2) << 5 | (crm))

static const struct { const char *name; uint32_t insn; } cp15[] = {
    { "MIDR",   MRC15(0, 0, 0, 0) },
    { "MPIDR",  MRC15(0, 0, 0, 5) },
    { "SCTLR",  MRC15(0, 1, 0, 0) },
    { "ACTLR",  MRC15(0, 1, 0, 1) },
    { "TTBR0",  MRC15(0, 2, 0, 0) },
    { "TTBR1",  MRC15(0, 2, 0, 1) },
    { "TTBCR",  MRC15(0, 2, 0, 2) },
    { "DACR",   MRC15(0, 3, 0, 0) },
    { "DFSR",   MRC15(0, 5, 0, 0) },
    { "IFSR",   MRC15(0, 5, 0, 1) },
    { "DFAR",   MRC15(0, 6, 0, 0) },
    { "IFAR",   MRC15(0, 6, 0, 2) },
    { "PAR",    MRC15(0, 7, 4, 0) },
    { "PRRR",   MRC15(0, 10, 2, 0) },
    { "NMRR",   MRC15(0, 10, 2, 1) },
    { "VBAR",   MRC15(0, 12, 0, 0) },
    { "CTXIDR", MRC15(0, 13, 0, 1) },
    { "PWRCTL", MRC15(0, 15, 0, 0) },
    { "DIAG",   MRC15(0, 15, 0, 1) },
    { "CBAR",   MRC15(4, 15, 0, 0) },
};

static int halted(void)
{
    if (rd(DSCR) & DSCR_HALTED)
        return 1;
    fprintf(stderr, "core not halted (DSCR %08x)\n", rd(DSCR));
    return 0;
}

static void status(void)
{
    int i;

    printf("DIDR %08x DSCR %08x PRSR %08x OSLSR %08x LSR %08x AUTH %08x\n",
           rd(DIDR), rd(DSCR), rd(PRSR), rd(OSLSR), rd(LSR), rd(AUTH));
    if (rd(DSCR) & DSCR_HALTED)
        return;                 /* PCSR reads 0xffffffff while halted */
    printf("PCSR");
    for (i = 0; i < 8; i++)
        printf(" %08x", rd(PCSR));
    printf("\n");
}

static int halt(void)
{
    wr(LAR, 0xc5acce55);
    wr(DSCR, (rd(DSCR) & ~DSCR_STICKY) | DSCR_HDBGEN | DSCR_ITREN);
    if (rd(DSCR) & DSCR_HALTED) {
        printf("already halted\n");
        return 0;
    }
    wr(DRCR, 1u << 0);                  /* HRQ */
    if (wait_set(DSCR, DSCR_HALTED, "HALTED"))
        return -1;
    printf("halted, DSCR %08x (entry %u)\n", rd(DSCR), (rd(DSCR) >> 2) & 0xf);
    return 0;
}

static void regs(void)
{
    uint32_t v[15], x;
    unsigned i;

    for (i = 0; i < 15; i++)
        if (get_reg(i, &v[i]))
            return;
    for (i = 0; i < 15; i++)
        printf("r%-2u %08x%s", i, v[i], i % 4 == 3 || i == 14 ? "\n" : "  ");
    if (!via_r0(0xe1a0000f, &x))            /* mov r0, pc */
        printf("pc  %08x (read as +8: halted at %08x)\n", x, x - 8);
    if (!via_r0(0xe10f0000, &x))            /* mrs r0, cpsr */
        printf("cpsr %08x (mode %02x)\n", x, x & 0x1f);
    if (!via_r0(0xe14f0000, &x))            /* mrs r0, spsr */
        printf("spsr %08x\n", x);
    for (i = 0; i < sizeof(cp15) / sizeof(cp15[0]); i++)
        if (!via_r0(cp15[i].insn, &x))
            printf("%-7s %08x\n", cp15[i].name, x);
    set_reg(0, v[0]);
}

static void xlate(uint32_t va)
{
    uint32_t par;

    if (set_reg(0, va) || exec(MCR15(0, 7, 8, 0)) ||    /* ATS1CPR */
        exec(0xf57ff06f) ||                              /* isb */
        via_r0(MRC15(0, 7, 4, 0), &par))
        return;
    if (par & 1)
        printf("VA %08x: fault, PAR %08x (FS %02x)\n", va, par,
               (par >> 1) & 0x1f);
    else
        printf("VA %08x -> PA %08x, PAR %08x (SS %u, NS %u, SH %u, "
               "inner %u, outer %u)\n", va, (par & ~0xfffu) | (va & 0xfff),
               par, (par >> 1) & 1, (par >> 9) & 1, (par >> 7) & 1,
               (par >> 4) & 7, (par >> 2) & 3);
}

static void rdmem(uint32_t va, unsigned n)
{
    uint32_t x;
    unsigned i;

    for (i = 0; i < n; i++) {
        if (set_reg(1, va + i * 4) || via_r0(0xe5910000, &x)) {  /* ldr r0, [r1] */
            printf("%08x: abort\n", va + i * 4);
            return;
        }
        printf("%08x: %08x\n", va + i * 4, x);
    }
}

/* MMU and caches off, then restart at addr (a WFI loop, say). */
static void park(uint32_t addr)
{
    uint32_t sctlr;

    if (via_r0(MRC15(0, 1, 0, 0), &sctlr))
        return;
    if (set_reg(0, sctlr & ~0x1805u) || exec(MCR15(0, 1, 0, 0)) ||
        exec(0xf57ff06f) ||                                     /* isb */
        set_reg(0, addr) || exec(0xe1a0f000))                   /* mov pc, r0 */
        return;
    wr(DSCR, rd(DSCR) & ~(DSCR_ITREN | DSCR_HDBGEN));
    wr(DRCR, (1u << 2) | (1u << 1));                            /* CSE, RRQ */
    if (!wait_set(DSCR, DSCR_RESTARTED, "RESTARTED"))
        printf("restarted at %08x, SCTLR was %08x\n", addr, sctlr);
}

int main(int argc, char **argv)
{
    int fd, cpu;
    const char *cmd;

    if (argc < 3) {
        fprintf(stderr, "usage: cpudbg CPU status|halt|regs|xlate VA..|"
                "rd VA [N]|park ADDR|resume|wreset\n");
        return 2;
    }
    cpu = atoi(argv[1]);
    cmd = argv[2];
    fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd < 0) {
        perror("/dev/mem");
        return 1;
    }
    dbg = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, DBG_BASE(cpu));
    if (dbg == MAP_FAILED) {
        perror("mmap");
        return 1;
    }

    if (!strcmp(cmd, "status")) {
        status();
    } else if (!strcmp(cmd, "sev")) {
        __asm__ volatile("dsb; sev" ::: "memory");
    } else if (!strcmp(cmd, "halt")) {
        return halt() ? 1 : 0;
    } else if (!strcmp(cmd, "wreset")) {
        wr(LAR, 0xc5acce55);
        wr(PRCR, rd(PRCR) | (1u << 1));
        usleep(1000);
        status();
    } else if (!halted()) {
        return 1;
    } else if (!strcmp(cmd, "regs")) {
        regs();
    } else if (!strcmp(cmd, "xlate")) {
        int i;
        for (i = 3; i < argc; i++)
            xlate(strtoul(argv[i], NULL, 0));
    } else if (!strcmp(cmd, "rd") && argc > 3) {
        rdmem(strtoul(argv[3], NULL, 0), argc > 4 ? strtoul(argv[4], NULL, 0) : 1);
    } else if (!strcmp(cmd, "park") && argc > 3) {
        park(strtoul(argv[3], NULL, 0));
    } else if (!strcmp(cmd, "resume")) {
        wr(DRCR, (1u << 2) | (1u << 1));
        wait_set(DSCR, DSCR_RESTARTED, "RESTARTED");
    } else {
        fprintf(stderr, "unknown command %s\n", cmd);
        return 2;
    }
    return 0;
}
