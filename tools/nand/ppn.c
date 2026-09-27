/* ppn.c -- READ-ONLY access to the A5's NAND through the H2FMI controllers.
 *
 * The NAND carries the iPad's iOS install.  Nothing here programs or erases:
 * every command byte goes through nand_cmd2(), which only lets through the
 * opcodes on READ_ONLY_OPS below.  There is no path that can issue a program
 * (80/10, PPN boot page 8A/17) or an erase (60/D0, PPN 67).  The one setter
 * is PPN set-feature (EF ... E7), and only for feature 0x180, the bus power
 * state, with the two values iBoot itself uses (ppn_set_power_state).
 *
 * Two buses, each an FMI (DMA/PIO side), an FMC (the NAND bus itself, +0x40000)
 * and an ECC block (+0x80000): FMI0 at 0x31200000, FMI1 at 0x31300000 (ADT
 * arm-io/flash-controller0).  Register use is iBEC's own, read out of its
 * disassembly (build/firmware/iBEC.dec): H2fmi.c (h2fmi_nand_reset,
 * h2fmi_nand_read_id, h2fmi_pio_read_sector, h2fmi_device_reset), H2fmi_ppn.c
 * (h2fmi_ppn_get_feature, h2fmi_ppn_get_device_params, h2fmi_get_nand_status,
 * h2fmi_ppn_set_features) and fmiss_ppn.c (the page read).  Clock gates are
 * PMGR 0x3f1010c4/c8 (FMI0) and 0x3f1010cc/d0 (FMI1); iBoot leaves them on,
 * and leaves the FMC in DDR (FMC_ON = 5).
 *
 * An absent CE must never be read from in DDR: data out is clocked by the
 * chip's DQS, and with no chip the FMC waits forever.  And one process per
 * bus: two interleaved command sequences on one FMC wedge it and confuse the
 * PPN, so ppn_claim() takes /tmp/nandctl-busN.lock.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "ppn.h"

#define R PPN_R
#define W PPN_W
#define fmi ppn_fmi
#define fmc ppn_fmc

volatile uint32_t *ppn_fmi[PPN_BUSES], *ppn_fmc[PPN_BUSES], *ppn_ecc[PPN_BUSES];
volatile uint32_t *ppn_pmgr;

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
    { 0x0a, 0x37 },     /* PPN page read */
};

#define CE_PRESENT      0x01    /* per bus, from the ADT's ce-bitmap 0x101 */

static void *map(int fd, uint32_t pa)
{
    void *p = mmap(NULL, 0x1000, PROT_READ | PROT_WRITE, MAP_SHARED, fd, pa);
    if (p == MAP_FAILED) {
        perror("mmap");
        exit(1);
    }
    return p;
}

int ppn_open(void)
{
    int fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd < 0) {
        perror("/dev/mem");
        exit(1);
    }
    ppn_pmgr = map(fd, PMGR_GATES);
    for (int b = 0; b < PPN_BUSES; b++) {
        fmi[b] = map(fd, FMI_BASE(b));
        fmc[b] = map(fd, FMI_BASE(b) + FMC_OFF);
        ppn_ecc[b] = map(fd, FMI_BASE(b) + ECC_OFF);
    }
    return 0;
}

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

int ppn_ce_ok(int ce)
{
    if (ce < 0 || ce > 7 || !(CE_PRESENT & (1u << ce))) {
        fprintf(stderr, "CE%d: no chip there (ce-bitmap 0x101); refusing\n", ce);
        return 0;
    }
    return 1;
}

int ppn_lock(int bus)
{
    char name[32];
    snprintf(name, sizeof(name), "/tmp/nandctl-bus%d.lock", bus);
    int fd = open(name, O_RDWR | O_CREAT, 0644);
    if (fd < 0 || flock(fd, LOCK_EX | LOCK_NB)) {
        fprintf(stderr, "FMI%d is busy (another nandctl, a dump, iosnand?): %s\n", bus,
                fd < 0 ? strerror(errno) : "locked");
        return 0;
    }
    return 1;       /* held until exit */
}

