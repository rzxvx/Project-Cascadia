/* k93-sdio-probe -- the iPad 2's SDIO host (SDHC at 0x30000000) driven by
 * hand, for the bring-up of drivers/mmc/host/sdhci-apple-s5l.c.
 *
 * Boot with sdhci_apple_s5l.hold=1 (the driver powers the block and
 * registers no MMC host), raise the BCM4329's REG_ON (PMU GPIO 3:
 * i2cset -y -f 0 0x3c 0x64 0x0b), then:
 *
 *   k93-sdio-probe [-d DIV] [-x] [-f [-D DIV] [-n BLOCKS] [-B BLKSZ] [-A ADDR] [-1]]
 *     -d  the SD clock divisor field (bits 15:8 of clock control, SDHCI v2:
 *         base / (2 * DIV)); default 0x40, 48 MHz / 128 = 375 kHz
 *     -x  also test what a CMD-line reset does to the clock afterwards
 *     -f  the PIO buffer test: 4-bit bus, F1 on, ALP clock, backplane window
 *         0 (the 4329's SOCRAM), then CMD53 block reads and writes moved one
 *         word at a time, logging the buffer-ready bits after every word.
 *         The chip's RAM is scribbled on: power-cycle it (REG_ON) after.
 *     -D  the divisor field for the -f transfers (default 1: 24 MHz)
 *     -n  blocks per CMD53 (default 2), -B block size (default 64),
 *     -A  F1 address (default 0x9000: window offset 0x1000, 2/4-byte access)
 *     -1  1-bit bus instead of 4
 *     -b  byte mode: one CMD53 of BLKSZ bytes instead of blocks
 *     -W  the backplane window (default 0: SOCRAM), -r reads only
 *     -c  instead of -f: CMD53 reads of the common CIS on function 0, in
 *         byte mode (4, 64) and block mode (1, 2 blocks of 64)
 *     -s N instead of -f: 4-byte chip ID reads, with N reads of the data
 *         port in between, then a CMD+DAT reset, then a reset all: does
 *         the buffer keep words that no transfer took out?
 *     -m N instead of -f: SOCRAM as brcmfmac gets to it (force ALP, SOCRAM
 *         core reset), 4-byte writes, then writes of 1, 2, 4 .. N blocks
 *
 * Every step prints the registers that matter; commands are polled (the
 * signal-enable register stays 0, so no interrupt reaches the kernel).  The
 * clock is set the way the IOP firmware does it: read-modify-write of clock
 * control, internal clock first, then the card clock with what reads back.
 */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

static volatile uint8_t *b;
#define R8(o)       (*(volatile uint8_t *)(b + (o)))
#define R16(o)      (*(volatile uint16_t *)(b + (o)))
#define R32(o)      (*(volatile uint32_t *)(b + (o)))
#define W8(o, v)    (*(volatile uint8_t *)(b + (o)) = (v))
#define W16(o, v)   (*(volatile uint16_t *)(b + (o)) = (v))
#define W32(o, v)   (*(volatile uint32_t *)(b + (o)) = (v))

static void dump(const char *tag)
{
    printf("%-24s present %08x host %02x pwr %02x clk %04x tmo %02x rst %02x int %08x en %08x resp %08x %08x\n",
           tag, R32(0x24), R8(0x28), R8(0x29), R16(0x2c), R8(0x2e), R8(0x2f), R32(0x30), R32(0x34),
           R32(0x10), R32(0x14));
}

static void msleep(int ms) { usleep(ms * 1000); }

static int clock_on(unsigned div)
{
    uint16_t v;
    int i;

    v = R16(0x2c);
    W16(0x2c, v & ~4);                               /* card clock off */
    v = R16(0x2c);
    W16(0x2c, (v & 0x00fb) | (div << 8) | 1);        /* divider, internal clock */
    for (i = 0; i < 1000; i++) {
        v = R16(0x2c);
        if (v & 2)
            break;
        usleep(10);
    }
    if (!(v & 2)) {
        dump("clock: never stable");
        return -1;
    }
    W16(0x2c, v | 4);                                /* card clock, bit 1 as read */
    dump("clock on");
    return 0;
}

/* one command; flags as SDHCI's command register low byte (response type,
 * CRC/index checks).  Returns the interrupt status it ended with. */
static uint32_t cmd(const char *name, int idx, uint32_t arg, int flags)
{
    uint32_t s = 0;
    int i;
    char tag[40];

    for (i = 0; i < 1000 && (R32(0x24) & 3); i++)
        usleep(10);
    W32(0x30, 0xffffffff);
    W32(0x08, arg);
    W16(0x0e, idx << 8 | flags);
    for (i = 0; i < 2000; i++) {                     /* 200 ms */
        s = R32(0x30);
        if (s & 0xffff8001)
            break;
        usleep(100);
    }
    snprintf(tag, sizeof(tag), "%s %s", name, (s & 1) && !(s & 0x8000) ? "OK" : (s ? "ERR" : "NO IRQ"));
    dump(tag);
    W32(0x30, s);
    if (s & 0x8000) {                                /* error: reset the CMD line */
        W8(0x2f, 2);
        for (i = 0; i < 1000 && (R8(0x2f) & 2); i++)
            usleep(10);
        dump("  after CMD reset");
    }
    return s;
}

