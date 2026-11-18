/* k93nand -- READ-ONLY access to the iPad 2's raw NAND (K93AP, S5L8940X),
 * from Linux userspace through /dev/mem, by PIO.
 *
 *   k93nand regs                      PMGR gates + FMI/FMC/ECC registers of both
 *                                     buses (never reads a FIFO)
 *   k93nand init                      clock gates on, block reset, FMC to SDR,
 *                                     timing, interrupts cleared (h2fmi_init_minimal)
 *   k93nand id                        NAND RESET + READ ID on CE0-7 of each bus, the
 *                                     ce-bitmap, geometry from iBEC's chip tables;
 *                                     saves the ce-bitmap to /tmp/k93nand.state
 *                                     ($K93NAND_DIR, or /run/k93nand if it
 *                                     exists, instead of /tmp)
 *   k93nand read BUS CE PAGE [opts]   one page through the BCH engine: data head,
 *                                     metadata (raw and unwhitened), ECC per sector
 *       -s          sector mode: 8 one-sector transfers (iBEC's boot-page PIO pattern)
 *       -k auto|none|meta|user:LPN    undo the data whitening (AES-128-CBC) for
 *                   the hexdump; auto = by the page's metadata type (default)
 *       -o FILE     write the 8192 data bytes as read + the 10 metadata bytes
 *       -O FILE     write the 8192 data bytes after the AES step
 *   k93nand raw BUS CE PAGE [FILE]    one page with ECC OFF (h2fmi_rx_raw format):
 *                                     data + spare exactly as stored on the flash
 *   k93nand scan BUS CE FIRSTBLOCK NBLOCKS [-1|-s] [-v]
 *                                     metadata of every page, one line per page;
 *                                     -1 reads sector 0 only (8x less), -s sector
 *                                     mode, -v register trace (off by default)
 *   k93nand sig                       look for the special blocks (NANDDRIVERSIGN ...)
 *                                     at the top of virtual CE 0, plain and AES'd
 *   k93nand selftest                  no hardware: whitening table, IV, AES vectors
 *
 * PAGE is the CE-relative row address that goes on the bus (what iBEC's FIL calls
 * the page number), or BLOCK:PAGE, converted with the chip's die stride.
 * Global options (before the subcommand): -q quiet (no register trace), -v trace
 * every register read too, -W do not unwhiten metadata.
 *
 * Everything comes from iBEC 12H321 (iBoot-2261.30.37, base 0x9ff00000) and the
 * 12H321 IOP firmware; docs/research/k93-nand.md has the addresses.  This file
 * keeps iBEC's register values; where it cannot do what iBEC does the comment
 * says so: no CDMA (the CPU drains the FIFOs iBEC gives to DMA channels 5-8), no
 * interrupt (the pending bit is polled), no FMI sequencer (FMI+0xc0000 is not
 * even mapped -- its command words are opaque, so it is never written).
 *
 * READ ONLY, structurally:
 *  - every FMC_CMD write goes through nand_cmd(), which only knows RESET (FF),
 *    READ ID (90), READ STATUS (70), READ (00 + 30) and the bare 00 that ends a
 *    status read.  No program (80/10), erase (60/D0), set-feature (EF) or any
 *    vendor command exists here;
 *  - FMI_CTRL only ever gets 3 (NAND -> FIFO) or 6 (stop); 5, the host -> NAND
 *    direction iBEC uses to write, is refused;
 *  - FMC_RWCTRL only gets the cycle patterns a read needs (0, 1, 9, 0xb, 0x50);
 *  - nothing is ever written to the data or metadata FIFO.
 *
 * Absent chip selects: the K93's raw NAND runs SDR (asynchronous, FMC_ON = 1),
 * where an unselected bus simply reads back the pull-ups -- the iPad mini's DDR
 * wedge (DQS that never comes) cannot happen, and iBEC itself sends RESET and
 * READ ID to every CE number at every boot to build the ce-bitmap.  `id` does the
 * same, on CE0-7 of each bus.  Every other command refuses a CE that is not in
 * the ce-bitmap `id` saved (or K93NAND_CEMAP, bits 0-7 bus 0, 8-15 bus 1).
 *
 * Build (from devices/a5):
 *   docker run --rm --user "$(id -u):$(id -g)" -v "$PWD":/cascadia -w /cascadia \
 *       cascadia-build arm-linux-gnueabihf-gcc -static -Os -Wall \
 *       -o build/k93/tools/k93nand tools/nand/k93nand.c
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* ---- hardware ------------------------------------------------------------ */

#define NBUS            2
#define NCE_BUS         8
#define PMGR_PA         0x3f101000u     /* PMGR + 0x1000: power/clock gate registers */
#define FMI_PA(b)       (0x31200000u + (b) * 0x100000u)   /* ADT flash-controller0 reg */

enum { B_FMI, B_FMC, B_ECC, NBLK };
static const uint32_t BLK_OFF[NBLK] = { 0x00000, 0x40000, 0x80000 };
static const char *const BLK_NAME[NBLK] = { "FMI", "FMC", "ECC" };

/* FMI: the FIFO/transfer side.  Names from the IOP firmware's register dump
 * (h2fmi_iop debug print, iop-firmware.bin 0x3144: "CFG CTRL STAT INTPND INT_EN
 * DBG0 DATSIZ"). */
#define FMI_CFG         0x00    /* (ecc_bits << 3) | 5 for a page, 5 raw 1 KB, 0 short */
#define FMI_CTRL        0x04    /* 3 = start transfer NAND -> FIFO, 6 = stop/reset */
#define FMI_STAT        0x08
#define FMI_INTPND      0x0c    /* W1C: bit 1 transfer done, bit 8 FMC event */
#define FMI_INTEN       0x10
#define FMI_DATA        0x14    /* data FIFO (iBEC: CDMA ch 5 / 7, 32-bit, bursts of 8) */
#define FMI_META        0x18    /* metadata FIFO (CDMA ch 6 / 8, 8-bit, bursts of 1) */
#define FMI_DBG0        0x1c    /* bits 3-4: data waiting (h2fmi_wait_dma_task_pending) */
#define FMI_DATSIZ      0x34    /* sectors | bytes << 8 | meta << 19 | meta << 25 */

/* FMC: the NAND bus ("ON IFCTRL CECTRL RWCTRL CMD ADDR0 ADDR1 ANUM DNUM FSTATUS
 * NSTATUS RBB_CFG", same dump). */
#define FMC_ON          0x00    /* 1 = SDR (raw NAND), 5 = DDR (PPN only) */
#define FMC_IFCTRL      0x08    /* dc << 16 | rs << 12 | rh << 8 | ws << 4 | wh */
#define FMC_CECTRL      0x0c    /* 1 << ce */
#define FMC_RWCTRL      0x10    /* cycles: 1 cmd1, 8 addr, 2 cmd2 (0xb), 0x50 poll status */
#define FMC_CMD         0x14    /* cmd1 | cmd2 << 8 */
#define FMC_ADDR0       0x18
#define FMC_ADDR1       0x1c
#define FMC_ANUM        0x20    /* address bytes - 1 */
#define FMC_DNUM        0x24
#define FMC_INTMASK     0x40
#define FMC_FSTATUS     0x44    /* W1C: cycle-done bits, 0x20 status matched */
#define FMC_NSTATUS     0x48    /* last NAND status byte */
#define FMC_RBBCFG      0x4c    /* status poll: mask | value << 8 */

/* ECC (the BCH engine, PMGR gate "FMIx-BCH"); offsets from iBEC's use. */
#define ECC_CFG         0x08    /* 1 << 31 | t << 16 | flag-threshold << 8 */
#define ECC_RESULT      0x0c    /* read: next sector's result; write 1: clear them */
#define ECC_PND         0x10    /* bits 16-20: most bits corrected in the page */
#define ECC_MASK        0x14
#define ECC_LEVEL       0x2c    /* results waiting, bits 0-5 */

/* PMGR gate registers (iBoot clock ids 0x2f-0x32 = PMGR+0x1008 + id*4; ADT
 * device-clocks FMI0 59, FMI0-BCH 60, FMI1 61, FMI1-BCH 62).  Bit 31 of the
 * FMIx register is the block reset (iBoot clock_reset_device 0x9ff1eb88). */
#define GATE_FMI(b)     (0xc4u + (b) * 8u)
#define GATE_BCH(b)     (0xc8u + (b) * 8u)

/* iBEC's raw page format (h2fmiInit, h2fmi_calculate_ecc_bits 0x9ff05c38) */
#define META_BYTES      10      /* valid metadata on the flash (FTL sees 12, +2 x FF) */
#define SECTOR          1024    /* BCH sector */
#define MAX_PAGE        8192
#define MAX_SPARE       640

static volatile uint32_t *pmgr;
static volatile uint32_t *regs[NBUS][NBLK];
static int trace = 1;           /* 1: writes, 2: reads too, 0: quiet */
static int unwhiten = 1;
static uint32_t timing[NBUS];   /* iBEC's fmi->timing (fmi+0x44): IF_CTRL to restore */

/* The only three ways this file touches a register.  K93NAND_SIM (a host-side
 * test build, not shipped) replaces them with a model of the controller. */
#ifndef K93NAND_SIM
static inline uint32_t mmio_rd(int bus, int blk, uint32_t off)
{
    return bus < 0 ? pmgr[off / 4] : regs[bus][blk][off / 4];
}

static inline void mmio_wr(int bus, int blk, uint32_t off, uint32_t v)
{
    if (bus < 0)
        pmgr[off / 4] = v;
    else
        regs[bus][blk][off / 4] = v;
}

static inline uint8_t mmio_rd8(int bus, int blk, uint32_t off)
{
    return ((volatile uint8_t *)regs[bus][blk])[off];
}
#else
uint32_t mmio_rd(int bus, int blk, uint32_t off);
void mmio_wr(int bus, int blk, uint32_t off, uint32_t v);
uint8_t mmio_rd8(int bus, int blk, uint32_t off);
#endif
#define RAW(b, k, o)    mmio_rd(b, k, o)

/* ---- register access, where the allowlists live ---------------------------- */

