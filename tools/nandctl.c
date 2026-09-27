/* nandctl -- READ-ONLY poking at the A5's NAND through the H2FMI controllers.
 *
 * The NAND carries the iPad's iOS install.  This tool never programs and never
 * erases: every command byte goes through nand_cmd(), which only lets through
 * the opcodes on READ_ONLY_OPS below.  There is no path in here that can issue
 * a program (80/10), an erase (60/D0) or a PPN set-features (EF ... E7).
 *
 * Two buses, each an FMI (DMA/PIO side), an FMC (the NAND bus itself, +0x40000)
 * and an ECC block (+0x80000): FMI0 at 0x31200000, FMI1 at 0x31300000 (ADT
 * arm-io/flash-controller0).  Register use is iBEC's own H2fmi.c, read out of
 * the disassembly (build/firmware/iBEC.dec): h2fmi_nand_reset, h2fmi_nand_read_id,
 * h2fmi_pio_read_sector, h2fmi_device_reset.  Clock gates are PMGR 0x3f1010c4/c8
 * (FMI0) and 0x3f1010cc/d0 (FMI1); iBoot leaves them on after reading NVRAM.
 *
 *   nandctl regs                    dump both controllers (skips the data FIFO)
 *   nandctl readid BUS CE [ADDR]    READ ID (90h) at ADDR (default 0), 8 bytes
 *   nandctl reset BUS CE            NAND RESET (FFh) -- touches no data
 *   nandctl getfeat BUS CE FEAT LEN PPN get feature (EE FEAT E7, 77 7D, 7A)
 *   nandctl params BUS CE           PPN device parameters (92 00 97), 512 bytes
 *   nandctl ppninfo                 firmware version (feature 0x9080), both buses
 *
 * PPN sequences are iBEC's H2fmi_ppn.c: h2fmi_ppn_get_feature,
 * h2fmi_ppn_get_device_params, h2fmi_get_nand_status.
 *
 * CE is the chip select on that bus.  On p105 the ADT has ce-bitmap 0x101: one
 * PPN package on bus 0 CE0 and one on bus 1 CE0, and only those are accepted.
 * An absent CE must never be read from while the FMC is in DDR (FMC_ON = 5,
 * as iBEC leaves it): data out is clocked by the chip's DQS, and with no chip
 * the FMC waits forever -- only a PMGR reset of the block gets it back.  Build:
 *   arm-linux-gnueabihf-gcc -static -Os -o nandctl tools/nandctl.c
 */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define FMI_BASE(bus)   (0x31200000u + (bus) * 0x100000u)
#define FMC_OFF         0x40000u
#define ECC_OFF         0x80000u
#define PMGR_GATES      0x3f101000u

/* FMI (DMA/PIO) */
#define FMI_CONFIG      0x00
#define FMI_CONTROL     0x04
#define FMI_STATUS      0x0c
#define FMI_DATA        0x14    /* PIO FIFO: a read pops it */
#define FMI_DMA_STATUS  0x1c
#define FMI_PIO_CONFIG  0x34

/* FMC (NAND bus) */
#define FMC_ON          0x00
#define FMC_IF_CTRL     0x08
#define FMC_CE_CTRL     0x0c
#define FMC_RW_CTRL     0x10
#define FMC_CMD         0x14
#define FMC_ADDR0       0x18
#define FMC_ADDRNUM     0x20
#define FMC_DATANUM     0x24
#define FMC_INTMASK     0x40
#define FMC_STATUS      0x44
#define FMC_NAND_STATUS 0x48
#define FMC_STATUS_MASK 0x4c

#define FMI_INTEN       0x10

/* first byte, and the second one it may be paired with (0 = none) */
static const uint8_t READ_ONLY_OPS[][2] = {
    { 0xff, 0 },        /* reset */
    { 0x90, 0 },        /* read ID */
    { 0x70, 0 },        /* read status */
    { 0xee, 0xe7 },     /* PPN get feature */
    { 0x92, 0x97 },     /* PPN get device parameters */
    { 0x77, 0x7d },     /* PPN operation status */
    { 0x77, 0 },        /* PPN end of operation */
    { 0x7a, 0 },        /* PPN data out */
};