/* CMD52 read of function 0, quietly: 0 and the byte in *v, or -1 */
static int cmd52q(uint32_t addr, int *v)
{
    uint32_t s = 0;
    int i;

    for (i = 0; i < 1000 && (R32(0x24) & 3); i++)
        usleep(10);
    W32(0x30, 0xffffffff);
    W32(0x08, (addr & 0x1ffff) << 9);
    W16(0x0e, 52 << 8 | 0x1a);
    for (i = 0; i < 2000; i++) {
        s = R32(0x30);
        if (s & 0xffff8001)
            break;
        usleep(50);
    }
    W32(0x30, s);
    if (!(s & 1) || (s & 0x8000)) {
        static int shown;
        if (shown++ < 4)
            printf("\n  [CMD52 %05x failed: int %08x present %08x clk %04x resp %08x]",
                   addr, s, R32(0x24), R16(0x2c), R32(0x10));
        W8(0x2f, 2);
        for (i = 0; i < 1000 && (R8(0x2f) & 2); i++)
            usleep(10);
        return -1;
    }
    *v = R32(0x10) & 0xff;
    return 0;
}

/* CMD52 to any function: read (val < 0) or write; the byte read back, or -1 */
static int cmd52(int fn, uint32_t addr, int val)
{
    uint32_t s = 0, arg;
    int i;

    arg = (val >= 0 ? 1u << 31 | (val & 0xff) : 0) | (uint32_t)fn << 28 | (addr & 0x1ffff) << 9;
    for (i = 0; i < 1000 && (R32(0x24) & 3); i++)
        usleep(10);
    W32(0x30, 0xffffffff);
    W32(0x08, arg);
    W16(0x0e, 52 << 8 | 0x1a);
    for (i = 0; i < 2000; i++) {
        s = R32(0x30);
        if (s & 0xffff8001)
            break;
        usleep(50);
    }
    W32(0x30, s);
    if (!(s & 1) || (s & 0x8000)) {
        printf("  CMD52 f%d %05x %s failed: int %08x present %08x\n", fn, addr,
               val >= 0 ? "write" : "read", s, R32(0x24));
        W8(0x2f, 2);
        for (i = 0; i < 1000 && (R8(0x2f) & 2); i++)
            usleep(10);
        return -1;
    }
    return R32(0x10) & 0xff;
}

static void dat_reset(void)
{
    int i;

    W8(0x2f, 6);                                     /* CMD and DAT lines */
    for (i = 0; i < 1000 && (R8(0x2f) & 6); i++)
        usleep(10);
}

/*
 * One CMD53 in block mode (incrementing address), its data moved by hand one
 * 32-bit word at a time through the buffer data port (0x20).  Before every
 * word: wait for Buffer Write Enable (present bit 10) or Buffer Read Enable
 * (bit 11).  Logged per word: present state before and after, the interrupt
 * status (bits 4/5 are the buffer-ready interrupts, cleared once seen) and
 * the block count register, which counts down the blocks still to go.
 */
static int bytemode;
static int dmabit;                                   /* transfer mode bit 0, as the IOP sets it */
static int errdrain;                                 /* data port reads after an error, before the reset */