static const char *reg_name(int blk, uint32_t off)
{
    static const char *const fmi[] = { "CFG", "CTRL", "STAT", "INTPND", "INT_EN", "DATA", "META", "DBG0" };
    static const char *const fmc[] = { "ON", "?", "IFCTRL", "CECTRL", "RWCTRL", "CMD", "ADDR0", "ADDR1", "ANUM", "DNUM" };
    if (blk == B_FMI && off < 0x20)
        return fmi[off / 4];
    if (blk == B_FMI && off == FMI_DATSIZ)
        return "DATSIZ";
    if (blk == B_FMC && off < 0x28)
        return fmc[off / 4];
    if (blk == B_FMC)
        switch (off) {
        case FMC_INTMASK: return "INTMASK";
        case FMC_FSTATUS: return "FSTATUS";
        case FMC_NSTATUS: return "NSTATUS";
        case FMC_RBBCFG: return "RBB_CFG";
        case 0x70: return "DQST";
        case 0x74: return "NANDT1";
        case 0x78: return "NANDT2";
        case 0x7c: return "NANDT3";
        }
    if (blk == B_ECC)
        switch (off) {
        case ECC_CFG: return "CFG";
        case ECC_RESULT: return "RESULT";
        case ECC_PND: return "PND";
        case ECC_MASK: return "MASK";
        case ECC_LEVEL: return "LEVEL";
        }
    return "";
}

static void die(const char *msg)
{
    fprintf(stderr, "k93nand: %s\n", msg);
    exit(2);
}

/* The only NAND opcodes this file can put on the bus (FMC_CMD = cmd1 | cmd2 << 8). */
static const uint32_t NAND_CMDS[] = {
    0x00ff,     /* RESET                                    h2fmi_nand_reset 0x9ff052ca */
    0x0090,     /* READ ID                                  h2fmi_nand_read_id 0x9ff0533e */
    0x0070,     /* READ STATUS                              0x9ff0588c */
    0x3000,     /* READ: 00h, 5 address cycles, 30h         0x9ff0608c */
    0x0000,     /* 00h: back to data output after 70h       0x9ff060dc */
};

static void wr(int bus, int blk, uint32_t off, uint32_t v)
{
    int ok = 1;
    if (blk == B_FMC && off == FMC_CMD) {
        ok = 0;
        for (size_t i = 0; i < sizeof(NAND_CMDS) / sizeof(NAND_CMDS[0]); i++)
            ok |= NAND_CMDS[i] == v;
    }
    if (blk == B_FMI && off == FMI_CTRL)
        ok = v == 3 || v == 6;
    if (blk == B_FMC && off == FMC_RWCTRL)
        ok = v == 0 || v == 1 || v == 9 || v == 0xb || v == 0x50;
    if (blk == B_FMI && (off == FMI_DATA || off == FMI_META))
        ok = 0;
    if (!ok) {
        fprintf(stderr, "k93nand: refusing %s%d+0x%02x (%s) <- 0x%08x: not on the read-only list\n",
                BLK_NAME[blk], bus, off, reg_name(blk, off), v);
        exit(2);
    }
    if (trace)
        printf("    W %s%d+0x%02x %-7s <- 0x%08x\n", BLK_NAME[blk], bus, off, reg_name(blk, off), v);
    mmio_wr(bus, blk, off, v);
}

static uint32_t rd(int bus, int blk, uint32_t off)
{
    uint32_t v;
    if ((blk == B_FMI && (off == FMI_DATA || off == FMI_META)) || (blk == B_ECC && off == ECC_RESULT))
        die("internal: FIFO registers are only read by the FIFO helpers");
    v = mmio_rd(bus, blk, off);
    if (trace > 1)
        printf("    R %s%d+0x%02x %-7s -> 0x%08x\n", BLK_NAME[blk], bus, off, reg_name(blk, off), v);
    return v;
}

static uint32_t pmgr_rd(uint32_t off)
{
    return mmio_rd(-1, 0, off);
}

static void pmgr_wr(uint32_t off, uint32_t v)
{
    if (off != GATE_FMI(0) && off != GATE_BCH(0) && off != GATE_FMI(1) && off != GATE_BCH(1))
        die("internal: PMGR write outside the four FMI gate registers");
    if (trace)
        printf("    W PMGR 0x%08x       <- 0x%08x\n", PMGR_PA + off, v);
    mmio_wr(-1, 0, off, v);
}

/* where the lock files and the state live: $K93NAND_DIR if set, else
 * /run/k93nand if it exists -- the NAND root's server holds its bus locks
 * there (initramfs/init), in the /run both roots see -- else /tmp */
static const char *state_dir(void)
{
    const char *d = getenv("K93NAND_DIR");
    struct stat st;
    if (d && *d)
        return d;
    if (!stat("/run/k93nand", &st) && S_ISDIR(st.st_mode))
        return "/run/k93nand";
    return "/tmp";
}

static const char *state_file(void)
{
    static char name[300];
    snprintf(name, sizeof(name), "%s/k93nand.state", state_dir());
    return name;
}

static uint64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000000ull + ts.tv_nsec / 1000;
}

/* h2fmi_wait_done 0x9ff05128: poll (reg & mask) == want, then write want back
 * (write-one-to-clear) -- iBEC writes it back on a timeout too.  iBEC's limit is
 * 100 ms; the status poll uses 2 s (0x1e8480). */
static int wait_bits(int bus, int blk, uint32_t off, uint32_t mask, uint32_t want, uint32_t us)
{
    uint64_t t0 = now_us();
    uint32_t v;
    int r = 0;
    while (((v = RAW(bus, blk, off)) & mask) != want)
        if (now_us() - t0 > us) {
            fprintf(stderr, "k93nand: timeout: %s%d+0x%02x %s = 0x%08x, waiting for 0x%x/0x%x\n",
                    BLK_NAME[blk], bus, off, reg_name(blk, off), v, mask, want);
            r = -1;
            break;
        }
    if (trace)
        printf("    . %s%d+0x%02x %-7s == 0x%08x (wanted 0x%x/0x%x)%s\n", BLK_NAME[blk], bus, off,
               reg_name(blk, off), v, mask, want, r ? " TIMEOUT" : "");
    wr(bus, blk, off & ~3u, want);
    return r;
}

/* h2fmi_wait_dma_task_pending 0x9ff051dc: FMI_DBG0 bits 3-4, 100 ms */
static int wait_fifo(int bus)
{
    uint64_t t0 = now_us();
    while (!(RAW(bus, B_FMI, FMI_DBG0) & 0x18))
        if (now_us() - t0 > 100000) {
            fprintf(stderr, "k93nand: FIFO timeout: FMI%d DBG0 0x%08x INTPND 0x%08x STAT 0x%08x\n", bus,
                    RAW(bus, B_FMI, FMI_DBG0), RAW(bus, B_FMI, FMI_INTPND), RAW(bus, B_FMI, FMI_STAT));
            return -1;
        }
    return 0;
}

static uint32_t fifo_word(int bus)
{
    return RAW(bus, B_FMI, FMI_DATA);
}

static uint8_t meta_byte(int bus)
{
    return mmio_rd8(bus, B_FMI, FMI_META);
}

static uint32_t ecc_result(int bus)
{
    return RAW(bus, B_ECC, ECC_RESULT);
}

/* ---- setup ------------------------------------------------------------------ */

static void *map(int fd, uint32_t pa)
{
    void *p = mmap(NULL, 0x1000, PROT_READ | PROT_WRITE, MAP_SHARED, fd, pa);
    if (p == MAP_FAILED) {
        perror("mmap /dev/mem");
        exit(1);
    }
    return p;
}

/* Only the iPad 2 Wi-Fi.  The iPad mini's NAND is PPN behind the same windows, and
 * these sequences mean something else to it -- tools/nand/nandctl is for that one. */
static void board_check(void)
{
    char buf[256];
    int fd = open("/proc/device-tree/compatible", O_RDONLY);
    ssize_t n = fd < 0 ? -1 : read(fd, buf, sizeof(buf) - 1);
    if (fd >= 0)
        close(fd);
    if (n <= 0) {
        fprintf(stderr, "k93nand: cannot read /proc/device-tree/compatible -- not running blind\n");
        exit(1);
    }
    buf[n] = 0;     /* the first string is the board */
    if (strcmp(buf, "apple,k93ap")) {
        fprintf(stderr, "k93nand: this is %s, not the iPad 2 Wi-Fi (apple,k93ap); refusing\n", buf);
        exit(1);
    }
}

static void open_hw(void)
{
#ifndef K93NAND_SIM
    board_check();
    int fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd < 0) {
        perror("/dev/mem");
        exit(1);
    }
    pmgr = map(fd, PMGR_PA);
    for (int b = 0; b < NBUS; b++)
        for (int k = 0; k < NBLK; k++)
            regs[b][k] = map(fd, FMI_PA(b) + BLK_OFF[k]);
#endif
}

static int gates_on(int bus)
{
    uint32_t a = pmgr_rd(GATE_FMI(bus)), b = pmgr_rd(GATE_BCH(bus));
    return (a & 0xff) == 0xff && (b & 0xff) == 0xff;
}

static int lock_bus(int bus)
{
    char name[300];
    snprintf(name, sizeof(name), "%s/k93nand-bus%d.lock", state_dir(), bus);
    int fd = open(name, O_RDWR | O_CREAT, 0644);
    if (fd < 0 || flock(fd, LOCK_EX | LOCK_NB)) {
        fprintf(stderr, "k93nand: FMI%d is busy (another k93nand?): %s\n", bus,
                fd < 0 ? strerror(errno) : "locked");
        return 0;
    }
    return 1;       /* held until exit */
}

/* A bus is usable when it is clocked; the timing to restore after a block reset
 * is what is in IF_CTRL now -- iBEC programmed it at boot (h2fmiInit 0x9ff0374a)
 * unless something reset the block since, in which case it is 0xffff, the slow
 * value iBEC itself starts with (h2fmi_init_minimal 0x9ff03980). */
static int claim(int bus)
{
    if (!gates_on(bus)) {
        fprintf(stderr, "k93nand: FMI%d clock gates off (0x%08x 0x%08x): run `k93nand init`\n", bus,
                pmgr_rd(GATE_FMI(bus)), pmgr_rd(GATE_BCH(bus)));
        return 0;
    }
    if (!lock_bus(bus))
        return 0;
    timing[bus] = RAW(bus, B_FMC, FMC_IFCTRL) & 0xfffff;
    if (!timing[bus])
        timing[bus] = 0xffff;
    return 1;
}

/* iBoot clock_gate 0x9ff1f128: low nibble = 0xf, wait for the status nibble */
static int gate_set_on(uint32_t off)
{
    uint32_t v = pmgr_rd(off);
    pmgr_wr(off, v | 0xf);
    for (uint64_t t0 = now_us();; ) {
        v = pmgr_rd(off);
        if (!((v ^ v >> 4) & 0xf))
            break;
        if (now_us() - t0 > 100000) {
            fprintf(stderr, "k93nand: PMGR 0x%08x stuck at 0x%08x\n", PMGR_PA + off, v);
            return -1;
        }
    }
    if (trace)
        printf("    . PMGR 0x%08x       == 0x%08x\n", PMGR_PA + off, v);
    return 0;
}