static volatile uint32_t *fmi[2], *fmc[2], *ecc[2];
static volatile uint32_t *pmgr;

static void *map(int fd, uint32_t pa)
{
    void *p = mmap(NULL, 0x1000, PROT_READ | PROT_WRITE, MAP_SHARED, fd, pa);
    if (p == MAP_FAILED) {
        perror("mmap");
        exit(1);
    }
    return p;
}

#define R(b, o)         ((b)[(o) / 4])
#define W(b, o, v)      ((b)[(o) / 4] = (v))

static uint64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000000ull + ts.tv_nsec / 1000;
}

/* h2fmi_wait_done: poll until (reg & mask) == want, then write want back
 * (write-one-to-clear).  iBEC gives up after 100 ms. */
static int wait_done(volatile uint32_t *b, uint32_t off, uint32_t mask, uint32_t want)
{
    uint64_t t0 = now_us();
    while ((R(b, off) & mask) != want) {
        if (now_us() - t0 > 100000) {
            fprintf(stderr, "timeout: reg +0x%x = 0x%08x, want 0x%x/0x%x\n",
                    off, R(b, off), mask, want);
            return -1;
        }
    }
    W(b, off & ~3u, want);
    return 0;
}

/* FMC_CMD holds the first command byte and, for two-command sequences, the
 * second one in bits 15:8. */
static void nand_cmd2(int bus, uint8_t op, uint8_t op2)
{
    for (size_t i = 0; i < sizeof(READ_ONLY_OPS) / sizeof(READ_ONLY_OPS[0]); i++)
        if (READ_ONLY_OPS[i][0] == op && READ_ONLY_OPS[i][1] == op2) {
            W(fmc[bus], FMC_CMD, op | op2 << 8);
            return;
        }
    fprintf(stderr, "refusing NAND opcodes %02x %02x: not on the read-only list\n", op, op2);
    exit(2);
}

static void nand_cmd(int bus, uint8_t op)
{
    nand_cmd2(bus, op, 0);
}

#define CE_PRESENT      0x01    /* per bus, from the ADT's ce-bitmap 0x101 */

static int ce_ok(int ce)
{
    if (ce < 0 || ce > 7 || !(CE_PRESENT & (1u << ce))) {
        fprintf(stderr, "CE%d: no chip there (ce-bitmap 0x101); refusing\n", ce);
        return 0;
    }
    return 1;
}

static int gates_on(int bus)
{
    uint32_t a = R(pmgr, 0xc4 + bus * 8), b = R(pmgr, 0xc8 + bus * 8);
    if ((a & 0xf0) != 0xf0 || (b & 0xf0) != 0xf0) {
        fprintf(stderr, "FMI%d clock gates off (0x%08x 0x%08x); not touching it\n", bus, a, b);
        return 0;
    }
    return 1;
}

/* h2fmi_device_reset without the PMGR reset pulse: stop the FMI, bring the
 * FMC back up the way it was (iBEC leaves it at 5, DDR), timing too. */
static void device_reset(int bus, uint32_t on, uint32_t if_ctrl)
{
    W(fmi[bus], FMI_CONTROL, 6);
    W(fmc[bus], FMC_ON, on);
    W(fmc[bus], FMC_IF_CTRL, if_ctrl);
}

static int nand_reset(int bus, int ce)
{
    int r;
    W(fmc[bus], FMC_CE_CTRL, 1u << ce);
    nand_cmd(bus, 0xff);
    W(fmc[bus], FMC_RW_CTRL, 1);
    r = wait_done(fmc[bus], FMC_STATUS, 1, 1);
    W(fmc[bus], FMC_CE_CTRL, 0);
    return r;
}