static int cmd53_pio(int write, int fn, uint32_t addr, int blocks, int blksz, uint32_t *buf, int log)
{
    uint32_t arg, s = 0, ps = 0, ps2, rdy = write ? 0x400 : 0x800;
    int words = blocks * blksz / 4, n, t, i;

    if (bytemode)                                    /* one transfer of blksz bytes */
        arg = (write ? 1u << 31 : 0) | (uint32_t)fn << 28 | 1u << 26 | (addr & 0x1ffff) << 9 | (blksz & 0x1ff);
    else
        arg = (write ? 1u << 31 : 0) | (uint32_t)fn << 28 | 1u << 27 | 1u << 26 | (addr & 0x1ffff) << 9 | (blocks & 0x1ff);
    for (i = 0; i < 1000 && (R32(0x24) & 3); i++)
        usleep(10);
    W32(0x30, 0xffffffff);
    W16(0x04, blksz);
    W16(0x06, blocks);
    W32(0x08, arg);
    W16(0x0c, (write ? 0 : 0x10) | (blocks > 1 ? 0x20 : 0) | 0x02 | (dmabit ? 1 : 0));
    W16(0x0e, 53 << 8 | 0x3a);                       /* R5, CRC + index check, data */
    for (i = 0; i < 2000; i++) {
        s = R32(0x30);
        if (s & 0xffff8001)
            break;
        usleep(50);
    }
    printf("CMD53 %s f%d %05x %s %d x %d (arg %08x): int %08x present %08x resp %08x\n", write ? "write" : "read",
           fn, addr, bytemode ? "byte mode" : "block mode", blocks, blksz, arg, s, R32(0x24), R32(0x10));
    if (!(s & 1) || (s & 0x8000)) {
        dat_reset();
        return -1;
    }
    W32(0x30, 1);
    for (n = 0; n < words; n++) {
        for (t = 0; t < 20000; t++) {
            ps = R32(0x24);
            if (ps & rdy)
                break;
            if (R32(0x30) & 0x8000)
                break;
            if (t > 200)
                usleep(20);
        }
        if (!(ps & rdy)) {
            printf("  word %d: the buffer never got ready: present %08x int %08x blocks left %u\n",
                   n, ps, R32(0x30), R16(0x06));
            if (errdrain) {
                printf("  error drain:");
                for (i = 0; i < errdrain; i++) {
                    uint32_t pv = R32(0x24), dv = R32(0x20);
                    if (i < 6)
                        printf(" %08x(%03x)", dv, (pv >> 8) & 0xfff);
                }
                printf(" .. present now %08x\n", R32(0x24));
            }
            dat_reset();
            return -1;
        }
        if (write)
            W32(0x20, buf[n]);
        else
            buf[n] = R32(0x20);
        ps2 = R32(0x24);
        s = R32(0x30);
        if (log)
            printf("  w%03d %s %08x  present %08x -> %08x  int %08x  blocks left %u\n", n,
                   write ? "wrote" : "read ", buf[n], ps, ps2, s, R16(0x06));
        if (s & 0x30)
            W32(0x30, s & 0x30);
    }
    for (i = 0; i < 20000; i++) {
        s = R32(0x30);
        if (s & 0xffff8002)
            break;
        usleep(50);
    }
    printf("  end: int %08x present %08x blocks left %u\n", s, R32(0x24), R16(0x06));
    W32(0x30, s);
    if (!(s & 2) || (s & 0x8000)) {
        dat_reset();
        return -1;
    }
    return 0;
}

static uint32_t window;
static int readonly;

/* reads of the buffer data port with no transfer behind them */
static void drain(int n)
{
    int i;

    printf("  drain:");
    for (i = 0; i < n; i++) {
        uint32_t ps = R32(0x24), v = R32(0x20);
        printf("%s%08x(%03x)", i % 6 ? " " : "\n   ", v, (ps >> 8) & 0xfff);
    }
    printf("\n");
}

static uint32_t cur_window = 0xffffffff;

static void quiet_drain(int n)
{
    while (n--)
        (void)R32(0x20);
}

static int set_window(uint32_t addr)
{
    uint32_t w = addr & 0xffff8000;

    if (w == cur_window)
        return 0;
    if (cmd52(1, 0x1000a, (w >> 8) & 0x80) < 0 || cmd52(1, 0x1000b, (w >> 16) & 0xff) < 0 ||
        cmd52(1, 0x1000c, w >> 24) < 0)
        return -1;
    cur_window = w;
    return 0;
}

/* one 32-bit backplane access: F1, 2/4-byte window, CMD53 byte mode */
static int bp_rw(int write, uint32_t addr, uint32_t *v)
{
    int sv = bytemode, ret;

    if (set_window(addr))
        return -1;
    bytemode = 1;
    quiet_drain(8);
    ret = cmd53_pio(write, 1, 0x8000 | (addr & 0x7fff), 1, 4, v, 0);
    bytemode = sv;
    return ret;
}

static uint32_t bp_r(uint32_t addr)
{
    uint32_t v = 0xdeadbeef;

    if (bp_rw(0, addr, &v))
        printf("  [read %08x failed]\n", addr);
    return v;
}

static void bp_w(uint32_t addr, uint32_t v)
{
    if (bp_rw(1, addr, &v))
        printf("  [write %08x failed]\n", addr);
}

/* SOCRAM the way brcmfmac gets to it (chip.c, SB backplane, BCM4329: ARM
 * CM3 at 0x18002000, SOCRAM at 0x18003000), then 4-byte and block writes */