/* h2fmi_reset 0x9ff050d8: block reset (WMR_CLOCK_RESET_DEVICE -> bit 31 of the
 * FMI gate register, pulsed for one timer tick), FMI stop, FMC on in SDR, timing
 * back (h2fmi_restore_timing 0x9ff05bd4).  iBEC does this at the start of every
 * page read (fmiss 0x9ff0707e) and after every READ ID. */
static void fmi_reset(int bus)
{
    uint32_t off = GATE_FMI(bus);
    if (trace)
        printf("  block reset FMI%d (h2fmi_reset)\n", bus);
    pmgr_wr(off, pmgr_rd(off) | 0x80000000u);
    usleep(1);
    pmgr_wr(off, pmgr_rd(off) & ~0x80000000u);
    wr(bus, B_FMI, FMI_CTRL, 6);
    wr(bus, B_FMC, FMC_ON, 1);
    wr(bus, B_FMC, FMC_IFCTRL, timing[bus]);
}

/* the ISR's tail / 0x9ff05904 */
static void clear_irqs(int bus)
{
    wr(bus, B_FMC, FMC_INTMASK, 0);
    wr(bus, B_FMI, FMI_INTEN, 0);
    wr(bus, B_FMC, FMC_FSTATUS, 0x31ffff);
    wr(bus, B_FMI, FMI_INTPND, 0xf);
}

/* h2fmi_enable_chip 0x9ff059b8 / h2fmi_disable_chip 0x9ff05ce8 */
static void ce_on(int bus, int ce)
{
    wr(bus, B_FMC, FMC_CECTRL, 1u << ce);
}

static void ce_off(int bus, int ce)
{
    wr(bus, B_FMC, FMC_CECTRL, rd(bus, B_FMC, FMC_CECTRL) & ~(1u << ce));
}

static void nand_cmd(int bus, uint32_t cmd)
{
    wr(bus, B_FMC, FMC_CMD, cmd);   /* allowlist in wr() */
}

/* ---- iBEC's tables (nandid.c) ---------------------------------------------- */

/* chip table 0x9ff466d0: 12 x 28 bytes.  +0x10 is not read by iBEC's raw path
 * (inferred: the vendor's ECC requirement); +0x14 becomes the ADT bbt-format. */
struct chip {
    uint8_t id[4];
    uint16_t blocks, ppb, page, spare;
    uint32_t req, bbt, dies;
};
static const struct chip CHIPS[] = {
    { { 0xec, 0xd7, 0x94, 0x7a }, 0x1038, 0x80, 0x2000, 0x280, 0x10, 8, 1 },
    { { 0xec, 0xde, 0xd5, 0x7a }, 0x2070, 0x80, 0x2000, 0x280, 0x10, 8, 2 },
    { { 0xec, 0xd7, 0x94, 0x72 }, 0x1038, 0x80, 0x2000, 0x1b4, 0x0c, 8, 1 },
    { { 0xec, 0xde, 0xd5, 0x72 }, 0x2070, 0x80, 0x2000, 0x1b4, 0x0c, 8, 2 },
    { { 0x98, 0xe7, 0x94, 0x32 }, 0x1004, 0x80, 0x2000, 0x1c0, 0x10, 1, 1 },
    { { 0x98, 0xee, 0x95, 0x32 }, 0x2008, 0x80, 0x2000, 0x1c0, 0x18, 1, 2 },
    { { 0x2c, 0x88, 0x04, 0x4b }, 0x1000, 0x100, 0x2000, 0x1c0, 0x18, 7, 1 },
    { { 0x45, 0x48, 0x94, 0x32 }, 0x1000, 0x80, 0x2000, 0x1c0, 0x08, 9, 1 },
    { { 0x45, 0x68, 0x95, 0x32 }, 0x2000, 0x80, 0x2000, 0x1c0, 0x08, 9, 2 },
    { { 0x45, 0x2d, 0x94, 0x82 }, 0x1000, 0x100, 0x2000, 0x280, 0x10, 9, 1 },
    { { 0x45, 0x4d, 0x94, 0x82 }, 0x1000, 0x100, 0x2000, 0x280, 0x10, 9, 1 },
    { { 0x45, 0x29, 0x94, 0x32 }, 0x1000, 0x80, 0x2000, 0x280, 0x10, 9, 1 },
};

/* board table 0x9ff46864 (25 x 24) and timing table 0x9ff46abc (25 x 28), both
 * keyed by {buses 2, buses with chips, (id, CEs per bus) x 2}.  Here per (id,
 * CEs per bus, buses with chips): the VSVFL vendor type (ADT vendor-type) and the
 * 8 timing requirement bytes NandRequirementToIFCTRL 0x9ff06b7c turns into
 * IF_CTRL. */
struct board {
    uint8_t id[4], nce, nbus;
    uint32_t vendor;
    uint8_t t[8];
};
static const struct board BOARDS[] = {
    { { 0xec, 0xd7, 0x94, 0x7a }, 4, 1, 0x100014, { 0x19, 0x0c, 0x0a, 0x19, 0x0c, 0x0a, 0x14, 0x0f } },
    { { 0xec, 0xd7, 0x94, 0x7a }, 4, 2, 0x100014, { 0x19, 0x0c, 0x0a, 0x19, 0x0c, 0x0a, 0x14, 0x0f } },
    { { 0xec, 0xde, 0xd5, 0x7a }, 4, 2, 0x100014, { 0x19, 0x0c, 0x0a, 0x19, 0x0c, 0x0a, 0x14, 0x0f } },
    { { 0xec, 0xd7, 0x94, 0x72 }, 4, 2, 0x100014, { 0x1e, 0x0f, 0x0a, 0x1e, 0x0f, 0x0a, 0x19, 0x0f } },
    { { 0xec, 0xd7, 0x94, 0x72 }, 4, 1, 0x100014, { 0x1e, 0x0f, 0x0a, 0x1e, 0x0f, 0x0a, 0x19, 0x0f } },
    { { 0xec, 0xde, 0xd5, 0x72 }, 4, 1, 0x100014, { 0x1e, 0x0f, 0x0a, 0x1e, 0x0f, 0x0a, 0x19, 0x0f } },
    { { 0xec, 0xde, 0xd5, 0x72 }, 4, 2, 0x100014, { 0x1e, 0x0f, 0x0a, 0x1e, 0x0f, 0x0a, 0x19, 0x0f } },
    { { 0xec, 0xde, 0xd5, 0x72 }, 8, 2, 0x010001, { 0x1e, 0x0f, 0x0a, 0x1e, 0x0f, 0x0a, 0x19, 0x0f } },
    { { 0xec, 0xde, 0xd5, 0x72 }, 8, 1, 0x100014, { 0x1e, 0x0f, 0x0a, 0x1e, 0x0f, 0x0a, 0x19, 0x0f } },
    { { 0x98, 0xee, 0x95, 0x32 }, 8, 2, 0x010001, { 0x19, 0x0c, 0x0a, 0x19, 0x0c, 0x0a, 0x14, 0x19 } },
    { { 0x98, 0xe7, 0x94, 0x32 }, 4, 1, 0x150011, { 0x19, 0x0c, 0x0a, 0x19, 0x0c, 0x0a, 0x14, 0x19 } },
    { { 0x98, 0xe7, 0x94, 0x32 }, 4, 2, 0x150011, { 0x19, 0x0c, 0x0a, 0x19, 0x0c, 0x0a, 0x14, 0x19 } },
    { { 0x98, 0xee, 0x95, 0x32 }, 4, 1, 0x150011, { 0x19, 0x0c, 0x0a, 0x19, 0x0c, 0x0a, 0x14, 0x19 } },
    { { 0x98, 0xee, 0x95, 0x32 }, 4, 2, 0x150011, { 0x19, 0x0c, 0x0a, 0x19, 0x0c, 0x0a, 0x14, 0x19 } },
    { { 0x2c, 0x88, 0x04, 0x4b }, 2, 1, 0x120014, { 0x14, 0x0a, 0x07, 0x14, 0x0a, 0x07, 0x10, 0x0f } },
    { { 0x2c, 0x88, 0x04, 0x4b }, 2, 2, 0x120014, { 0x14, 0x0a, 0x07, 0x14, 0x0a, 0x07, 0x10, 0x0f } },
    { { 0x2c, 0x88, 0x04, 0x4b }, 4, 2, 0x120014, { 0x14, 0x0a, 0x07, 0x14, 0x0a, 0x07, 0x10, 0x0f } },
    { { 0x45, 0x4d, 0x94, 0x82 }, 4, 2, 0x150011, { 0x19, 0x0c, 0x0a, 0x19, 0x0c, 0x0a, 0x14, 0x19 } },
    { { 0x45, 0x29, 0x94, 0x32 }, 2, 1, 0x150011, { 0x14, 0x0c, 0x0a, 0x14, 0x0c, 0x0a, 0x14, 0x19 } },
    { { 0x45, 0x2d, 0x94, 0x82 }, 2, 1, 0x150011, { 0x19, 0x0c, 0x0a, 0x19, 0x0c, 0x0a, 0x14, 0x19 } },
    { { 0x45, 0x48, 0x94, 0x32 }, 4, 1, 0x150011, { 0x19, 0x0c, 0x0a, 0x19, 0x0c, 0x0a, 0x14, 0x19 } },
    { { 0x45, 0x2d, 0x94, 0x82 }, 2, 2, 0x150011, { 0x19, 0x0c, 0x0a, 0x19, 0x0c, 0x0a, 0x14, 0x19 } },
    { { 0x45, 0x48, 0x94, 0x32 }, 4, 2, 0x150011, { 0x19, 0x0c, 0x0a, 0x19, 0x0c, 0x0a, 0x14, 0x19 } },
    { { 0x45, 0x68, 0x95, 0x32 }, 4, 1, 0x150011, { 0x19, 0x0c, 0x0a, 0x19, 0x0c, 0x0a, 0x14, 0x19 } },
    { { 0x45, 0x68, 0x95, 0x32 }, 4, 2, 0x150011, { 0x19, 0x0c, 0x0a, 0x19, 0x0c, 0x0a, 0x14, 0x19 } },
};

/* ECC strength by spare bytes per 1 KB sector (table 0x9ff44094): 14-bit BCH
 * symbols, so t bits cost ceil(14 t / 8) parity bytes. */
static const uint8_t ECC_TABLE[][2] = { { 53, 30 }, { 51, 29 }, { 44, 25 }, { 28, 16 }, { 27, 15 } };

struct geom {
    const struct chip *c;
    unsigned sectors, ecc_bits, parity, ppb2, bpd, stride;
    uint32_t cfg, datsiz, ecc_cfg;
};