static int read_id(int bus, int ce, uint8_t addr, uint8_t id[8])
{
    uint32_t on = R(fmc[bus], FMC_ON), if_ctrl = R(fmc[bus], FMC_IF_CTRL);
    uint32_t w[2] = { 0, 0 };
    int r = -1;

    W(fmc[bus], FMC_CE_CTRL, 1u << ce);
    nand_cmd(bus, 0x90);
    W(fmc[bus], FMC_ADDR0, addr);
    W(fmc[bus], FMC_ADDRNUM, 0);                    /* one address cycle */
    W(fmc[bus], FMC_RW_CTRL, 9);                    /* cmd + addr */
    if (wait_done(fmc[bus], FMC_STATUS, 9, 9))
        goto out;
    W(fmi[bus], FMI_CONFIG, 0);
    W(fmi[bus], FMI_PIO_CONFIG, 0x801);             /* 8 bytes by PIO */
    W(fmi[bus], FMI_CONTROL, 3);
    if (wait_done(fmi[bus], FMI_STATUS, 2, 2))
        goto reset;
    for (uint64_t t0 = now_us(); !(R(fmi[bus], FMI_DMA_STATUS) & 0x18); )
        if (now_us() - t0 > 100000) {
            fprintf(stderr, "timeout: FMI+0x1c = 0x%08x\n", R(fmi[bus], FMI_DMA_STATUS));
            goto reset;
        }
    w[0] = R(fmi[bus], FMI_DATA);
    w[1] = R(fmi[bus], FMI_DATA);
    r = 0;
reset:
    device_reset(bus, on, if_ctrl);
out:
    W(fmc[bus], FMC_CE_CTRL, 0);
    memcpy(id, w, 8);
    return r;
}

/* h2fmi_clear_interrupts_and_reset_masks */
static void clear_irqs(int bus)
{
    W(fmc[bus], FMC_INTMASK, 0);
    W(fmi[bus], FMI_INTEN, 0);
    W(fmc[bus], FMC_STATUS, 0x31ffff);
    W(fmi[bus], FMI_STATUS, 0xf);
}

/* single command, FMC_RW_CTRL bit 0 */
static int ppn_cmd1(int bus, uint8_t op)
{
    nand_cmd(bus, op);
    W(fmc[bus], FMC_RW_CTRL, 1);
    return wait_done(fmc[bus], FMC_STATUS, 1, 1);
}

/* command, 1-8 address bytes, second command: RW_CTRL 0xb */
static int ppn_cmd_addr_cmd(int bus, uint8_t op, uint8_t op2, uint32_t addr, int naddr)
{
    nand_cmd2(bus, op, op2);
    W(fmc[bus], FMC_ADDR0, addr);
    W(fmc[bus], FMC_ADDRNUM, (naddr + 7) & 7);
    W(fmc[bus], FMC_RW_CTRL, 0xb);
    return wait_done(fmc[bus], FMC_STATUS, 0xb, 0xb);
}

/* h2fmi_ppn_get_operation_status + h2fmi_get_nand_status: 77 7D, then let the
 * FMC poll the status byte until bit 6 (ready) is up.  iBEC waits for the
 * interrupt; here the raw FMC status bit 5 is polled instead. */
static int ppn_status(int bus, uint8_t *st)
{
    nand_cmd2(bus, 0x77, 0x7d);
    W(fmc[bus], FMC_RW_CTRL, 3);
    if (wait_done(fmc[bus], FMC_STATUS, 3, 3))
        return -1;
    clear_irqs(bus);
    W(fmi[bus], FMI_INTEN, 0x100);
    W(fmc[bus], FMC_IF_CTRL, R(fmc[bus], FMC_IF_CTRL) & ~0x100000u);
    W(fmc[bus], FMC_STATUS_MASK, 0x4040);
    W(fmc[bus], FMC_DATANUM, 0);
    W(fmc[bus], FMC_INTMASK, 0x20);
    W(fmc[bus], FMC_RW_CTRL, 0x50);
    uint64_t t0 = now_us();
    while (!(R(fmc[bus], FMC_STATUS) & 0x20))
        if (now_us() - t0 > 2000000) {
            fprintf(stderr, "status timeout: FMC+0x44 0x%08x FMI+0xc 0x%08x byte 0x%02x\n",
                    R(fmc[bus], FMC_STATUS), R(fmi[bus], FMI_STATUS),
                    R(fmc[bus], FMC_NAND_STATUS) & 0xff);
            W(fmc[bus], FMC_RW_CTRL, 0);
            clear_irqs(bus);
            return -1;
        }
    *st = R(fmc[bus], FMC_NAND_STATUS);
    W(fmc[bus], FMC_RW_CTRL, 0);
    clear_irqs(bus);
    return 0;
}