static void socram_test(unsigned div, int nblocks)
{
    static uint32_t out[512 * 16];
    uint32_t v;
    int i, k, bad;

    v = cmd52(0, 0x07, -1);
    cmd52(0, 0x07, (v & ~3) | 2);
    W8(0x28, R8(0x28) | 0x02);
    if (clock_on(div))
        return;
    cmd52(0, 0x110, 64);
    cmd52(0, 0x111, 0);
    cmd52(0, 0x02, 0x02);
    for (i = 0; i < 100 && !((v = cmd52(0, 0x03, -1)) & 2); i++)
        usleep(1000);
    /* flush the FIFO: draining only pops after a read transfer, so a
     * 4-byte read first, then the data port read out; twice over */
    for (k = 0; k < 3; k++) {
        (void)bp_r(0x18000000);
        quiet_drain(600);
    }
    printf("chip ID %08x, capabilities %08x, chip ID %08x\n", bp_r(0x18000000), bp_r(0x18000004),
           bp_r(0x18000000));
    cmd52(1, 0x1000e, 0x28);
    for (i = 0; i < 100 && !((v = cmd52(1, 0x1000e, -1)) & 0x40); i++)
        usleep(1000);
    cmd52(1, 0x1000e, 0x21);
    printf("chip clock CSR %02x -> %02x\n", v, cmd52(1, 0x1000e, -1));
    printf("ARM CM3: tmstatelow %08x tmstatehigh %08x idhigh %08x\n",
           bp_r(0x18002f98), bp_r(0x18002f9c), bp_r(0x18002ffc));
    printf("SOCRAM:  tmstatelow %08x tmstatehigh %08x idhigh %08x\n",
           bp_r(0x18003f98), bp_r(0x18003f9c), bp_r(0x18003ffc));
    /* SOCRAM: reset with the clock forced, then out of reset, clock on */
    bp_w(0x18003f98, 0x00030001);
    (void)bp_r(0x18003f98);
    usleep(10);
    bp_w(0x18003f98, 0x00030000);
    (void)bp_r(0x18003f98);
    usleep(10);
    bp_w(0x18003f98, 0x00010000);
    printf("SOCRAM after reset: tmstatelow %08x tmstatehigh %08x imstate %08x coreinfo %08x\n",
           bp_r(0x18003f98), bp_r(0x18003f9c), bp_r(0x18003f90), bp_r(0x18003000));
    printf("SOCRAM[0x1000..]: %08x %08x\n", bp_r(0x1000), bp_r(0x1004));
    bp_w(0x1000, 0x12345678);
    bp_w(0x1004, 0x9abcdef0);
    printf("after 4-byte writes: %08x %08x\n", bp_r(0x1000), bp_r(0x1004));

    for (k = 1; k <= nblocks; k *= 2) {
        uint32_t base = 0x2000 * k;

        for (i = 0; i < k * 16; i++)
            out[i] = 0x5a000000 | k << 16 | i;
        set_window(base);
        quiet_drain(600);
        bytemode = 0;
        printf("-- %d block(s) of 64 to %05x:\n", k, base);
        if (cmd53_pio(1, 1, 0x8000 | (base & 0x7fff), k, 64, out, 0)) {
            printf("   the write failed\n");
            quiet_drain(600);
            return;
        }
        bad = 0;
        for (i = 0; i < k * 16; i++) {
            v = bp_r(base + 4 * i);
            if (v != out[i] && bad++ < 4)
                printf("   word %d: %08x, wanted %08x\n", i, v, out[i]);
        }
        printf("   read back word by word: %d of %d differ\n", bad, k * 16);
    }
}

/* chip ID and capabilities, alternately: 0 in step, 1 one word late, 2 other */
static int lagcheck(const char *tag)
{
    uint32_t v[4];
    int lag;

    v[0] = bp_r(0x18000000);
    v[1] = bp_r(0x18000004);
    v[2] = bp_r(0x18000000);
    v[3] = bp_r(0x18000004);
    if (v[0] == 0x09944329 && v[1] == 0x3048010a && v[2] == 0x09944329 && v[3] == 0x3048010a)
        lag = 0;
    else if (v[1] == 0x09944329 && v[2] == 0x3048010a && v[3] == 0x09944329)
        lag = 1;
    else
        lag = 2;
    printf("  %-34s %08x %08x %08x %08x  -> %s\n", tag, v[0], v[1], v[2], v[3],
           lag == 0 ? "in step" : lag == 1 ? "ONE WORD LATE" : "garbage");
    return lag;
}

static void host_setup(unsigned div)
{
    W32(0x34, 0xffff00ff);
    W32(0x38, 0);
    W8(0x29, 0x0f);
    W8(0x2e, 0x0e);
    W8(0x28, 0x02);
    clock_on(div);
}

static void reset_bits(uint8_t m)
{
    int i;

    W8(0x2f, m);
    for (i = 0; i < 1000 && (R8(0x2f) & m); i++)
        usleep(10);
}

/* how many words late the FIFO is: chipcommon 0x00..0x1c read round twice;
 * the chip ID's place in the second round gives the lag (mod 8) */
static int lagmeasure(const char *tag)
{
    uint32_t v[16];
    int i, lag = -1;

    for (i = 0; i < 16; i++)
        v[i] = bp_r(0x18000000 + 4 * (i % 8));
    for (i = 8; i < 16; i++)
        if (v[i] == 0x09944329) {
            lag = i - 8;
            break;
        }
    printf("  %-30s lag %d  [", tag, lag);
    for (i = 8; i < 16; i++)
        printf("%s%08x", i > 8 ? " " : "", v[i]);
    printf("]\n");
    return lag;
}