static int match4(const uint8_t *a, const uint8_t *b)
{
    /* iBEC 0x9ff22038: bytes compared only where both are non-zero */
    for (int i = 0; i < 4; i++)
        if (a[i] && b[i] && a[i] != b[i])
            return 0;
    return 1;
}

static int geom_for(const uint8_t *id, struct geom *g)
{
    memset(g, 0, sizeof(*g));
    for (size_t i = 0; i < sizeof(CHIPS) / sizeof(CHIPS[0]) && !g->c; i++)
        if (match4(CHIPS[i].id, id))
            g->c = &CHIPS[i];       /* the first match, as findNandInfo 0x9ff2187a */
    if (!g->c)
        return -1;
    g->sectors = g->c->page >> 10;                                  /* fmi+0x20 */
    unsigned per = (g->c->spare - META_BYTES) / g->sectors;         /* 0x9ff05c42 */
    for (size_t i = 0; i < sizeof(ECC_TABLE) / sizeof(ECC_TABLE[0]) && !g->ecc_bits; i++)
        if (per >= ECC_TABLE[i][0])
            g->ecc_bits = ECC_TABLE[i][1];
    if (!g->ecc_bits)
        return -1;
    g->parity = (14 * g->ecc_bits + 7) / 8;
    for (g->ppb2 = 1; g->ppb2 < g->c->ppb; g->ppb2 <<= 1)
        ;
    g->bpd = g->c->blocks / g->c->dies;                             /* 0x9ff04f56 */
    if (g->c->blocks & (g->c->blocks - 1))
        for (g->stride = 1; g->stride < g->bpd; g->stride <<= 1)
            ;
    else
        g->stride = g->bpd;
    g->cfg = (g->ecc_bits & 0x1f) << 3 | 5;                          /* 0x9ff05cc4 */
    g->datsiz = g->sectors | 1u << 18 | (META_BYTES & 0x3f) << 19 | (META_BYTES & 0x3f) << 25; /* 0x9ff05cd0 */
    g->ecc_cfg = 0x80000000u | (g->ecc_bits << 16 & 0x1f0000) | ((g->ecc_bits + 1) & 0x1f) << 8; /* 0x9ff05d34 */
    return 0;
}

/* linear block -> row address: dies are stride apart (0x9ff04f5a, VSVFL 0x9ff2f6a0) */
static uint32_t row_of(const struct geom *g, uint32_t blk, uint32_t pg)
{
    return ((blk / g->bpd) * g->stride + blk % g->bpd) * g->ppb2 + pg;
}

/* ---- state: what `id` found ------------------------------------------------- */

static uint32_t cemap;
static uint8_t chip_id[8];
static struct geom G;

static void save_state(void)
{
    FILE *f = fopen(state_file(), "w");
    if (!f) {
        perror(state_file());
        return;
    }
    fprintf(f, "cemap 0x%04x\nid", cemap);
    for (int i = 0; i < 8; i++)
        fprintf(f, " %02x", chip_id[i]);
    fprintf(f, "\n");
    fclose(f);
}

static int load_state(void)
{
    unsigned m, b[8];
    FILE *f = fopen(state_file(), "r");
    if (!f || fscanf(f, "cemap %x id %x %x %x %x %x %x %x %x", &m, &b[0], &b[1], &b[2], &b[3],
                     &b[4], &b[5], &b[6], &b[7]) != 9) {
        if (f)
            fclose(f);
        fprintf(stderr, "k93nand: no %s: run `k93nand id` first (it finds the ce-bitmap the\n"
                "way iBEC does; nothing addresses a CE outside it)\n", state_file());
        return -1;
    }
    fclose(f);
    cemap = m;
    for (int i = 0; i < 8; i++)
        chip_id[i] = b[i];
    if (getenv("K93NAND_CEMAP"))
        cemap = strtoul(getenv("K93NAND_CEMAP"), NULL, 0);
    if (geom_for(chip_id, &G)) {
        fprintf(stderr, "k93nand: chip %02x %02x %02x %02x is not in iBEC's table\n",
                chip_id[0], chip_id[1], chip_id[2], chip_id[3]);
        return -1;
    }
    return 0;
}

static int ce_present(int bus, int ce)
{
    if (bus < 0 || bus >= NBUS || ce < 0 || ce >= NCE_BUS || !(cemap & 1u << (bus * 8 + ce))) {
        fprintf(stderr, "k93nand: FMI%d CE%d is not in the ce-bitmap 0x%04x; refusing\n", bus, ce, cemap);
        return 0;
    }
    return 1;
}

/* ---- whitening ----------------------------------------------------------------- */

/* Metadata whitening, h2fmiInit 0x9ff03866: an LCG, 763 steps per entry; applied
 * in h2fmiReadSinglePage 0x9ff04776 as meta32[i] ^= T[(row + i) & 0xff], i < 3. */
static uint32_t WT[256];

static void whiten_init(void)
{
    uint32_t v = 0x50f4546a;
    for (int i = 0; i < 256; i++) {
        for (int k = 0; k < 763; k++)
            v = v * 0x19660d + 0x3c6ef35f;
        WT[i] = v;
    }
}

static void meta_unwhiten(const uint8_t *in, uint8_t *out, uint32_t row)
{
    uint8_t m[12];
    memset(m, 0xff, sizeof(m));
    memcpy(m, in, META_BYTES);
    for (int i = 0; i < 3; i++) {
        uint32_t w = (uint32_t)m[4 * i] | m[4 * i + 1] << 8 | m[4 * i + 2] << 16 | (uint32_t)m[4 * i + 3] << 24;
        w ^= WT[(row + i) & 0xff];
        for (int k = 0; k < 4; k++)
            m[4 * i + k] = w >> (8 * k);
    }
    memcpy(out, m, META_BYTES);
}

/* Data whitening: AES-128-CBC on the CDMA (h2fmi_setup_aes 0x9ff044c8).  FTL user
 * data: key_1, IV from the LPN; everything else: META key, IV from the row. */
static const uint8_t KEY_USER[16] = { 0xf6, 0x5d, 0xae, 0x95, 0x0e, 0x90, 0x6c, 0x42,
                                      0xb2, 0x54, 0xcc, 0x58, 0xfc, 0x78, 0xee, 0xce };
static const uint8_t KEY_META[16] = { 0x92, 0xa7, 0x42, 0xab, 0x08, 0xc9, 0x69, 0xbf,
                                      0x00, 0x6c, 0x94, 0x12, 0xd3, 0xcc, 0x79, 0xa5 };

/* IV callbacks 0x9ff04ea0 / 0x9ff05eb8 */
static void iv_for(uint32_t v, uint8_t iv[16])
{
    for (int k = 0; k < 4; k++) {
        v = v & 1 ? (v >> 1) ^ 0x80000061u : v >> 1;
        for (int j = 0; j < 4; j++)
            iv[4 * k + j] = v >> (8 * j);
    }
}

static uint8_t SBOX[256], ISBOX[256];

static uint8_t xt(uint8_t a)
{
    return (uint8_t)(a << 1) ^ (a & 0x80 ? 0x1b : 0);
}

static uint8_t gmul(uint8_t a, uint8_t b)
{
    uint8_t r = 0;
    while (b) {
        if (b & 1)
            r ^= a;
        a = xt(a);
        b >>= 1;
    }
    return r;
}

static void aes_init(void)
{
    uint8_t p = 1, q = 1;
    do {    /* walk the multiplicative group with generator 3 */
        p = p ^ xt(p);
        q ^= q << 1;
        q ^= q << 2;
        q ^= q << 4;
        if (q & 0x80)
            q ^= 0x09;
        uint8_t x = q ^ (uint8_t)(q << 1 | q >> 7) ^ (uint8_t)(q << 2 | q >> 6) ^
                    (uint8_t)(q << 3 | q >> 5) ^ (uint8_t)(q << 4 | q >> 4);
        SBOX[p] = x ^ 0x63;
    } while (p != 1);
    SBOX[0] = 0x63;
    for (int i = 0; i < 256; i++)
        ISBOX[SBOX[i]] = i;
}

static void aes_expand(const uint8_t key[16], uint8_t rk[176])
{
    uint8_t rcon = 1;
    memcpy(rk, key, 16);
    for (int i = 16; i < 176; i += 4) {
        uint8_t t[4] = { rk[i - 4], rk[i - 3], rk[i - 2], rk[i - 1] };
        if (i % 16 == 0) {
            uint8_t u = t[0];
            t[0] = SBOX[t[1]] ^ rcon;
            t[1] = SBOX[t[2]];
            t[2] = SBOX[t[3]];
            t[3] = SBOX[u];
            rcon = xt(rcon);
        }
        for (int k = 0; k < 4; k++)
            rk[i + k] = rk[i - 16 + k] ^ t[k];
    }
}

static void aes_decrypt_block(const uint8_t rk[176], uint8_t s[16])
{
    for (int i = 0; i < 16; i++)
        s[i] ^= rk[160 + i];
    for (int r = 9; r >= 0; r--) {
        uint8_t t[16];
        for (int c = 0; c < 4; c++)             /* InvShiftRows + InvSubBytes */
            for (int w = 0; w < 4; w++)
                t[4 * ((c + w) % 4) + w] = ISBOX[s[4 * c + w]];
        for (int i = 0; i < 16; i++)
            t[i] ^= rk[16 * r + i];
        if (r)
            for (int c = 0; c < 4; c++) {       /* InvMixColumns */
                uint8_t *a = t + 4 * c, b0 = a[0], b1 = a[1], b2 = a[2], b3 = a[3];
                a[0] = gmul(b0, 14) ^ gmul(b1, 11) ^ gmul(b2, 13) ^ gmul(b3, 9);
                a[1] = gmul(b0, 9) ^ gmul(b1, 14) ^ gmul(b2, 11) ^ gmul(b3, 13);
                a[2] = gmul(b0, 13) ^ gmul(b1, 9) ^ gmul(b2, 14) ^ gmul(b3, 11);
                a[3] = gmul(b0, 11) ^ gmul(b1, 13) ^ gmul(b2, 9) ^ gmul(b3, 14);
            }
        memcpy(s, t, 16);
    }
}

static void aes_cbc_decrypt(const uint8_t key[16], uint32_t seed, const uint8_t *in, uint8_t *out, size_t len)
{
    uint8_t rk[176], iv[16];
    aes_expand(key, rk);
    iv_for(seed, iv);
    for (size_t o = 0; o + 16 <= len; o += 16) {
        uint8_t b[16];
        memcpy(b, in + o, 16);
        aes_decrypt_block(rk, b);
        for (int i = 0; i < 16; i++)
            b[i] ^= iv[i];
        memcpy(iv, in + o, 16);
        memcpy(out + o, b, 16);
    }
}

/* ---- ECC results (h2fmi_rx_check_page_ecc 0x9ff0612c) ---------------------------- */