static int pio_read(int bus, uint32_t *dst, uint32_t bytes)
{
    for (uint64_t t0 = now_us(); !(R(fmi[bus], FMI_DMA_STATUS) & 0x18); )
        if (now_us() - t0 > 100000) {
            fprintf(stderr, "timeout: FMI+0x1c = 0x%08x\n", R(fmi[bus], FMI_DMA_STATUS));
            return -1;
        }
    for (uint32_t i = 0; i < bytes / 4; i++)
        dst[i] = R(fmi[bus], FMI_DATA);
    return 0;
}

/* h2fmi_ppn_read_data_out: 7A, then the bytes by PIO.  FMI_PIO_CONFIG is
 * sector bytes << 8 | sectors. */
static int ppn_data_out(int bus, uint8_t *buf, uint32_t len)
{
    uint32_t w[256];
    if (ppn_cmd1(bus, 0x7a))
        return -1;
    for (uint32_t off = 0; off < len; ) {
        uint32_t n = len - off > 0x400 ? 0x400 : len - off;
        uint32_t nw = n < 4 ? 4 : n & ~3u;
        W(fmi[bus], FMI_CONFIG, 5);
        W(fmi[bus], FMI_PIO_CONFIG, (nw << 8 & 0x7fc00) | 1);
        W(fmi[bus], FMI_CONTROL, 3);
        if (pio_read(bus, w, nw))
            return -1;
        memcpy(buf + off, w, n);
        off += nw;
    }
    return 0;
}

static void ppn_end(int bus)
{
    W(fmi[bus], FMI_CONTROL, 6);
    ppn_cmd1(bus, 0x77);
    W(fmc[bus], FMC_CE_CTRL, 0);
}

static int ppn_get_feature(int bus, int ce, uint16_t feat, uint8_t *buf, uint32_t len, uint8_t *st)
{
    int r = -1;
    W(fmc[bus], FMC_CE_CTRL, 1u << ce);
    clear_irqs(bus);
    if (ppn_cmd_addr_cmd(bus, 0xee, 0xe7, feat, 2) == 0 && ppn_status(bus, st) == 0)
        r = ppn_data_out(bus, buf, len);
    ppn_end(bus);
    return r;
}

static int ppn_params(int bus, int ce, uint8_t *buf, uint8_t *st)
{
    int r = -1;
    W(fmc[bus], FMC_CE_CTRL, 1u << ce);
    clear_irqs(bus);
    if (ppn_cmd_addr_cmd(bus, 0x92, 0x97, 0, 1) == 0 && ppn_status(bus, st) == 0 && *st == 0x40)
        r = ppn_data_out(bus, buf, 0x200);
    ppn_end(bus);
    return r;
}

static void hexdump(const uint8_t *p, uint32_t n)
{
    for (uint32_t o = 0; o < n; o += 16) {
        printf("  %04x:", o);
        for (uint32_t k = o; k < o + 16 && k < n; k++)
            printf(" %02x", p[k]);
        printf("  ");
        for (uint32_t k = o; k < o + 16 && k < n; k++)
            putchar(p[k] >= 0x20 && p[k] < 0x7f ? p[k] : '.');
        printf("\n");
    }
}

static void dump(const char *name, volatile uint32_t *b, uint32_t pa, uint32_t n, int skip_fifo)
{
    printf("%s @ 0x%08x\n", name, pa);
    for (uint32_t o = 0; o < n; o += 16) {
        printf("  +%03x:", o);
        for (uint32_t k = o; k < o + 16; k += 4)
            if (skip_fifo && k == FMI_DATA)
                printf(" --------");
            else
                printf(" %08x", R(b, k));
        printf("\n");
    }
}

static void print_id(int bus, int ce, uint8_t addr, const uint8_t id[8])
{
    printf("FMI%d CE%d id@%02x:", bus, ce, addr);
    for (int i = 0; i < 8; i++)
        printf(" %02x", id[i]);
    printf("  \"");
    for (int i = 0; i < 8; i++)
        putchar(id[i] >= 0x20 && id[i] < 0x7f ? id[i] : '.');
    printf("\"\n");
}