/* a 4-byte CMD53 read whose data is left in the buffer: wait for Buffer
 * Read Enable, then CMD+DAT reset without touching the data port */
static void unread_read(int fn, uint32_t addr)
{
    uint32_t arg = (uint32_t)fn << 28 | 1u << 26 | (addr & 0x1ffff) << 9 | 4;
    int i;

    for (i = 0; i < 1000 && (R32(0x24) & 3); i++)
        usleep(10);
    W32(0x30, 0xffffffff);
    W16(0x04, 4);
    W16(0x06, 1);
    W32(0x08, arg);
    W16(0x0c, 0x12);
    W16(0x0e, 53 << 8 | 0x3a);
    for (i = 0; i < 2000 && !(R32(0x24) & 0x800) && !(R32(0x30) & 0x8000); i++)
        usleep(10);
    printf("  unread read: present %08x int %08x -> reset\n", R32(0x24), R32(0x30));
    W32(0x30, 0xffffffff);
    dat_reset();
}

/* CCCR 0x08..0x0b with a 4-byte CMD53 on function 0: 02 00 10 00 */
static uint32_t probe_cccr(void)
{
    uint32_t v = 0xdeadbeef;
    int sv = bytemode;

    bytemode = 1;
    cmd53_pio(0, 0, 0x08, 1, 4, &v, 0);
    bytemode = sv;
    return v;
}

static void lag_test(unsigned div)
{
    uint32_t v;
    int i;

    v = cmd52(0, 0x07, -1);
    cmd52(0, 0x07, (v & ~3) | 2);
    W8(0x28, R8(0x28) | 0x02);
    if (clock_on(div))
        return;
    cmd52(0, 0x110, 64);
    cmd52(0, 0x111, 0);
    cmd52(0, 0x02, 0x02);
    for (i = 0; i < 100 && !((v = cmd52(0, 0x03, -1)) & 2); i++)
        usleep(1000);
    lagmeasure("start");
    (void)bp_r(0x180000ac);
    lagmeasure("after an error");
    (void)bp_r(0x180000ac);
    lagmeasure("after another");
    quiet_drain(600);
    lagmeasure("drain 600");
    for (i = 0; i < 600; i++) {
        (void)R32(0x24);
        (void)R32(0x20);
    }
    lagmeasure("drain 600 with present reads");
    for (i = 0; i < 4; i++) {
        printf("  CCCR probe: %08x %08x\n", probe_cccr(), probe_cccr());
        unread_read(1, 0x8000);
        lagmeasure("after a read left unread");
    }
    (void)bp_r(0x180000ac);
    lagmeasure("after an error");
    printf("  CCCR probe: %08x %08x\n", probe_cccr(), probe_cccr());
    unread_read(0, 0x08);
    lagmeasure("after an unread CCCR read");
    printf("  CCCR probe: %08x %08x\n", probe_cccr(), probe_cccr());
}

/* a data error on purpose (chipcommon 0xac: this card ends its data with a
 * bad end bit), then one way of getting the FIFO back in step after it */
static void error_test(unsigned div)
{
    volatile uint32_t *pm;
    uint32_t v;
    int i, k, fd2;

    v = cmd52(0, 0x07, -1);
    cmd52(0, 0x07, (v & ~3) | 2);
    W8(0x28, R8(0x28) | 0x02);
    if (clock_on(div))
        return;
    cmd52(0, 0x110, 64);
    cmd52(0, 0x111, 0);
    cmd52(0, 0x02, 0x02);
    for (i = 0; i < 100 && !((v = cmd52(0, 0x03, -1)) & 2); i++)
        usleep(1000);
    lagcheck("start");
    for (k = 0; k < 7; k++) {
        static const char *what[7] = { "nothing", "drain 600", "4-byte read + drain 600",
                                       "DAT reset + drain", "reset all + drain",
                                       "PMGR power off/on + reset all", "reads until in step" };
        printf("-- error, then: %s\n", what[k]);
        v = bp_r(0x180000ac);
        printf("  chipcommon 0xac: %08x, int %08x present %08x\n", v, R32(0x30), R32(0x24));
        switch (k) {
        case 1: quiet_drain(600); break;
        case 2: (void)bp_r(0x18000000); quiet_drain(600); break;
        case 3: reset_bits(4); quiet_drain(600); break;
        case 4: reset_bits(1); host_setup(div); quiet_drain(600); break;
        case 5:
            fd2 = open("/dev/mem", O_RDWR | O_SYNC);
            pm = mmap(NULL, 0x2000, PROT_READ | PROT_WRITE, MAP_SHARED, fd2, 0x3f100000);
            v = pm[0x10b8 / 4];
            pm[0x10b8 / 4] = (v | 0x300) & ~15;
            for (i = 0; i < 1000 && (pm[0x10b8 / 4] & 0xf0); i++)
                usleep(10);
            printf("  PMGR SDIO-WIFI %08x -> off %08x", v, pm[0x10b8 / 4]);
            usleep(1000);
            pm[0x10b8 / 4] = (pm[0x10b8 / 4] & ~0x10f) | 0xf;
            for (i = 0; i < 1000 && (pm[0x10b8 / 4] & 0xf0) != 0xf0; i++)
                usleep(10);
            printf(" -> on %08x\n", pm[0x10b8 / 4]);
            reset_bits(1);
            host_setup(div);
            munmap((void *)pm, 0x2000);
            close(fd2);
            break;
        case 6:
            for (i = 0; i < 6 && lagcheck("  read"); i++)
                ;
            break;
        }
        lagcheck("after");
    }
}