struct eccres {
    uint32_t pnd, sec[16];
    int n, clean, uecc, zero, maxfix, status;   /* status: 1 ok, 2 erased, <0 error */
};

static const char *ECC_STATUS[] = { "?", "ok", "erased" };

static void ecc_collect(int bus, int nsec, struct eccres *e)
{
    int c = 0, u = 0, z = 0, ze = 0;
    memset(e, 0, sizeof(*e));
    e->n = nsec;
    e->pnd = rd(bus, B_ECC, ECC_PND);
    e->maxfix = e->pnd >> 16 & 0x1f;
    for (int s = 0; s < nsec; s++) {
        uint32_t v = e->sec[s] = ecc_result(bus);
        if (v & 2) {
            if ((v & 0x3f00) > 0x200)
                ze++;           /* "erased" with more than 2 zero bits: not clean */
            else
                c++;
        } else if (v & 4) {
            z++;
        } else if (v & 1) {
            u++;
        }
    }
    e->clean = c;
    e->uecc = u + ze;
    e->zero = z;
    if (c == nsec)
        e->status = 2;
    else if (z == nsec)
        e->status = -0x25;      /* 0x80000025 */
    else if (u + ze)
        e->status = -0x24;      /* 0x80000024: uncorrectable */
    else
        e->status = 1;
}

static void ecc_print(const struct eccres *e)
{
    printf("ECC: PND 0x%08x (max corrected %d), page %s\n", e->pnd, e->maxfix,
           e->status > 0 ? ECC_STATUS[e->status] : e->status == -0x25 ? "all sectors flag 0x4 (0x80000025)"
                                                                      : "UNCORRECTABLE (0x80000024)");
    for (int s = 0; s < e->n; s++) {
        uint32_t v = e->sec[s];
        printf("  sector %d: 0x%08x  ", s, v);
        if (v & 2)
            printf("erased, %u zero bits%s\n", v >> 8 & 0x3f, (v & 0x3f00) > 0x200 ? " (too many: not clean)" : "");
        else if (v & 4)
            printf("flag 0x4 (meaning unknown; iBEC fails the page only when every sector has it)\n");
        else if (v & 1)
            printf("UNCORRECTABLE\n");
        else
            printf("ok, %u bits corrected\n", v >> 16 & 0x1f);
    }
}

static const char *ecc_short(const struct eccres *e, char *buf)
{
    if (e->status == 2)
        return "erased";
    if (e->status == 1) {
        if (!e->maxfix)
            return "ok";
        snprintf(buf, 16, "ok:%d", e->maxfix);
        return buf;
    }
    return e->status == -0x25 ? "flag4" : "UECC";
}

/* ---- the NAND command sequences ------------------------------------------------- */

static void dump_regs(int bus)
{
    printf("  FMI%d: CFG 0x%08x CTRL 0x%08x STAT 0x%08x INTPND 0x%08x INT_EN 0x%08x DBG0 0x%08x DATSIZ 0x%08x\n",
           bus, RAW(bus, B_FMI, 0x00), RAW(bus, B_FMI, 0x04), RAW(bus, B_FMI, 0x08), RAW(bus, B_FMI, 0x0c),
           RAW(bus, B_FMI, 0x10), RAW(bus, B_FMI, 0x1c), RAW(bus, B_FMI, 0x34));
    printf("  FMC%d: ON 0x%08x IFCTRL 0x%08x CECTRL 0x%08x RWCTRL 0x%08x CMD 0x%08x ADDR0 0x%08x "
           "ADDR1 0x%08x ANUM 0x%08x DNUM 0x%08x FSTATUS 0x%08x NSTATUS 0x%08x RBB_CFG 0x%08x\n",
           bus, RAW(bus, B_FMC, 0x00), RAW(bus, B_FMC, 0x08), RAW(bus, B_FMC, 0x0c), RAW(bus, B_FMC, 0x10),
           RAW(bus, B_FMC, 0x14), RAW(bus, B_FMC, 0x18), RAW(bus, B_FMC, 0x1c), RAW(bus, B_FMC, 0x20),
           RAW(bus, B_FMC, 0x24), RAW(bus, B_FMC, 0x44), RAW(bus, B_FMC, 0x48), RAW(bus, B_FMC, 0x4c));
    printf("  ECC%d: CFG 0x%08x PND 0x%08x MASK 0x%08x LEVEL 0x%08x\n", bus, RAW(bus, B_ECC, 0x08),
           RAW(bus, B_ECC, 0x10), RAW(bus, B_ECC, 0x14), RAW(bus, B_ECC, 0x2c));
}

/* after a failure: what iBEC would do next time anyway */
static void recover(int bus, int ce)
{
    fprintf(stderr, "k93nand: FMI%d CE%d failed; registers, then a block reset:\n", bus, ce);
    dump_regs(bus);
    ce_off(bus, ce);
    clear_irqs(bus);
    fmi_reset(bus);
}

/* h2fmi_nand_reset 0x9ff052bc */
static int nand_reset(int bus, int ce)
{
    if (trace)
        printf("  RESET FMI%d CE%d (h2fmi_nand_reset)\n", bus, ce);
    ce_on(bus, ce);
    nand_cmd(bus, 0xff);
    wr(bus, B_FMC, FMC_RWCTRL, 1);
    int r = wait_bits(bus, B_FMC, FMC_FSTATUS, 1, 1, 100000);
    ce_off(bus, ce);
    return r;
}

/* h2fmi_nand_read_id 0x9ff0531c: 90 00, 8 bytes by PIO, then a block reset */
static int nand_read_id(int bus, int ce, uint8_t id[8])
{
    uint32_t w[2] = { 0, 0 };
    int r = -1;
    if (trace)
        printf("  READ ID FMI%d CE%d (h2fmi_nand_read_id)\n", bus, ce);
    ce_on(bus, ce);
    nand_cmd(bus, 0x90);
    wr(bus, B_FMC, FMC_ADDR0, 0);
    wr(bus, B_FMC, FMC_ANUM, 0);
    wr(bus, B_FMC, FMC_RWCTRL, 9);
    if (wait_bits(bus, B_FMC, FMC_FSTATUS, 9, 9, 100000) == 0) {
        wr(bus, B_FMI, FMI_CFG, 0);
        wr(bus, B_FMI, FMI_DATSIZ, 0x801);
        wr(bus, B_FMI, FMI_CTRL, 3);
        if (wait_bits(bus, B_FMI, FMI_INTPND, 2, 2, 100000) == 0 && wait_fifo(bus) == 0) {
            w[0] = fifo_word(bus);
            w[1] = fifo_word(bus);
            r = 0;
        }
    }
    fmi_reset(bus);         /* iBEC: 0x9ff053ca, on success and failure */
    ce_off(bus, ce);
    memcpy(id, w, 8);
    return r;
}

/* the address of a page: h2fmi_set_address 0x9ff05270 -- column 0, 3 row bytes */
static void set_row(int bus, uint32_t row)
{
    wr(bus, B_FMC, FMC_ADDR1, row >> 16 & 0xff);
    wr(bus, B_FMC, FMC_ADDR0, (row & 0xffff) << 16);
    wr(bus, B_FMC, FMC_ANUM, 4);
}

/* 0x9ff06080 + h2fmi_complete_read_page_start 0x9ff060c0 / h2fmi_wait_status
 * 0x9ff0592c: 00 ADDR 30, then 70 and let the FMC poll the status byte until bit 6
 * (ready), then a bare 00 to get the data out.  iBEC sleeps on the interrupt; here
 * INTPND bit 8 (FMC event) is polled, and the ISR's clearing done by hand. */
static int start_page_read(int bus, int ce, uint32_t row, uint8_t *nstatus)
{
    int r;
    ce_on(bus, ce);
    set_row(bus, row);
    nand_cmd(bus, 0x3000);
    wr(bus, B_FMC, FMC_RWCTRL, 0xb);
    if (wait_bits(bus, B_FMC, FMC_FSTATUS, 0xb, 0xb, 100000))
        return -1;
    wr(bus, B_FMC, FMC_IFCTRL, rd(bus, B_FMC, FMC_IFCTRL) & ~0x100000u);
    wr(bus, B_FMC, FMC_RBBCFG, 0x40 | 0x40 << 8);
    nand_cmd(bus, 0x70);
    wr(bus, B_FMC, FMC_RWCTRL, 1);
    if (wait_bits(bus, B_FMC, FMC_FSTATUS, 1, 1, 100000))
        return -1;
    clear_irqs(bus);
    wr(bus, B_FMC, FMC_DNUM, 0);
    wr(bus, B_FMC, FMC_RWCTRL, 0x50);
    wr(bus, B_FMC, FMC_INTMASK, 0x20);
    wr(bus, B_FMI, FMI_INTEN, 0x100);
    /* iBEC waits for the interrupt (INTPND bit 8, the FMC event); the FMC's own
     * status-match bit (FSTATUS bit 5, what tools/nand/ppn.c polls on the mini) is
     * accepted too, whichever comes first. */
    uint64_t t0 = now_us();
    uint32_t p, f;
    while (!((p = RAW(bus, B_FMI, FMI_INTPND)) & 0x100) && !((f = RAW(bus, B_FMC, FMC_FSTATUS)) & 0x20))
        if (now_us() - t0 > 2000000)
            break;
    f = RAW(bus, B_FMC, FMC_FSTATUS);
    *nstatus = RAW(bus, B_FMC, FMC_NSTATUS);
    r = (p & 0x100) || (f & 0x20) ? 0 : -1;
    if (trace || r)
        printf("    . FMI%d+0x0c INTPND  == 0x%08x, FMC FSTATUS 0x%08x (wanted bit 8 / bit 5)%s, NAND status 0x%02x\n",
               bus, p, f, r ? " TIMEOUT" : "", *nstatus);
    clear_irqs(bus);
    wr(bus, B_FMC, FMC_RWCTRL, 0);
    if (r)
        return -1;
    nand_cmd(bus, 0x0000);
    wr(bus, B_FMC, FMC_RWCTRL, 0);
    wr(bus, B_FMC, FMC_RWCTRL, 1);
    return wait_bits(bus, B_FMC, FMC_FSTATUS, 1, 1, 100000);
}

/* The page format iBEC's raw read uses (fmiss 0x9ff0707e..0x9ff070da):
 * h2fmi_reset, then 0x9ff05d34 with flag threshold ecc_bits + 1, the pending bits
 * cleared, RW_CTRL 0, the ECC result queue cleared. */