int ppn_gates_on(int bus)
{
    uint32_t a = R(ppn_pmgr, 0xc4 + bus * 8), b = R(ppn_pmgr, 0xc8 + bus * 8);
    if ((a & 0xf0) != 0xf0 || (b & 0xf0) != 0xf0) {
        fprintf(stderr, "FMI%d clock gates off (0x%08x 0x%08x); not touching it\n", bus, a, b);
        return 0;
    }
    return 1;
}

int ppn_claim(int bus)
{
    return ppn_gates_on(bus) && ppn_lock(bus);
}

/* h2fmi_device_reset without the PMGR reset pulse: stop the FMI, bring the
 * FMC back up the way it was (iBEC leaves it at 5, DDR), timing too. */
static void device_reset(int bus, uint32_t on, uint32_t if_ctrl)
{
    W(fmi[bus], FMI_CONTROL, 6);
    W(fmc[bus], FMC_ON, on);
    W(fmc[bus], FMC_IF_CTRL, if_ctrl);
}

/* The FMC config iBEC leaves (DDR on, Toggle timing) is lost on a block
 * reset; everything else comes back at its reset value. */
static const uint32_t FMC_KEEP[] = { 0x00, 0x04, 0x08, 0x30, 0x34, 0x4c, 0x68, 0x6c, 0x70, 0x74, 0x78 };

void ppn_recover(int bus)
{
    uint32_t v[sizeof(FMC_KEEP) / sizeof(FMC_KEEP[0])];
    for (size_t i = 0; i < sizeof(v) / sizeof(v[0]); i++)
        v[i] = R(fmc[bus], FMC_KEEP[i]);
    uint32_t g = R(ppn_pmgr, 0xc4 + bus * 8);
    W(ppn_pmgr, 0xc4 + bus * 8, g | 0x80000000u);
    usleep(10);
    W(ppn_pmgr, 0xc4 + bus * 8, g & ~0x80000000u);
    W(fmi[bus], FMI_CONTROL, 6);
    for (size_t i = 1; i < sizeof(v) / sizeof(v[0]); i++)
        W(fmc[bus], FMC_KEEP[i], v[i]);
    W(fmc[bus], FMC_ON, v[0]);
}

int ppn_nand_reset(int bus, int ce)
{
    int r;
    W(fmc[bus], FMC_CE_CTRL, 1u << ce);
    nand_cmd(bus, 0xff);
    W(fmc[bus], FMC_RW_CTRL, 1);
    r = wait_done(fmc[bus], FMC_STATUS, 1, 1);
    W(fmc[bus], FMC_CE_CTRL, 0);
    return r;
}

int ppn_read_id(int bus, int ce, uint8_t addr, uint8_t id[8])
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

/* E7 on its own closes a set-feature; only ppn_set_power_state sends it. */
static int ppn_cmd_e7(int bus)
{
    W(fmc[bus], FMC_CMD, 0xe7);
    W(fmc[bus], FMC_RW_CTRL, 1);
    return wait_done(fmc[bus], FMC_STATUS, 1, 1);
}

static void ppn_end(int bus)
{
    W(fmi[bus], FMI_CONTROL, 6);
    ppn_cmd1(bus, 0x77);
    W(fmc[bus], FMC_CE_CTRL, 0);
}

int ppn_get_feature(int bus, int ce, uint16_t feat, uint8_t *buf, uint32_t len, uint8_t *st)
{
    int r = -1;
    W(fmc[bus], FMC_CE_CTRL, 1u << ce);
    clear_irqs(bus);
    if (ppn_cmd_addr_cmd(bus, 0xee, 0xe7, feat, 2) == 0 && ppn_status(bus, st) == 0)
        r = ppn_data_out(bus, buf, len);
    ppn_end(bus);
    return r;
}