/* is the buffer a FIFO that survives errors and resets?  4-byte chip ID
 * reads (chipcommon, 0x18000000) before and after draining the data port */
static void stale_test(unsigned div, int ndrain)
{
    uint32_t in[4];
    int v, i, k;

    v = cmd52(0, 0x07, -1);
    if (v < 0)
        return;
    cmd52(0, 0x07, (v & ~3) | 2);
    W8(0x28, R8(0x28) | 0x02);
    if (clock_on(div))
        return;
    cmd52(0, 0x110, 64);
    cmd52(0, 0x111, 0);
    cmd52(0, 0x02, 0x02);
    for (i = 0; i < 100 && !((v = cmd52(0, 0x03, -1)) & 2); i++)
        usleep(1000);
    cmd52(1, 0x1000a, 0);
    cmd52(1, 0x1000b, 0);
    cmd52(1, 0x1000c, 0x18);
    bytemode = 1;
    for (k = 0; k < 9; k++) {
        if (k == 3)
            drain(ndrain);
        if (k == 5) {
            dat_reset();
            printf("  (CMD+DAT reset)\n");
        }
        if (k == 7) {
            W8(0x2f, 1);
            for (i = 0; i < 1000 && (R8(0x2f) & 1); i++)
                usleep(10);
            printf("  (reset all -- and the setup again)\n");
            W32(0x34, 0xffff00ff);
            W32(0x38, 0);
            W8(0x29, 0x0f);
            W8(0x2e, 0x0e);
            W8(0x28, 0x02);
            clock_on(div);
        }
        in[0] = 0xdeadbeef;
        if (cmd53_pio(0, 1, 0x8000, 1, 4, in, 0) == 0)
            printf("  chip ID read %d: %08x  (present %08x)\n", k, in[0], R32(0x24));
        in[0] = 0xdeadbeef;
        if (cmd53_pio(0, 1, 0x8004, 1, 4, in, 0) == 0)
            printf("  capabilities %d: %08x  (present %08x)\n", k, in[0], R32(0x24));
    }
}

/* CMD53 on function 0 over the common CIS (bytes known from CMD52): checks
 * the host's PIO path against data that cannot be stale */
static void cis_test(unsigned div, int onebit)
{
    static const uint8_t cis[16] = { 0x20, 0x04, 0xd0, 0x02, 0x29, 0x43, 0x15, 0x1a,
                                     0x08, 0x00, 0x00, 0x00, 0x73, 0x3d, 0x43, 0x30 };
    uint32_t in[64];
    int v, i, k, nb;

    v = cmd52(0, 0x07, -1);
    if (v < 0)
        return;
    if (!onebit) {
        cmd52(0, 0x07, (v & ~3) | 2);
        W8(0x28, R8(0x28) | 0x02);
    }
    if (clock_on(div))
        return;
    cmd52(0, 0x10, 64);                              /* function 0 block size */
    cmd52(0, 0x11, 0);
    printf("FN0 block size %02x%02x\n", cmd52(0, 0x11, -1), cmd52(0, 0x10, -1));
    for (k = 0; k < 4; k++) {
        static const int mode[4][2] = { { 1, 4 }, { 1, 64 }, { 0, 1 }, { 0, 2 } };
        bytemode = mode[k][0];
        nb = bytemode ? 1 : mode[k][1];
        memset(in, 0xee, sizeof(in));
        if (cmd53_pio(0, 0, 0x1000, nb, bytemode ? mode[k][1] : 64, in, 0))
            continue;
        printf("  got:");
        for (i = 0; i < (bytemode ? mode[k][1] : 64 * nb) / 4 && i < 32; i++)
            printf("%s%08x", i % 8 ? " " : "\n   ", in[i]);
        printf("\n  CIS starts 20 04 d0 02 29 43 15 1a 08 00 00 00 73 3d 43 30 (LE words %02x%02x%02x%02x ...)\n",
               cis[3], cis[2], cis[1], cis[0]);
    }
}