static void page_format(int bus, uint32_t datsiz)
{
    fmi_reset(bus);
    wr(bus, B_FMI, FMI_CFG, G.cfg);
    wr(bus, B_FMI, FMI_DATSIZ, datsiz);
    wr(bus, B_ECC, ECC_CFG, G.ecc_cfg);
    wr(bus, B_FMI, FMI_INTPND, 0xffffffff);
    wr(bus, B_FMC, FMC_RWCTRL, 0);
    wr(bus, B_ECC, ECC_RESULT, 1);
}

/* One page through the BCH engine, by PIO.
 *   sector_mode 0: iBEC's page format (8 x 1 KB + 10 metadata bytes in one
 *     transfer); the CPU takes the data FIFO 8 words at a time whenever DBG0 says
 *     data is waiting -- what CDMA channel 5/7 does with its 8-word bursts -- then
 *     the 10 metadata bytes from the metadata FIFO with byte reads, then waits for
 *     the transfer-done bit.
 *   sector_mode 1: one 1 KB sector per transfer, each finished before the FIFO is
 *     read (the pattern of h2fmi_read_bootpage_pio 0x9ff05f9c); sector 0 carries
 *     the metadata (inferred layout: [1024 data][10 meta][parity] first).
 *   nsec < sectors (sector mode only): stop after nsec sectors (scan -1).
 * Returns -1 when the bus failed (registers dumped, block reset done), else 0 and
 * the ECC verdict in *e. */
static int read_page(int bus, int ce, uint32_t row, uint8_t *data, uint8_t *meta, int sector_mode,
                     unsigned nsec, struct eccres *e, uint8_t *nstatus)
{
    uint32_t *d32 = (uint32_t *)data;
    uint32_t first = (G.datsiz & ~0xffu) | 1;          /* one sector, with the metadata */
    uint32_t rest = (G.datsiz & 0x7ff00u) | 1;         /* one sector, no metadata */
    if (trace)
        printf("  READ FMI%d CE%d row 0x%06x (%s)\n", bus, ce, row, sector_mode ? "sector mode" : "page mode");
    memset(meta, 0, META_BYTES);
    page_format(bus, sector_mode ? first : G.datsiz);
    if (start_page_read(bus, ce, row, nstatus))
        goto fail;
    if (!sector_mode) {
        wr(bus, B_FMI, FMI_CTRL, 3);
        for (unsigned i = 0; i < G.sectors * SECTOR / 4; i += 8) {
            if (wait_fifo(bus))
                goto fail;
            for (int k = 0; k < 8; k++)
                d32[i + k] = fifo_word(bus);
        }
        for (int k = 0; k < META_BYTES; k++)
            meta[k] = meta_byte(bus);
        if (wait_bits(bus, B_FMI, FMI_INTPND, 2, 2, 100000))
            goto fail;
        nsec = G.sectors;
    } else {
        for (unsigned s = 0; s < nsec; s++) {
            if (s == 1)
                wr(bus, B_FMI, FMI_DATSIZ, rest);
            wr(bus, B_FMI, FMI_CTRL, 3);
            if (wait_bits(bus, B_FMI, FMI_INTPND, 2, 2, 100000) || wait_fifo(bus))
                goto fail;
            for (unsigned k = 0; k < SECTOR / 4; k++)
                d32[s * SECTOR / 4 + k] = fifo_word(bus);
            if (s == 0)
                for (int k = 0; k < META_BYTES; k++)
                    meta[k] = meta_byte(bus);
        }
    }
    uint32_t level = rd(bus, B_ECC, ECC_LEVEL) & 0x3f;
    if (trace || level < nsec)
        printf("    . ECC%d+0x2c LEVEL   == 0x%02x (%u sector results waiting, %u expected)\n", bus, level,
               level, nsec);
    ecc_collect(bus, nsec, e);
    ce_off(bus, ce);
    clear_irqs(bus);
    return 0;
fail:
    recover(bus, ce);
    return -1;
}

/* h2fmi_rx_raw 0x9ff06204, by PIO: ECC off (CFG 5), the 8 KB as 1 KB transfers
 * (iBEC: one 8 x 1 KB transfer into DMA), then the spare as one transfer (CFG 0). */
static int read_raw(int bus, int ce, uint32_t row, uint8_t *buf, uint8_t *nstatus)
{
    uint32_t *b32 = (uint32_t *)buf;
    unsigned spare = (G.c->spare + 3) & ~3u;
    if (trace)
        printf("  RAW READ FMI%d CE%d row 0x%06x, ECC off\n", bus, ce, row);
    fmi_reset(bus);
    if (start_page_read(bus, ce, row, nstatus))
        goto fail;
    wr(bus, B_FMI, FMI_CFG, 5);
    for (unsigned s = 0; s < G.sectors; s++) {
        wr(bus, B_FMI, FMI_DATSIZ, SECTOR << 8 | 1);
        wr(bus, B_FMI, FMI_CTRL, 3);
        if (wait_bits(bus, B_FMI, FMI_INTPND, 2, 2, 100000) || wait_fifo(bus))
            goto fail;
        for (unsigned k = 0; k < SECTOR / 4; k++)
            b32[s * SECTOR / 4 + k] = fifo_word(bus);
    }
    wr(bus, B_FMI, FMI_CFG, 0);
    wr(bus, B_FMI, FMI_DATSIZ, (spare << 8 & 0x7fc00) | 1);
    wr(bus, B_FMI, FMI_CTRL, 3);
    if (wait_bits(bus, B_FMI, FMI_INTPND, 2, 2, 100000) || wait_fifo(bus))
        goto fail;
    for (unsigned k = 0; k < spare / 4; k++)
        b32[G.sectors * SECTOR / 4 + k] = fifo_word(bus);
    ce_off(bus, ce);
    clear_irqs(bus);
    return 0;
fail:
    recover(bus, ce);
    return -1;
}

/* ---- output helpers ------------------------------------------------------------- */

static void hexdump(const uint8_t *p, uint32_t n, uint32_t base)
{
    for (uint32_t o = 0; o < n; o += 16) {
        printf("  %04x:", base + o);
        for (uint32_t k = o; k < o + 16 && k < n; k++)
            printf(" %02x", p[k]);
        printf("  ");
        for (uint32_t k = o; k < o + 16 && k < n; k++)
            putchar(p[k] >= 0x20 && p[k] < 0x7f ? p[k] : '.');
        printf("\n");
    }
}