int main(int argc, char **argv)
{
    int fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd < 0) {
        perror("/dev/mem");
        return 1;
    }
    pmgr = map(fd, PMGR_GATES);
    for (int b = 0; b < 2; b++) {
        fmi[b] = map(fd, FMI_BASE(b));
        fmc[b] = map(fd, FMI_BASE(b) + FMC_OFF);
        ecc[b] = map(fd, FMI_BASE(b) + ECC_OFF);
    }

    if (argc >= 2 && !strcmp(argv[1], "regs")) {
        for (int b = 0; b < 2; b++) {
            printf("FMI%d gates: 0x%08x 0x%08x\n", b, R(pmgr, 0xc4 + b * 8), R(pmgr, 0xc8 + b * 8));
            if (!gates_on(b))
                continue;
            dump("  FMI", fmi[b], FMI_BASE(b), 0x40, 1);
            dump("  FMC", fmc[b], FMI_BASE(b) + FMC_OFF, 0x80, 0);
            dump("  ECC", ecc[b], FMI_BASE(b) + ECC_OFF, 0x40, 0);
        }
        return 0;
    }
    if (argc >= 4 && !strcmp(argv[1], "readid")) {
        int bus = atoi(argv[2]) & 1, ce = atoi(argv[3]);
        if (!ce_ok(ce))
            return 1;
        uint8_t addr = argc > 4 ? strtoul(argv[4], NULL, 0) : 0, id[8];
        if (!gates_on(bus))
            return 1;
        int r = read_id(bus, ce, addr, id);
        print_id(bus, ce, addr, id);
        return r ? 1 : 0;
    }
    if (argc >= 4 && !strcmp(argv[1], "reset")) {
        int bus = atoi(argv[2]) & 1, ce = atoi(argv[3]);
        if (!ce_ok(ce))
            return 1;
        if (!gates_on(bus))
            return 1;
        int r = nand_reset(bus, ce);
        printf("FMI%d CE%d reset: %s\n", bus, ce, r ? "timeout" : "ok");
        return r ? 1 : 0;
    }
    if (argc >= 6 && !strcmp(argv[1], "getfeat")) {
        int bus = atoi(argv[2]) & 1, ce = atoi(argv[3]);
        if (!ce_ok(ce))
            return 1;
        uint16_t feat = strtoul(argv[4], NULL, 0);
        uint32_t len = strtoul(argv[5], NULL, 0);
        uint8_t buf[4096] = { 0 }, st = 0;
        if (len > sizeof(buf) || !gates_on(bus))
            return 1;
        int r = ppn_get_feature(bus, ce, feat, buf, len, &st);
        printf("FMI%d CE%d feature 0x%04x: status 0x%02x%s\n", bus, ce, feat, st, r ? " (failed)" : "");
        hexdump(buf, len);
        return r ? 1 : 0;
    }
    if (argc >= 4 && !strcmp(argv[1], "params")) {
        int bus = atoi(argv[2]) & 1, ce = atoi(argv[3]);
        if (!ce_ok(ce))
            return 1;
        uint8_t buf[0x200] = { 0 }, st = 0;
        if (!gates_on(bus))
            return 1;
        int r = ppn_params(bus, ce, buf, &st);
        printf("FMI%d CE%d device parameters: status 0x%02x%s\n", bus, ce, st, r ? " (failed)" : "");
        hexdump(buf, sizeof(buf));
        return r ? 1 : 0;
    }
    if (argc >= 2 && !strcmp(argv[1], "ppninfo")) {
        for (int b = 0; b < 2; b++) {
            uint8_t buf[16] = { 0 }, st = 0;
            if (!gates_on(b))
                continue;
            int r = ppn_get_feature(b, 0, 0x9080, buf, 16, &st);
            printf("FMI%d CE0 firmware (0x9080): status 0x%02x%s \"%.16s\"\n", b, st,
                   r ? " (failed)" : "", (char *)buf);
        }
        return 0;
    }
    fprintf(stderr, "usage: nandctl regs | readid BUS CE [ADDR] | reset BUS CE |\n"
                    "               getfeat BUS CE FEAT LEN | params BUS CE | ppninfo\n");
    return 1;
}