static void fifo_test(unsigned div, int blocks, int blksz, uint32_t addr, int onebit)
{
    static uint32_t out[512 * 128], in[512 * 128];
    int v, i, words, bad = 0;

    if (bytemode)
        blocks = 1;
    words = blocks * blksz / 4;

    printf("== PIO buffer test: %d blocks of %d bytes, F1 %05x\n", blocks, blksz, addr);
    v = cmd52(0, 0x07, -1);                          /* bus width */
    if (v < 0)
        return;
    if (!onebit) {
        cmd52(0, 0x07, (v & ~3) | 2);
        W8(0x28, R8(0x28) | 0x02);
    }
    printf("CCCR 07 %02x -> %02x, host control %02x\n", v, cmd52(0, 0x07, -1), R8(0x28));
    if (clock_on(div))
        return;
    /* brcmfmac's order: F1 block size, F1 on, the window on chipcommon and
     * its chip ID read with a 4-byte CMD53, and only then the clock */
    cmd52(0, 0x110, blksz & 0xff);                   /* F1 block size */
    cmd52(0, 0x111, blksz >> 8);
    cmd52(0, 0x02, 0x02);                            /* enable F1 */
    for (i = 0; i < 100 && !((v = cmd52(0, 0x03, -1)) & 2); i++)
        usleep(1000);
    printf("F1 ready: %02x; FBR1 block size %02x%02x, CCCR 08 %02x 13 %02x\n", v,
           cmd52(0, 0x111, -1), cmd52(0, 0x110, -1), cmd52(0, 0x08, -1), cmd52(0, 0x13, -1));
    cmd52(1, 0x1000a, 0);                            /* window: chipcommon */
    cmd52(1, 0x1000b, 0);
    cmd52(1, 0x1000c, 0x18);
    {
        int sv = bytemode;
        bytemode = 1;
        if (cmd53_pio(0, 1, 0x8000, 1, 4, in, 0) == 0)
            printf("chip ID (0x18000000): %08x\n", in[0]);
        bytemode = sv;
    }
    cmd52(1, 0x1000e, 0x28);                         /* ALP request, HW clock requests off */
    for (i = 0; i < 100 && !((v = cmd52(1, 0x1000e, -1)) & 0x40); i++)
        usleep(1000);
    cmd52(1, 0x1000e, 0x21);                         /* force ALP, as brcmfmac leaves it */
    printf("chip clock CSR %02x (ALP %s) -> %02x\n", v, v & 0x40 ? "available" : "NOT available",
           cmd52(1, 0x1000e, -1));
    {
        int sv = bytemode;
        bytemode = 1;
        if (cmd53_pio(0, 1, 0x8000, 1, 4, in, 0) == 0)
            printf("chip ID again: %08x\n", in[0]);
        bytemode = sv;
    }
    cmd52(1, 0x1000a, (window >> 8) & 0x80);         /* the test's window */
    cmd52(1, 0x1000b, (window >> 16) & 0xff);
    cmd52(1, 0x1000c, window >> 24);
    printf("window %08x\n", window);

    if (cmd53_pio(0, 1, addr, blocks, blksz, in, 1) == 0) {
        printf("read:");
        for (i = 0; i < words && i < 16; i++)
            printf(" %08x", in[i]);
        printf("\n");
    }
    for (i = 0; i < words; i++)
        out[i] = 0xc0de0000 | i;
    if (readonly) {
        dump("after the test");
        return;
    }
    if (cmd53_pio(1, 1, addr, blocks, blksz, out, 1) == 0) {
        memset(in, 0, sizeof(in));
        if (cmd53_pio(0, 1, addr, blocks, blksz, in, 0) == 0) {
            for (i = 0; i < words; i++)
                if (in[i] != out[i] && bad++ < 8)
                    printf("  word %d: wrote %08x, read %08x\n", i, out[i], in[i]);
            printf("read back: %d of %d words differ\n", bad, words);
        }
    }
    dump("after the test");
}