static const char *meta_type(uint8_t t)
{
    switch (t) {
    case 0xff: return "erased";
    case 0x10: return "user data";
    case 0x04: return "index";
    case 0x0c: return "index-block BTOC";
    case 0x08: return "BTOC";
    case 0x20: return "FTL context";
    case 0x80: return "VSVFL context";
    }
    return "?";
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

static int parse_bus_ce(char **argv, int *bus, int *ce)
{
    *bus = atoi(argv[0]);
    *ce = atoi(argv[1]);
    return ce_present(*bus, *ce) ? 0 : -1;
}

static uint32_t parse_row(const char *s)
{
    const char *c = strchr(s, ':');
    if (!c)
        return strtoul(s, NULL, 0);
    uint32_t blk = strtoul(s, NULL, 0), pg = strtoul(c + 1, NULL, 0);
    if (blk >= G.c->blocks || pg >= G.c->ppb)
        die("block:page out of range");
    return row_of(&G, blk, pg);
}

static int write_file(const char *name, const void *a, size_t na, const void *b, size_t nb)
{
    FILE *f = fopen(name, "wb");
    if (!f || fwrite(a, 1, na, f) != na || (nb && fwrite(b, 1, nb, f) != nb) || fclose(f)) {
        perror(name);
        return -1;
    }
    printf("wrote %s (%zu bytes)\n", name, na + nb);
    return 0;
}

/* ---- commands -------------------------------------------------------------------- */

static int cmd_regs(void)
{
    for (int b = 0; b < NBUS; b++) {
        printf("FMI%d gates: PMGR 0x%08x = 0x%08x (FMI%d), 0x%08x = 0x%08x (FMI%d-BCH)\n", b,
               PMGR_PA + GATE_FMI(b), pmgr_rd(GATE_FMI(b)), b, PMGR_PA + GATE_BCH(b), pmgr_rd(GATE_BCH(b)), b);
        if (!gates_on(b)) {
            printf("  not clocked: registers not read\n");
            continue;
        }
        dump_regs(b);
        printf("  FMC%d: DQST 0x%08x NANDT1 0x%08x NANDT2 0x%08x NANDT3 0x%08x\n", b,
               RAW(b, B_FMC, 0x70), RAW(b, B_FMC, 0x74), RAW(b, B_FMC, 0x78), RAW(b, B_FMC, 0x7c));
        uint32_t ifc = RAW(b, B_FMC, FMC_IFCTRL);
        printf("  IF_CTRL fields: dc %u rs %u rh %u ws %u wh %u (clocks - 1)%s\n", ifc >> 16 & 0xf,
               ifc >> 12 & 0xf, ifc >> 8 & 0xf, ifc >> 4 & 0xf, ifc & 0xf,
               (ifc & 0xfffff) == 0xffff ? "  <- iBEC's untuned start value" : "");
    }
    return 0;
}

/* h2fmi_init_minimal 0x9ff03914 for both buses, keeping the tuned IF_CTRL */
static int cmd_init(void)
{
    int rc = 0;
    for (int b = 0; b < NBUS; b++) {
        printf("FMI%d: gates 0x%08x 0x%08x, FMC_ON 0x%08x, IF_CTRL 0x%08x before\n", b, pmgr_rd(GATE_FMI(b)),
               pmgr_rd(GATE_BCH(b)), gates_on(b) ? RAW(b, B_FMC, FMC_ON) : 0, gates_on(b) ? RAW(b, B_FMC, FMC_IFCTRL) : 0);
        uint32_t keep = gates_on(b) ? RAW(b, B_FMC, FMC_IFCTRL) & 0xfffff : 0;
        if (!lock_bus(b)) {
            rc = 1;
            continue;
        }
        printf("  clock gates on (WMR_CLOCK_GATE: iBoot ids 0x%x 0x%x)\n", 0x2f + 2 * b, 0x30 + 2 * b);
        if (gate_set_on(GATE_FMI(b)) || gate_set_on(GATE_BCH(b))) {
            rc = 1;
            continue;
        }
        timing[b] = keep && keep != 0xffff ? keep : 0xffff;
        printf("  timing to restore: 0x%05x (%s)\n", timing[b], timing[b] == 0xffff ?
               "iBEC's untuned start value; iBoot's tuned one was not there" : "what iBoot left, kept");
        fmi_reset(b);
        printf("  interrupts off and cleared (0x9ff057a4)\n");
        wr(b, B_FMC, FMC_INTMASK, 0);
        wr(b, B_FMI, FMI_INTEN, 0);
        wr(b, B_FMC, FMC_FSTATUS, 0x31ffff);
        wr(b, B_FMI, FMI_INTPND, 0xf);
        wr(b, B_ECC, ECC_MASK, 0);
    }
    return rc;
}

static const char *vendor_name(uint8_t v)
{
    switch (v) {
    case 0xec: return "Samsung";
    case 0x98: return "Toshiba";
    case 0x2c: return "Micron";
    case 0x45: return "SanDisk";
    case 0xad: return "Hynix";
    }
    return "?";
}

static int vsvfl_banks(uint32_t vendor)
{
    switch (vendor) {   /* VFL_Open 0x9ff333be */
    case 0x100014: case 0x120014: case 0x140014: case 0x150011: case 0x150010:
    case 0x160010: case 0x160011: case 0x110012: case 0x130012: case 0x010012:
        return 2;
    case 0x140013: case 0x120013: case 0x010013:
        return 4;
    case 0x010001: case 0x170015:
        return 1;
    }
    return 0;
}

/* h2fmi_reset_and_read_chipids 0x9ff05b20 + the bitmap 0x9ff059f0 + findNandInfo */
static int cmd_id(void)
{
    static const uint8_t EMPTY0[8] = { 0 }, EMPTY1[8] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0, 0, 0 };
    uint8_t ids[NBUS][NCE_BUS][8];
    uint32_t probe = getenv("K93NAND_CEMAP") ? strtoul(getenv("K93NAND_CEMAP"), NULL, 0) : 0xffff;
    int nce[NBUS] = { 0, 0 };

    memset(ids, 0, sizeof(ids));
    for (int b = 0; b < NBUS; b++) {
        if (!claim(b))
            return 1;
        printf("FMI%d: IF_CTRL 0x%05x\n", b, timing[b]);
        for (int ce = 0; ce < NCE_BUS; ce++)
            if (probe & 1u << (b * 8 + ce))
                nand_reset(b, ce);
        printf("  50 ms for the resets (0x9ff05ba6)\n");
        usleep(50000);
        for (int ce = 0; ce < NCE_BUS; ce++)
            if (probe & 1u << (b * 8 + ce) && nand_read_id(b, ce, ids[b][ce]))
                printf("  FMI%d CE%d: read ID failed\n", b, ce);
    }
    memcpy(chip_id, ids[0][0], 8);
    cemap = 0;
    printf("\nchip IDs (READ ID 90/00, 8 bytes; reference = FMI0 CE0, as iBEC):\n");
    for (int b = 0; b < NBUS; b++)
        for (int ce = 0; ce < NCE_BUS; ce++) {
            const uint8_t *id = ids[b][ce];
            const char *what;
            if (!(probe & 1u << (b * 8 + ce)))
                what = "not probed";
            else if (memcmp(chip_id, EMPTY0, 8) && !memcmp(id, chip_id, 4)) {
                cemap |= 1u << (b * 8 + ce);
                nce[b]++;
                what = "present";
            } else if (!memcmp(id, EMPTY0, 8) || !memcmp(id, EMPTY1, 8)) {
                what = "nothing";
            } else {
                what = "mismatched -- iBEC ignores it";
            }
            printf("  FMI%d CE%d: %02x %02x %02x %02x %02x %02x %02x %02x  %s\n", b, ce, id[0], id[1], id[2],
                   id[3], id[4], id[5], id[6], id[7], what);
        }
    printf("ce-bitmap 0x%04x (bit = bus * 8 + CE), %d CEs\n", cemap, nce[0] + nce[1]);
    if (!cemap) {
        printf("no NAND found\n");
        return 1;
    }
    save_state();
    printf("saved to %s\n\n", state_file());

    if (geom_for(chip_id, &G)) {
        printf("chip %02x %02x %02x %02x is NOT in iBEC's table (\"Unknown Chip ID!\"): iBEC would not boot\n",
               chip_id[0], chip_id[1], chip_id[2], chip_id[3]);
        return 1;
    }
    const struct chip *c = G.c;
    printf("%s, iBEC chip table entry %d (0x9ff466d0 + %d * 28):\n", vendor_name(chip_id[0]),
           (int)(c - CHIPS), (int)(c - CHIPS));
    printf("  %u blocks/CE, %u pages/block, %u + %u bytes/page, %u die(s)/CE, bbt-format %u, +0x10 = %u\n",
           c->blocks, c->ppb, c->page, c->spare, c->dies, c->bbt, c->req);
    printf("  rows: block b, page p -> ((b / %u) * %u + b %% %u) * %u + p\n", G.bpd, G.stride, G.bpd, G.ppb2);
    printf("  BCH: %u sectors of %u, (%u - %u) / %u = %u spare bytes each -> %u bits, %u parity bytes\n",
           G.sectors, SECTOR, c->spare, META_BYTES, G.sectors, (c->spare - META_BYTES) / G.sectors, G.ecc_bits,
           G.parity);
    printf("  page on flash (inferred): [1024 + 10 meta + %u] + %u x [1024 + %u] = %u of %u bytes\n",
           G.parity, G.sectors - 1, G.parity, G.sectors * (SECTOR + G.parity) + META_BYTES, c->page + c->spare);
    printf("  FMI_CFG 0x%02x, FMI_DATSIZ 0x%08x, ECC_CFG 0x%08x, refresh threshold %u bits\n", G.cfg, G.datsiz,
           G.ecc_cfg, G.ecc_bits >= 9 ? G.ecc_bits * 8 / 10 : 8);
    printf("  raw size %llu MB\n", (unsigned long long)c->blocks * c->ppb * c->page * (nce[0] + nce[1]) >> 20);

    int nbus_used = (nce[0] > 0) + (nce[1] > 0);
    const struct board *bd = NULL;
    if ((nce[0] && nce[1] && nce[0] != nce[1]))
        printf("CE counts differ between buses: \"Chip IDs not symmetrical!\" -- iBEC would stop\n");
    for (size_t i = 0; i < sizeof(BOARDS) / sizeof(BOARDS[0]); i++)
        if (!memcmp(BOARDS[i].id, chip_id, 4) && BOARDS[i].nbus == nbus_used &&
            BOARDS[i].nce == (nce[0] ? nce[0] : nce[1]))
            bd = &BOARDS[i];
    if (!bd) {
        printf("board/timing tables have no entry for %d bus(es) x %d CEs: \"Board support not found!\"\n",
               nbus_used, nce[0] ? nce[0] : nce[1]);
        return 0;
    }
    int bpc = vsvfl_banks(bd->vendor);
    unsigned ncetot = nce[0] + nce[1];
    printf("board entry: vendor-type 0x%06x -> VSVFL %d bank(s) per CE; timing requirement bytes", bd->vendor, bpc);
    for (int i = 0; i < 8; i++)
        printf(" %02x", bd->t[i]);
    printf("\n  (NandRequirementToIFCTRL 0x9ff06b7c turns them into IF_CTRL with the FMI clock)\n");
    printf("VSVFL: %u CEs x %d = %u banks, superblock = %u pages = %u MB\n", ncetot, bpc, ncetot * bpc,
           c->ppb * ncetot * bpc, c->ppb * ncetot * bpc * (c->page >> 10) >> 10);
    printf("virtual CEs, the VFL's CE numbers (h2fmiInitVirtToPhysMap 0x9ff039bc):");
    for (int k = 0, v = 0; k < 8; k++)
        for (int b = 0; b < NBUS; b++)
            if (cemap & 1u << (b * 8 + k))
                printf(" v%d=FMI%d:CE%d", v++, b, k);
    printf("\n");
    return 0;
}

/* virtual CE 0: CE numbers walked in step over both buses, bus 0 first */
static int vce0(int *bus, int *ce)
{
    for (int k = 0; k < 8; k++)
        for (int b = 0; b < NBUS; b++)
            if (cemap & 1u << (b * 8 + k)) {
                *bus = b;
                *ce = k;
                return 0;
            }
    return -1;
}

static void print_meta(const uint8_t *raw, uint32_t row, int erased)
{
    uint8_t m[META_BYTES];
    printf("metadata (10 bytes, as read):");
    for (int i = 0; i < META_BYTES; i++)
        printf(" %02x", raw[i]);
    printf("\n");
    if (erased) {
        printf("  erased page: not unwhitened (the VFL reports FF metadata for it, 0x9ff33ee8)\n");
        return;
    }
    if (unwhiten) {
        meta_unwhiten(raw, m, row);
        printf("unwhitened (XOR T[(0x%06x + i) & 0xff]):", row);
        for (int i = 0; i < META_BYTES; i++)
            printf(" %02x", m[i]);
        printf("\n");
    } else {
        memcpy(m, raw, META_BYTES);
    }
    printf("  lpn 0x%08x  usn 0x%08x  byte8 0x%02x  type 0x%02x (%s)\n", le32(m), le32(m + 4), m[8], m[9],
           meta_type(m[9]));
}

static int cmd_read(int argc, char **argv)
{
    static uint8_t data[MAX_PAGE] __attribute__((aligned(8))), dec[MAX_PAGE];
    uint8_t meta[META_BYTES], m[META_BYTES], nst = 0;
    int bus, ce, sector_mode = 0;
    const char *key = "auto", *out = NULL, *outd = NULL;
    struct eccres e;

    if (argc < 3 || load_state() || parse_bus_ce(argv, &bus, &ce))
        return 1;
    uint32_t row = parse_row(argv[2]);
    for (int i = 3; i < argc; i++) {
        if (!strcmp(argv[i], "-s"))
            sector_mode = 1;
        else if (!strcmp(argv[i], "-k") && i + 1 < argc)
            key = argv[++i];
        else if (!strcmp(argv[i], "-o") && i + 1 < argc)
            out = argv[++i];
        else if (!strcmp(argv[i], "-O") && i + 1 < argc)
            outd = argv[++i];
        else
            die("read: unknown option");
    }
    if (!claim(bus))
        return 1;
    if (read_page(bus, ce, row, data, meta, sector_mode, G.sectors, &e, &nst))
        return 1;
    printf("\nFMI%d CE%d row 0x%06x (block %u page %u), NAND status 0x%02x\n", bus, ce, row,
           row / G.ppb2 / G.stride * G.bpd + row / G.ppb2 % G.stride, row % G.ppb2, nst);
    ecc_print(&e);
    print_meta(meta, row, e.status == 2);
    printf("data as read, first 256 bytes:\n");
    hexdump(data, 256, 0);

    if (unwhiten)
        meta_unwhiten(meta, m, row);
    else
        memcpy(m, meta, META_BYTES);
    const uint8_t *k = NULL;
    uint32_t seed = 0;
    if (!strcmp(key, "auto")) {
        if (e.status == 2)
            key = "none";
        else if (m[9] == 0x10)
            k = KEY_USER, seed = le32(m);
        else
            k = KEY_META, seed = row;
    } else if (!strcmp(key, "meta")) {
        k = KEY_META, seed = row;
    } else if (!strncmp(key, "user:", 5)) {
        k = KEY_USER, seed = strtoul(key + 5, NULL, 0);
    } else if (strcmp(key, "none")) {
        die("-k auto | none | meta | user:LPN");
    }
    if (k) {
        aes_cbc_decrypt(k, seed, data, dec, G.c->page);
        printf("data after AES-128-CBC decrypt (%s key, IV from 0x%x), first 256 bytes:\n",
               k == KEY_USER ? "FTL user-data" : "META", seed);
        hexdump(dec, 256, 0);
    }
    if (out && write_file(out, data, G.c->page, meta, META_BYTES))
        return 1;
    if (outd && k && write_file(outd, dec, G.c->page, NULL, 0))
        return 1;
    return e.status > 0 ? 0 : 1;
}