/* h2fmi_ppn_set_features, for the power-state feature only: EF FEAT (RW_CTRL
 * 9), the value as one PIO word (FMI_CONTROL 5 = host to NAND), E7, status. */
int ppn_set_power_state(int bus, int ce, uint32_t ps, uint8_t *st)
{
    uint32_t on = R(fmc[bus], FMC_ON), if_ctrl = R(fmc[bus], FMC_IF_CTRL);
    int r = -1;

    if (ps != PPN_PS_ASYNC && ps != PPN_PS_DDR)
        return -1;
    W(fmc[bus], FMC_CE_CTRL, 1u << ce);
    W(fmi[bus], FMI_CONTROL, 6);
    W(fmc[bus], FMC_CMD, 0xef);
    W(fmc[bus], FMC_ADDR0, PPN_FEATURE_POWER_STATE);
    W(fmc[bus], FMC_ADDRNUM, 1);
    W(fmc[bus], FMC_RW_CTRL, 9);
    if (wait_done(fmc[bus], FMC_STATUS, 9, 9))
        goto out;
    clear_irqs(bus);
    W(fmi[bus], FMI_CONFIG, 5);
    W(fmi[bus], FMI_PIO_CONFIG, 4 << 8 | 1);
    W(fmi[bus], FMI_CONTROL, 5);
    for (uint64_t t0 = now_us(); !(R(fmi[bus], FMI_DMA_STATUS) & 0x18); )
        if (now_us() - t0 > 100000)
            goto out;
    W(fmi[bus], FMI_DATA, ps);
    if (wait_done(fmi[bus], FMI_STATUS, 2, 2))
        goto out;
    if (ppn_cmd_e7(bus) || ppn_status(bus, st))
        goto out;
    r = *st == 0x40 ? 0 : -1;
out:
    ppn_cmd1(bus, 0x77);
    W(fmi[bus], FMI_CONTROL, 6);
    W(fmc[bus], FMC_CE_CTRL, 0);
    device_reset(bus, on, if_ctrl);
    return r;
}

int ppn_params(int bus, int ce, uint8_t *buf, uint8_t *st)
{
    int r = -1;
    W(fmc[bus], FMC_CE_CTRL, 1u << ce);
    clear_irqs(bus);
    if (ppn_cmd_addr_cmd(bus, 0x92, 0x97, 0, 1) == 0 && ppn_status(bus, st) == 0 && *st == 0x40)
        r = ppn_data_out(bus, buf, 0x200);
    ppn_end(bus);
    return r;
}

/* One page read, the way iBEC's sequencer queues it (fmiss_ppn_read_multi:
 * 0A ROW 37, status, 7A, data, 77), done by hand with PIO.  The PPN corrects
 * its own pages, so what comes out is data + metadata.  Boot pages are the
 * exception: they carry the FMI's BCH (ECC_CONFIG 0x1a8 = 53 bytes per 512),
 * which iBEC only decodes after switching the PPN to SDR (set-feature 0x180,
 * transitionWorldFromDDR) -- in DDR the FMI skips 54 bytes per sector, so
 * they have to be read raw and the BCH bytes dropped. */
int ppn_read_page(int bus, int ce, uint32_t row, uint8_t *buf, uint32_t len, uint8_t *st)
{
    uint32_t on = R(fmc[bus], FMC_ON), if_ctrl = R(fmc[bus], FMC_IF_CTRL);
    int r = -1;

    *st = 0;
    W(fmc[bus], FMC_CE_CTRL, 1u << ce);
    clear_irqs(bus);
    if (ppn_cmd_addr_cmd(bus, 0x0a, 0x37, row, 3) == 0 && ppn_status(bus, st) == 0)
        r = ppn_data_out(bus, buf, len);
    ppn_cmd1(bus, 0x77);
    W(fmc[bus], FMC_CE_CTRL, 0);
    device_reset(bus, on, if_ctrl);
    return r;
}