int main(int argc, char **argv)
{
    unsigned div = 0x40, fdiv = 1;
    int opt, xtest = 0, fd, i, ftest = 0, fblocks = 2, fblksz = 64, onebit = 0;
    uint32_t faddr = 0x9000;
    volatile uint32_t *pmgr;
    uint32_t s, ocr;

    while ((opt = getopt(argc, argv, "d:xfD:n:B:A:1bW:rcs:m:eL")) != -1) {
        if (opt == 'd') div = strtoul(optarg, NULL, 0);
        else if (opt == 'x') xtest = 1;
        else if (opt == 'f') ftest = 1;
        else if (opt == 'D') fdiv = strtoul(optarg, NULL, 0);
        else if (opt == 'n') fblocks = strtoul(optarg, NULL, 0);
        else if (opt == 'B') fblksz = strtoul(optarg, NULL, 0);
        else if (opt == 'A') faddr = strtoul(optarg, NULL, 0);
        else if (opt == '1') onebit = 1;
        else if (opt == 'b') bytemode = 1;
        else if (opt == 'W') window = strtoul(optarg, NULL, 0) & 0xffff8000;
        else if (opt == 'r') readonly = 1;
        else if (opt == 'c') ftest = 2;
        else if (opt == 's') { ftest = 3; fblocks = strtoul(optarg, NULL, 0); }
        else if (opt == 'm') { ftest = 4; fblocks = strtoul(optarg, NULL, 0); }
        else if (opt == 'e') ftest = 5;
        else if (opt == 'L') ftest = 6;
        else { fprintf(stderr, "usage: %s [-d div] [-x] [-f [-D div] [-n blocks] [-B blksz] [-A addr] [-1]]\n", argv[0]); return 2; }
    }
    if (fblocks < 1 || fblocks > 511 || fblksz < 4 || fblksz > 512 || fblksz % 4) {
        fprintf(stderr, "-n 1..511, -B 4..512 in steps of 4\n");
        return 2;
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd < 0) { perror("/dev/mem"); return 1; }
    b = mmap(NULL, 0x1000, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0x30000000);
    pmgr = mmap(NULL, 0x2000, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0x3f100000);
    if (b == MAP_FAILED || pmgr == MAP_FAILED) { perror("mmap"); return 1; }

    /* SDIO-WIFI power state, 0x3f1010b8: on as AppleS5L8940XIO switches it */
    s = pmgr[0x10b8 / 4];
    if ((s & 0xff) != 0xff) {
        pmgr[0x10b8 / 4] = (s & ~0x10f) | 0xf;
        for (i = 0; i < 1000 && (pmgr[0x10b8 / 4] & 0xf0) != 0xf0; i++)
            usleep(10);
    }
    printf("PMGR SDIO-WIFI %08x -> %08x, SDIO-CLKs %08x\n", s, pmgr[0x10b8 / 4], pmgr[0x50 / 4]);

    dump("start");
    W8(0x2f, 1);                                     /* software reset: all */
    for (i = 0; i < 1000 && (R8(0x2f) & 1); i++)
        usleep(10);
    dump("after reset all");
    W32(0x34, 0xffff00ff);                           /* status: everything but card irq */
    W32(0x38, 0);                                    /* no signal: polled */
    W8(0x29, 0x0f);                                  /* bus power, 3.3 V */
    msleep(10);
    dump("power on");
    W8(0x2e, 0x0e);                                  /* data timeout */
    if (clock_on(div))
        return 1;
    msleep(5);                                       /* 74 clocks and more */

    cmd("CMD52 rd CCCR0", 52, 0x00000000, 0x1a);     /* R5: 48-bit, CRC, index */
    cmd("CMD0", 0, 0, 0x00);
    msleep(2);
    cmd("CMD52 rd CCCR0", 52, 0x00000000, 0x1a);
    s = cmd("CMD5 (probe)", 5, 0, 0x02);             /* R4: 48-bit, no CRC */
    ocr = R32(0x10);
    if ((s & 1) && !(s & 0x8000)) {
        for (i = 0; i < 20; i++) {
            s = cmd("CMD5 (OCR)", 5, ocr & 0x00ffff00, 0x02);
            if (R32(0x10) & 0x80000000)
                break;
            msleep(10);
        }
        printf("CMD5 response %08x: %s, %u functions\n", R32(0x10),
               R32(0x10) & 0x80000000 ? "ready" : "NOT ready", (R32(0x10) >> 28) & 7);
        s = cmd("CMD3 (RCA)", 3, 0, 0x1a);
        if ((s & 1) && !(s & 0x8000)) {
            uint32_t rca = R32(0x10) >> 16, cis = 0, a;
            int t;

            cmd("CMD7 (select)", 7, rca << 16, 0x1b);      /* R1b */
            printf("CCCR:");
            for (a = 0; a < 0x16; a++) {
                if (!(cmd52q(a, &t)))
                    printf(" %02x", t);
                else
                    printf(" --");
            }
            printf("\n");
            for (a = 0; a < 3; a++)
                if (!cmd52q(0x09 + a, &t))
                    cis |= (uint32_t)t << (8 * a);
            printf("common CIS at %06x:", cis);
            for (a = 0; a < 64; a++)
                if (!cmd52q(cis + a, &t))
                    printf(" %02x", t);
                else
                    printf(" --");
            printf("\n");
            if (ftest == 1)
                fifo_test(fdiv, fblocks, fblksz, faddr, onebit);
            else if (ftest == 2)
                cis_test(fdiv, onebit);
            else if (ftest == 3)
                stale_test(fdiv, fblocks);
            else if (ftest == 4)
                socram_test(fdiv, fblocks);
            else if (ftest == 5)
                error_test(fdiv);
            else if (ftest == 6)
                lag_test(fdiv);
        }
    }
    if (xtest) {
        dump("x: before");
        W8(0x2f, 2);
        for (i = 0; i < 1000 && (R8(0x2f) & 2); i++)
            usleep(10);
        dump("x: after CMD reset");
        W8(0x2f, 4);
        for (i = 0; i < 1000 && (R8(0x2f) & 4); i++)
            usleep(10);
        dump("x: after DATA reset");
    }
    return 0;
}