static int cmd_raw(int argc, char **argv)
{
    static uint8_t buf[MAX_PAGE + MAX_SPARE] __attribute__((aligned(8)));
    uint8_t nst = 0;
    int bus, ce;
    if (argc < 3 || load_state() || parse_bus_ce(argv, &bus, &ce))
        return 1;
    uint32_t row = parse_row(argv[2]);
    if (!claim(bus))
        return 1;
    if (read_raw(bus, ce, row, buf, &nst))
        return 1;
    printf("\nFMI%d CE%d row 0x%06x raw (no ECC), NAND status 0x%02x; first sector + spare:\n", bus, ce, row, nst);
    hexdump(buf, 0x440, 0);
    printf("  ...\n");
    hexdump(buf + G.c->page, G.c->spare, G.c->page);
    if (argc > 3)
        return write_file(argv[3], buf, G.c->page + G.c->spare, NULL, 0) ? 1 : 0;
    return 0;
}

static int cmd_scan(int argc, char **argv)
{
    static uint8_t data[MAX_PAGE] __attribute__((aligned(8)));
    uint8_t meta[META_BYTES], m[META_BYTES], nst;
    int bus, ce, mode = 0;      /* 0 page mode, 1 sector mode, 2 sector 0 only */
    struct eccres e;
    char eb[16];

    if (argc < 4 || load_state() || parse_bus_ce(argv, &bus, &ce))
        return 1;
    uint32_t first = strtoul(argv[2], NULL, 0), n = strtoul(argv[3], NULL, 0);
    trace = 0;
    for (int i = 4; i < argc; i++)
        if (!strcmp(argv[i], "-1"))
            mode = 2;
        else if (!strcmp(argv[i], "-s"))
            mode = 1;
        else if (!strcmp(argv[i], "-v"))
            trace = 1;
        else
            die("scan: options are -1 (sector 0 only), -s (sector mode), -v (trace)");
    if (first + n > G.c->blocks || !n)
        die("scan: block range past the end of the CE");
    if (!claim(bus))
        return 1;
    printf("# FMI%d CE%d blocks %u..%u, %s; metadata %s\n# block page row ecc meta(raw) | lpn usn b8 type\n",
           bus, ce, first, first + n - 1, mode == 2 ? "sector 0 only" : mode ? "sector mode" : "page mode",
           unwhiten ? "unwhitened" : "as read");
    uint64_t t0 = now_us();
    for (uint32_t blk = first; blk < first + n; blk++)
        for (uint32_t pg = 0; pg < G.c->ppb; pg++) {
            uint32_t row = row_of(&G, blk, pg);
            if (read_page(bus, ce, row, data, meta, mode != 0, mode == 2 ? 1 : G.sectors, &e, &nst)) {
                printf("%5u %3u 0x%06x FAIL\n", blk, pg, row);
                continue;
            }
            if (unwhiten && e.status != 2)
                meta_unwhiten(meta, m, row);
            else
                memcpy(m, meta, META_BYTES);
            printf("%5u %3u 0x%06x %-6s ", blk, pg, row, ecc_short(&e, eb));
            for (int i = 0; i < META_BYTES; i++)
                printf("%02x", meta[i]);
            if (e.status == 2)
                printf(" | erased\n");
            else
                printf(" | %08x %08x %02x %02x %s\n", le32(m), le32(m + 4), m[8], m[9], meta_type(m[9]));
        }
    fprintf(stderr, "k93nand: %u pages in %.1f s\n", n * G.c->ppb, (now_us() - t0) / 1e6);
    return 0;
}

/* VFL special blocks (0x9ff2ee4c): page 0 of blocks from the top of virtual CE 0
 * down to (0x60 << floor(log2(blocks))) / 100, tag in the first 16 bytes. */
static int cmd_sig(void)
{
    static uint8_t data[MAX_PAGE] __attribute__((aligned(8))), dec[MAX_PAGE];
    static const char *const TAGS[] = { "NANDDRIVERSIGN", "DEVICEUNIQUEINFO", "DIAGCONTROLINFO", "DEVICEINFOBBT" };
    uint8_t meta[META_BYTES], nst;
    struct eccres e;
    char eb[16];
    int bus, ce, found = 0;

    if (load_state() || vce0(&bus, &ce) || !claim(bus))
        return 1;
    trace = trace > 1;
    unsigned lg = 0;
    while (1u << (lg + 1) <= G.c->blocks)
        lg++;
    unsigned low = (0x60u << lg) / 100;
    printf("virtual CE 0 = FMI%d CE%d, blocks %u down to %u, page 0 of each\n", bus, ce, G.c->blocks - 1, low);
    for (unsigned blk = G.c->blocks - 1; blk >= low && blk < G.c->blocks; blk--) {
        uint32_t row = row_of(&G, blk, 0);
        if (read_page(bus, ce, row, data, meta, 0, G.sectors, &e, &nst))
            continue;
        if (e.status == 2)
            continue;
        aes_cbc_decrypt(KEY_META, row, data, dec, G.c->page);
        for (size_t t = 0; t < sizeof(TAGS) / sizeof(TAGS[0]); t++) {
            int plain = !strncmp((char *)data, TAGS[t], 16), whit = !strncmp((char *)dec, TAGS[t], 16);
            if (!plain && !whit)
                continue;
            const uint8_t *p = plain ? data : dec;
            found++;
            printf("block %u row 0x%06x: %s, %s (ECC %s); payload length %u, payload u32:", blk, row, TAGS[t],
                   plain ? "PLAIN -> data whitening off" : "under META-key AES -> data whitening on",
                   ecc_short(&e, eb), le32(p + 0x34));
            for (int i = 0; i < 4; i++)
                printf(" %08x", le32(p + 0x38 + 4 * i));
            printf("\n");
            if (t == 0)
                printf("  signature 0x%08x (iBEC expects 0x43313100: bytes 00 '1' '1' 'C'), word1 0x%08x: "
                       "metadata whitening %s, minor version %u\n", le32(p + 0x38), le32(p + 0x3c),
                       le32(p + 0x3c) & 0x10000 ? "ON" : "off", le32(p + 0x3c) & 0xff);
        }
    }
    printf("%d special block(s) found\n", found);
    return found ? 0 : 1;
}

/* No hardware: the whitening table, the IV and the AES against known answers. */
static int cmd_selftest(void)
{
    static const uint8_t K[16] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 };
    static const uint8_t C[16] = { 0x69, 0xc4, 0xe0, 0xd8, 0x6a, 0x7b, 0x04, 0x30,
                                   0xd8, 0xcd, 0xb7, 0x80, 0x70, 0xb4, 0xc5, 0x5a };
    uint8_t rk[176], b[16], iv[16];
    int bad = 0;

    printf("whitening table T[0..3] %08x %08x %08x %08x, T[255] %08x (expect ac1f1d93 5d7600d0 27056531 "
           "b5e15d06, fb86536a)\n", WT[0], WT[1], WT[2], WT[3], WT[255]);
    bad |= WT[0] != 0xac1f1d93 || WT[255] != 0xfb86536a;
    iv_for(1, iv);
    printf("IV(1) =");
    for (int i = 0; i < 16; i++)
        printf(" %02x", iv[i]);
    printf("  (expect 61 00 00 80 51 00 00 c0 49 00 00 e0 45 00 00 f0)\n");
    bad |= iv[0] != 0x61 || iv[4] != 0x51 || iv[15] != 0xf0;
    aes_expand(K, rk);
    memcpy(b, C, 16);
    aes_decrypt_block(rk, b);
    printf("AES-128 FIPS-197 C.1 decrypt:");
    for (int i = 0; i < 16; i++)
        printf(" %02x", b[i]);
    printf("  (expect 00 11 22 .. ff)\n");
    for (int i = 0; i < 16; i++)
        bad |= b[i] != (uint8_t)(i * 0x11);
    printf("%s\n", bad ? "SELFTEST FAILED" : "selftest ok");
    return bad;
}

static void usage(void)
{
    fprintf(stderr,
            "usage: k93nand [-q|-v|-W] regs | init | id | sig | selftest\n"
            "       k93nand [-q|-v|-W] read BUS CE PAGE [-s] [-k auto|none|meta|user:LPN] [-o FILE] [-O FILE]\n"
            "       k93nand [-q|-v|-W] raw BUS CE PAGE [FILE]\n"
            "       k93nand [-W] scan BUS CE FIRSTBLOCK NBLOCKS [-1|-s] [-v]\n"
            "  PAGE = row address (block * pages + page, dies stride apart) or BLOCK:PAGE\n"
            "  -q no register trace, -v trace reads too, -W leave the metadata whitened\n");
}

int main(int argc, char **argv)
{
    int i = 1;
    for (; i < argc && argv[i][0] == '-'; i++)
        if (!strcmp(argv[i], "-q"))
            trace = 0;
        else if (!strcmp(argv[i], "-v"))
            trace = 2;
        else if (!strcmp(argv[i], "-W"))
            unwhiten = 0;
        else {
            usage();
            return 1;
        }
    if (i >= argc) {
        usage();
        return 1;
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    whiten_init();
    aes_init();
    if (!strcmp(argv[i], "selftest"))
        return cmd_selftest();
    open_hw();
    const char *c = argv[i];
    argc -= i + 1;
    argv += i + 1;
    if (!strcmp(c, "regs"))
        return cmd_regs();
    if (!strcmp(c, "init"))
        return cmd_init();
    if (!strcmp(c, "id"))
        return cmd_id();
    if (!strcmp(c, "read"))
        return cmd_read(argc, argv);
    if (!strcmp(c, "raw"))
        return cmd_raw(argc, argv);
    if (!strcmp(c, "scan"))
        return cmd_scan(argc, argv);
    if (!strcmp(c, "sig"))
        return cmd_sig();
    usage();
    return 1;
}
