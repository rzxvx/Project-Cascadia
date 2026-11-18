/* k93nandw -- the iPad 2's raw NAND, WRITE side: erase and program the blocks
 * Linux may own in iOS's VSVFL reserved pool, and the hardware tests that prove
 * the recipe first.  docs/research/k93-nand-write.md: section 3.4 is the
 * recipe, 5.5 the rules for where, 8 the tests T0-T5.
 *
 *   k93nandw selftest                       no hardware: AES encryption, whitening,
 *                                           the test pattern
 *   k93nandw wp                             T0: READ STATUS on every CE with FMC IFCTRL
 *                                           bit 21 clear, set, clear (is it WP#?)
 *   k93nandw pool [-e] [-d DIR] [VCE...]    T1: every copy of every VSVFL context of each
 *                                           virtual CE, the special blocks, the window
 *                                           of 5.5 -> the blocks Linux may use.  -e reads
 *                                           each of them (256 pages: erased? ours?), -d
 *                                           saves one decrypted copy per context set
 *                                           (+ the special blocks) for k93-pool-plan.py
 *   k93nandw dryrun VCE BLOCK [-m pre|mid|post] [-r dbg|none]
 *                                           T2: the program sequence of the block's
 *                                           page 0 with WP# held and without 10h, then
 *                                           FFh and a block reset: nothing is programmed
 *   k93nandw erase VCE BLOCK                T3/T5: erase one block of the window
 *   k93nandw program VCE BLOCK FIRST N [SEED]
 *                                           T4: pages FIRST..FIRST+N-1, in order, with
 *                                           the test pattern; each read back at once
 *   k93nandw check VCE BLOCK [SEED]         read a block back: erased / pattern / ECC
 *
 * VCE is the VFL's virtual CE (k93nand id prints them: v0=FMI0:CE0, v1=FMI1:CE0,
 * v2=FMI0:CE2, v3=FMI1:CE2 on the 32 GB iPad 2).
 *
 * Safety, structurally:
 *  - the reads are k93nand.c's (its allowlist: no program/erase command exists
 *    there).  The write path has its own register function, wrw(), which refuses
 *    everything unless one block is ARMED, and arm() is only reached after the
 *    guard: this process re-reads every copy of the CE's VSVFL context, the
 *    special blocks and the block itself, and the block must be a free slot of
 *    the window in every copy (5.5, checks 1-4), erased or already Linux's;
 *  - wrw() sends 80h only with ANUM 4 and a row inside the armed block, 60h/D0h
 *    only with ANUM 2 and the armed block's first row, 10h only in a real
 *    program, all checked against the FMC's own registers read back at that
 *    moment, and only to the armed CE (CECTRL read back);
 *  - WP# (IFCTRL bit 21, if T0 says so) is released only inside one erase or
 *    program and never in a dry run; every block reset (k93nand.c's fmi_reset)
 *    writes IFCTRL without it;
 *  - Linux's pages carry their own magic and metadata type (T_LINUX), none of
 *    Apple's, so a later run can tell them from iOS's.
 *
 * Build (from devices/a5):
 *   docker run --rm --user "$(id -u):$(id -g)" -v "$PWD":/cascadia -w /cascadia \
 *       cascadia-build arm-linux-gnueabihf-gcc -static -O2 -Wall -Wno-unused-function \
 *       -pthread -D_FILE_OFFSET_BITS=64 -o build/k93/tools/k93nandw tools/nand/k93nandw.c
 */
#define K93IOSNAND_LIB
#include "k93iosnand.c"

#define T_LINUX         0x4c            /* metadata type byte of Linux's pages ('L') */
static const uint8_t LMAGIC[16] = { 'C', 'a', 's', 'c', 'a', 'd', 'i', 'a', 'K', '9', '3', 'L', 'i', 'n', 'u', 'x' };

/* the window of k93-nand-write.md 5.5, in reserved-pool slots per bank */
static const unsigned WIN_LO[2] = { 48, 48 }, WIN_HI[2] = { 87, 63 };
#define POOL_START      1952            /* VFL_Format's, for 4096 blocks and 2 banks */
#define MARGIN          8               /* free slots kept between iOS's clusters and the window */
#define FREE_SLOT       0xfff0
#define NSLOT_MAX       192

static const char *const TAGS[] = { "NANDDRIVERSIGN", "DEVICEUNIQUEINFO", "DIAGCONTROLINFO", "DEVICEINFOBBT" };

/* ---- AES-128 encryption (k93nand.c has the decryption) ------------------------ */

static uint8_t RK_META[176];

static void aes_encrypt_block(const uint8_t rk[176], uint8_t s[16])
{
    uint8_t t[16];
    for (int i = 0; i < 16; i++)
        s[i] ^= rk[i];
    for (int r = 1; r <= 10; r++) {
        for (int c = 0; c < 4; c++)             /* SubBytes + ShiftRows */
            for (int w = 0; w < 4; w++)
                t[4 * c + w] = SBOX[s[4 * ((c + w) % 4) + w]];
        if (r < 10)
            for (int c = 0; c < 4; c++) {       /* MixColumns */
                uint8_t *a = t + 4 * c, b0 = a[0], b1 = a[1], b2 = a[2], b3 = a[3], x = b0 ^ b1 ^ b2 ^ b3;
                a[0] = b0 ^ x ^ xt(b0 ^ b1);
                a[1] = b1 ^ x ^ xt(b1 ^ b2);
                a[2] = b2 ^ x ^ xt(b2 ^ b3);
                a[3] = b3 ^ x ^ xt(b3 ^ b0);
            }
        for (int i = 0; i < 16; i++)
            s[i] = t[i] ^ rk[16 * r + i];
    }
}

/* The same with four 1 KB tables (rijndael-alg-fst.c's Te0-Te3), as
 * k93iosnand's decryption: the byte-wise version above took ~2 ms of an 8 KB
 * page on the iPad, more than programming it.  The selftest checks one
 * against the other. */
static uint32_t Te[4][256], EK_META[44];

static void aese_init(void)
{
    uint8_t rk[176];
    for (int x = 0; x < 256; x++) {
        uint8_t v = SBOX[x];
        uint32_t w = (uint32_t)xt(v) << 24 | (uint32_t)v << 16 | (uint32_t)v << 8 | (uint8_t)(xt(v) ^ v);
        Te[0][x] = w;
        Te[1][x] = w >> 8 | w << 24;
        Te[2][x] = w >> 16 | w << 16;
        Te[3][x] = w >> 24 | w << 8;
    }
    aes_expand(KEY_META, rk);
    for (int i = 0; i < 44; i++)
        EK_META[i] = GETU32(rk + 4 * i);
}

static void aese_block(const uint32_t *rk, const uint8_t *in, uint8_t *out)
{
    uint32_t s0 = GETU32(in) ^ rk[0], s1 = GETU32(in + 4) ^ rk[1];
    uint32_t s2 = GETU32(in + 8) ^ rk[2], s3 = GETU32(in + 12) ^ rk[3], t0, t1, t2, t3;
    for (int r = 1; r < 10; r++) {
        rk += 4;
        t0 = Te[0][s0 >> 24] ^ Te[1][s1 >> 16 & 0xff] ^ Te[2][s2 >> 8 & 0xff] ^ Te[3][s3 & 0xff] ^ rk[0];
        t1 = Te[0][s1 >> 24] ^ Te[1][s2 >> 16 & 0xff] ^ Te[2][s3 >> 8 & 0xff] ^ Te[3][s0 & 0xff] ^ rk[1];
        t2 = Te[0][s2 >> 24] ^ Te[1][s3 >> 16 & 0xff] ^ Te[2][s0 >> 8 & 0xff] ^ Te[3][s1 & 0xff] ^ rk[2];
        t3 = Te[0][s3 >> 24] ^ Te[1][s0 >> 16 & 0xff] ^ Te[2][s1 >> 8 & 0xff] ^ Te[3][s2 & 0xff] ^ rk[3];
        s0 = t0, s1 = t1, s2 = t2, s3 = t3;
    }
    rk += 4;
#define SB(x, n)        ((uint32_t)SBOX[(x) >> (n) & 0xff] << (n))
    t0 = (SB(s0, 24) | SB(s1, 16) | SB(s2, 8) | SB(s3, 0)) ^ rk[0];
    t1 = (SB(s1, 24) | SB(s2, 16) | SB(s3, 8) | SB(s0, 0)) ^ rk[1];
    t2 = (SB(s2, 24) | SB(s3, 16) | SB(s0, 8) | SB(s1, 0)) ^ rk[2];
    t3 = (SB(s3, 24) | SB(s0, 16) | SB(s1, 8) | SB(s2, 0)) ^ rk[3];
#undef SB
    uint32_t t[4] = { t0, t1, t2, t3 };
    for (int i = 0; i < 16; i++)
        out[i] = t[i / 4] >> (24 - 8 * (i % 4));
}

/* the META key, the IV from seed (the row): what every Linux page gets */
static void aese_cbc_meta(uint32_t seed, const uint8_t *in, uint8_t *out, size_t len)
{
    uint8_t iv[16], b[16];
    iv_for(seed, iv);
    for (size_t o = 0; o + 16 <= len; o += 16) {
        for (int i = 0; i < 16; i++)
            b[i] = in[o + i] ^ iv[i];
        aese_block(EK_META, b, out + o);
        memcpy(iv, out + o, 16);
    }
}

/* what the CDMA's AES filter does on the way to the FIFO (h2fmi_setup_aes, encrypt) */
static void aes_cbc_encrypt(const uint8_t rk[176], uint32_t seed, const uint8_t *in, uint8_t *out, size_t len)
{
    uint8_t iv[16], b[16];
    iv_for(seed, iv);
    for (size_t o = 0; o + 16 <= len; o += 16) {
        for (int i = 0; i < 16; i++)
            b[i] = in[o + i] ^ iv[i];
        aes_encrypt_block(rk, b);
        memcpy(out + o, b, 16);
        memcpy(iv, b, 16);
    }
}

/* ---- Linux's test pattern ---------------------------------------------------------
 * page: LMAGIC, then u32 version 1, vCE, block, page, seed, then xorshift32 words;
 * metadata (12 bytes, 10 stored): u32 block << 8 | page, u32 seed, 0x00, T_LINUX. */
static void pattern(uint32_t seed, int vce, uint32_t blk, uint32_t pg, uint8_t *page, uint8_t meta[12])
{
    uint32_t *w = (uint32_t *)page, x = seed ^ (blk << 8 | pg) * 0x9e3779b9u ^ (uint32_t)vce << 28;
    unsigned n = G.c->page / 4;
    if (!x)
        x = 0x93;
    memcpy(page, LMAGIC, 16);
    w[4] = 1;
    w[5] = vce;
    w[6] = blk;
    w[7] = pg;
    w[8] = seed;
    for (unsigned i = 9; i < n; i++) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        w[i] = x;
    }
    memset(meta, 0xff, 12);
    uint32_t l = blk << 8 | pg;
    memcpy(meta, &l, 4);
    memcpy(meta + 4, &seed, 4);
    meta[8] = 0x00;
    meta[9] = T_LINUX;
}

/* is this decrypted page 0 + unwhitened metadata Linux's, for this block? */
static int is_ours(const uint8_t *dec, const uint8_t *m, int vce, uint32_t blk)
{
    const uint32_t *w = (const uint32_t *)dec;
    return m[9] == T_LINUX && !memcmp(dec, LMAGIC, 16) && w[4] == 1 && w[5] == (uint32_t)vce && w[6] == blk &&
           w[7] == 0;
}

/* ---- the write path's register access ---------------------------------------------- */

enum { OP_NONE, OP_DRY, OP_PROGRAM, OP_ERASE };
static const char *const OP_NAME[] = { "none", "dry run", "program", "erase" };
static struct {
    int op, vce, bus, ce;
    uint32_t blk, row0;
} A;

static void refuse(int bus, int blk, uint32_t off, uint32_t v, const char *why)
{
    fprintf(stderr, "k93nandw: REFUSING %s%d+0x%02x (%s) <- 0x%08x: %s (armed: %s, vCE %d block %u)\n",
            BLK_NAME[blk], bus, off, reg_name(blk, off), v, why, OP_NAME[A.op], A.vce, A.blk);
    if (bus >= 0 && bus < NBUS) {
        dump_regs(bus);
        fmi_reset(bus);         /* IFCTRL back to the timing alone: WP# held */
    }
    exit(3);
}

/* the row the FMC's address registers hold now, read back from the hardware */
static const char *addr_check(int bus, uint32_t anum)
{
    uint32_t n = RAW(bus, B_FMC, FMC_ANUM), a0 = RAW(bus, B_FMC, FMC_ADDR0), a1 = RAW(bus, B_FMC, FMC_ADDR1);
    uint32_t cs = RAW(bus, B_FMC, FMC_CECTRL), row;
    if (n != anum)
        return "address length is not the operation's";
    if (cs != 1u << A.ce)
        return "chip select is not the armed CE";
    if (anum == 4) {
        if (a0 & 0xffff)
            return "column address not 0";
        row = (a1 & 0xff) << 16 | a0 >> 16;
        if (row < A.row0 || row >= A.row0 + G.c->ppb)
            return "row outside the armed block";
    } else {
        row = a0 & 0xffffff;
        if (row != A.row0)
            return "erase row is not the armed block's";
    }
    return NULL;
}

static void wrw(int bus, int blk, uint32_t off, uint32_t v)
{
    const char *why = NULL;
    int prog = A.op == OP_PROGRAM || A.op == OP_DRY;
    if (A.op == OP_NONE || bus != A.bus) {
        why = "the write path is not armed for this bus";
    } else if (blk == B_FMC && off == FMC_CMD) {
        if (v == 0x0080)
            why = !prog ? "80h outside a program" : addr_check(bus, 4);
        else if (v == 0x1000)
            why = A.op != OP_PROGRAM ? "10h outside a real program" : NULL;
        else if (v == 0xd060)
            why = A.op != OP_ERASE ? "60h/D0h outside an erase" : addr_check(bus, 2);
        else
            why = "not a command of the write path";
    } else if (blk == B_FMC && off == FMC_RWCTRL) {
        uint32_t cmd = RAW(bus, B_FMC, FMC_CMD);
        if (v == 9)
            why = !prog || cmd != 0x0080 ? "cmd1 + address cycles without 80h" : addr_check(bus, 4);
        else if (v == 2)
            why = A.op != OP_PROGRAM || cmd != 0x1000 ? "cmd2 cycle without 10h" : NULL;
        else if (v == 0xb)
            why = A.op != OP_ERASE || cmd != 0xd060 ? "erase cycles without 60h/D0h" : addr_check(bus, 2);
        else
            why = "cycle pattern not in the write path";
    } else if (blk == B_FMC && off == FMC_IFCTRL) {
        if (v & 0x200000 && A.op == OP_DRY)
            why = "WP# release in a dry run";
    } else if (blk == B_FMC && (off == FMC_ADDR0 || off == FMC_ADDR1 || off == FMC_ANUM)) {
        /* checked when the command goes out */
    } else if (blk == B_FMI && off == FMI_CTRL) {
        why = !prog ? "FIFO direction outside a program" : v != 4 && v != 5 ? "FMI CTRL value" : NULL;
    } else if ((blk == B_FMI && (off == FMI_CFG || off == FMI_DATSIZ || off == FMI_INTEN)) ||
               (blk == B_ECC && (off == ECC_CFG || off == ECC_PND))) {
        /* the page format */
    } else {
        why = "register not in the write path";
    }
    if (why)
        refuse(bus, blk, off, v, why);
    if (trace)
        printf("    W %s%d+0x%02x %-7s <- 0x%08x   (write path)\n", BLK_NAME[blk], bus, off, reg_name(blk, off), v);
    mmio_wr(bus, blk, off, v);
}

/* the two FIFOs, host -> NAND */
static void fifo_put_words(int bus, const uint32_t *w, unsigned n)
{
    if ((A.op != OP_PROGRAM && A.op != OP_DRY) || bus != A.bus)
        refuse(bus, B_FMI, FMI_DATA, w[0], "data FIFO outside a program");
    for (unsigned i = 0; i < n; i++)
        regs[bus][B_FMI][FMI_DATA / 4] = w[i];
}

static void fifo_put_meta(int bus, const uint8_t *m)
{
    if ((A.op != OP_PROGRAM && A.op != OP_DRY) || bus != A.bus)
        refuse(bus, B_FMI, FMI_META, m[0], "metadata FIFO outside a program");
    if (trace)
        printf("    W FMI%d+0x18 META    <- %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x (8-bit)\n", bus, m[0],
               m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8], m[9]);
    for (int i = 0; i < META_BYTES; i++)
        ((volatile uint8_t *)regs[bus][B_FMI])[FMI_META] = m[i];
}

/* the blocks a pool check in this process cleared for writing (guard(), or
 * k93lv's start); arm() takes no other */
static uint8_t armable[16][4096 / 8];

static void allow_block(int vce, uint32_t blk)
{
    if (vce >= 0 && vce < 16 && blk < 4096)
        armable[vce][blk / 8] |= 1u << (blk % 8);
}

static void arm(int op, int vce, uint32_t blk)
{
    if (vce < 0 || vce >= 16 || blk >= 4096 || !(armable[vce][blk / 8] >> (blk % 8) & 1)) {
        fprintf(stderr, "k93nandw: REFUSING to arm vCE %d block %u: no pool check in this process cleared it\n",
                vce, blk);
        exit(3);
    }
    A.op = op;
    A.vce = vce;
    A.bus = vce_bus[vce];
    A.ce = vce_ce[vce];
    A.blk = blk;
    A.row0 = row_of(&G, blk, 0);
}

static void disarm(void)
{
    A.op = OP_NONE;
}

/* the write path's address cycles: column 0, 3 row bytes (h2fmi_set_page_address 0x3428) */
static void set_row_w(int bus, uint32_t row)
{
    wrw(bus, B_FMC, FMC_ADDR1, row >> 16 & 0xff);
    wrw(bus, B_FMC, FMC_ADDR0, (row & 0xffff) << 16);
    wrw(bus, B_FMC, FMC_ANUM, 4);
}

static void dbg(int bus, const char *when)
{
    printf("    : %-28s DBG0 0x%08x STAT 0x%08x INTPND 0x%08x CTRL 0x%08x FSTATUS 0x%08x\n", when,
           RAW(bus, B_FMI, FMI_DBG0), RAW(bus, B_FMI, FMI_STAT), RAW(bus, B_FMI, FMI_INTPND),
           RAW(bus, B_FMI, FMI_CTRL), RAW(bus, B_FMC, FMC_FSTATUS));
}

/* h2fmi_wait_status 0x39f4 (= k93nand.c start_page_read's status half): 70h, the
 * FMC polls the status byte until bit 6 (ready), 2 s */
static int wait_status(int bus, uint8_t *st)
{
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
    uint64_t t0 = now_us();
    uint32_t p = 0, f = 0;
    while (!((p = RAW(bus, B_FMI, FMI_INTPND)) & 0x100) && !((f = RAW(bus, B_FMC, FMC_FSTATUS)) & 0x20))
        if (now_us() - t0 > 2000000)
            break;
    f = RAW(bus, B_FMC, FMC_FSTATUS);
    *st = RAW(bus, B_FMC, FMC_NSTATUS);
    int r = (p & 0x100) || (f & 0x20) ? 0 : -1;
    if (trace || r)
        printf("    . status after %.3f ms: INTPND 0x%08x FSTATUS 0x%08x%s, NAND status 0x%02x\n",
               (now_us() - t0) / 1e3, p, f, r ? " TIMEOUT" : "", *st);
    clear_irqs(bus);
    wr(bus, B_FMC, FMC_RWCTRL, 0);
    return r;
}

static int status_read(int bus, int ce, uint8_t *st)
{
    ce_on(bus, ce);
    int r = wait_status(bus, st);
    ce_off(bus, ce);
    return r;
}

/* DBG0 bits 3-4: room for a burst in the TX direction, as h2fmi_tx_bootpage_pio
 * 0x4ad8 waits for them before writing (k93-nand-write.md 3.4, [?] until T2) */
static int wait_room(int bus)
{
    uint64_t t0 = now_us();
    while (!(RAW(bus, B_FMI, FMI_DBG0) & 0x18))
        if (now_us() - t0 > 100000) {
            fprintf(stderr, "k93nandw: no FIFO room: FMI%d DBG0 0x%08x INTPND 0x%08x STAT 0x%08x\n", bus,
                    RAW(bus, B_FMI, FMI_DBG0), RAW(bus, B_FMI, FMI_INTPND), RAW(bus, B_FMI, FMI_STAT));
            return -1;
        }
    return 0;
}

/* after a failure before 10h: controller block reset, then FFh to the chip
 * (drops the page register, aborts the data input), then its status */
static void abort_program(int bus, int ce)
{
    uint8_t st = 0;
    fprintf(stderr, "k93nandw: FMI%d CE%d: aborting before 10h; registers, block reset, FFh:\n", bus, ce);
    dump_regs(bus);
    ce_off(bus, ce);
    clear_irqs(bus);
    fmi_reset(bus);
    nand_reset(bus, ce);
    status_read(bus, ce, &st);
    fprintf(stderr, "k93nandw: status after the reset 0x%02x\n", st);
}

/* ---- program and erase (k93-nand-write.md 3.4) --------------------------------------- */

enum { META_PRE, META_MID, META_POST };

/* h2fmi_write_multi 0x48f4 for one page, the CPU in the place of CDMA channels 5
 * (data) and 6 (metadata).  data: the 8192 bytes as they go to the FIFO (already
 * AES'd), meta: the 10 bytes as they go (already whitened).  In a dry run (armed
 * OP_DRY) WP# stays held, 10h is never preloaded or sent, and FFh ends it.
 * Returns 0, -EROFS (status bit 7 clear: still protected), -EIO (status bit 0:
 * program failed), -ETIMEDOUT (the bus; nothing programmed if before 10h). */
static int program_page(int bus, int ce, uint32_t row, const uint8_t *data, const uint8_t *meta, uint8_t *nst,
                        int meta_mode, int room, int verbose)
{
    const uint32_t *d32 = (const uint32_t *)data;
    unsigned nw = G.sectors * SECTOR / 4;
    int dry = A.op == OP_DRY;
    *nst = 0;
    if (trace)
        printf("  %s FMI%d CE%d row 0x%06x (block %u page %u), metadata %s\n", dry ? "DRY-RUN PROGRAM" : "PROGRAM",
               bus, ce, row, row / G.ppb2, row % G.ppb2,
               meta_mode == META_PRE ? "before the data" : meta_mode == META_MID ? "after sector 0" : "after the data");
    fmi_reset(bus);
    uint32_t ifc = rd(bus, B_FMC, FMC_IFCTRL);
    wr(bus, B_FMI, FMI_INTPND, 0xffffffff);
    if (verbose)
        dbg(bus, "after the block reset");
    if (!dry)
        wrw(bus, B_FMC, FMC_IFCTRL, ifc | 0x200000);
    wrw(bus, B_FMI, FMI_CFG, G.cfg);                    /* 0x3d14 */
    wrw(bus, B_FMI, FMI_DATSIZ, G.datsiz);
    wrw(bus, B_ECC, ECC_CFG, G.ecc_cfg);
    wrw(bus, B_ECC, ECC_PND, 0x1a8);                    /* 0x3464 */
    wrw(bus, B_FMI, FMI_INTEN, 2);
    wrw(bus, B_FMI, FMI_CTRL, 4);
    if (verbose)
        dbg(bus, "after CTRL 4");
    if (meta_mode == META_PRE) {
        fifo_put_meta(bus, meta);
        if (verbose)
            dbg(bus, "after the metadata");
    }
    ce_on(bus, ce);
    set_row_w(bus, row);
    wrw(bus, B_FMC, FMC_CMD, 0x0080);
    wrw(bus, B_FMC, FMC_RWCTRL, 9);
    if (wait_bits(bus, B_FMC, FMC_FSTATUS, 9, 9, 100000))
        goto abort;
    wrw(bus, B_FMI, FMI_INTEN, 2);
    wrw(bus, B_FMI, FMI_CTRL, 5);
    if (!dry)
        wrw(bus, B_FMC, FMC_CMD, 0x1000);
    if (verbose)
        dbg(bus, "after CTRL 5");
    for (unsigned i = 0; i < nw; i += 8) {
        if (meta_mode == META_MID && i == SECTOR / 4) {
            fifo_put_meta(bus, meta);
            if (verbose)
                dbg(bus, "after the metadata");
        }
        if (room && wait_room(bus)) {
            fprintf(stderr, "k93nandw: at data word %u of %u\n", i, nw);
            goto abort;
        }
        fifo_put_words(bus, d32 + i, 8);
        if (verbose && (i + 8) % 256 == 0) {
            char w[40];
            snprintf(w, sizeof(w), "after %u data words", i + 8);
            dbg(bus, w);
        }
    }
    if (trace)
        printf("    W FMI%d+0x14 DATA    <- %u words (%s)\n", bus, nw, room ? "8 at a time, DBG0 & 0x18 first" :
                                                                       "no room check");
    if (meta_mode == META_POST) {
        fifo_put_meta(bus, meta);
        if (verbose)
            dbg(bus, "after the metadata");
    }
    if (wait_bits(bus, B_FMI, FMI_INTPND, 2, 2, 100000))
        goto abort;
    if (verbose)
        dbg(bus, "transfer done");
    clear_irqs(bus);
    if (dry) {
        printf("  dry run: no 10h; FFh to drop the page register\n");
        if (verbose)
            dump_regs(bus);
        ce_off(bus, ce);
        nand_reset(bus, ce);
        fmi_reset(bus);
        status_read(bus, ce, nst);
        return 0;
    }
    wrw(bus, B_FMC, FMC_RWCTRL, 2);                     /* 10h: the chip programs */
    int r = wait_bits(bus, B_FMC, FMC_FSTATUS, 2, 2, 100000) || wait_status(bus, nst);
    wr(bus, B_FMC, FMC_RWCTRL, 0);
    ce_off(bus, ce);
    wrw(bus, B_FMC, FMC_IFCTRL, ifc);                   /* WP# back */
    if (r) {
        recover(bus, ce);
        return -ETIMEDOUT;
    }
    if (!(*nst & 0x80))
        return -EROFS;
    return *nst & 1 ? -EIO : 0;
abort:
    abort_program(bus, ce);
    return -ETIMEDOUT;
}

/* h2fmi_erase_blocks 0x36c4 for one block */
static int erase_block(int bus, int ce, uint32_t blk, uint8_t *nst)
{
    *nst = 0;
    if (trace)
        printf("  ERASE FMI%d CE%d block %u (row 0x%06x)\n", bus, ce, blk, row_of(&G, blk, 0));
    fmi_reset(bus);
    uint32_t ifc = rd(bus, B_FMC, FMC_IFCTRL);
    wr(bus, B_FMI, FMI_INTPND, 0xffffffff);
    wrw(bus, B_FMC, FMC_IFCTRL, ifc | 0x200000);
    ce_on(bus, ce);
    wrw(bus, B_FMC, FMC_ADDR0, row_of(&G, blk, 0) & 0xffffff);
    wrw(bus, B_FMC, FMC_ANUM, 2);
    wrw(bus, B_FMC, FMC_CMD, 0xd060);
    wrw(bus, B_FMC, FMC_RWCTRL, 0xb);
    int r = wait_bits(bus, B_FMC, FMC_FSTATUS, 0xb, 0xb, 100000) || wait_status(bus, nst);
    wr(bus, B_FMC, FMC_RWCTRL, 0);
    ce_off(bus, ce);
    wrw(bus, B_FMC, FMC_IFCTRL, ifc);
    if (r) {
        recover(bus, ce);
        return -ETIMEDOUT;
    }
    if (!(*nst & 0x80))
        return -EROFS;
    return *nst & 1 ? -EIO : 0;
}

/* ---- the pool: VSVFL contexts, special blocks, the window (5.5 checks 1-3) -------------- */

struct pool {
    int vce, stop, have_bbt;
    unsigned pool, nres, copies, sets, badsum, unread, foreign, erased;
    uint16_t cb[4], nf[2], nscrub, scrub[20], ftlcb[3];
    uint32_t page0usn[4], newest_dec, newest_inc, min_dec;
    unsigned newest_blk, newest_pg, min_blk, min_pg;
    uint16_t map[NSLOT_MAX];        /* the newest copy's, iOS's choice */
    uint8_t taken[NSLOT_MAX];       /* union: 1 if any valid copy says other than 0xFFF0 */
    unsigned nspecial, nsp_unread;
    uint16_t special[32];           /* tagged blocks, then values DEVICEINFOBBT's header lists */
    const char *special_tag[32];
    uint8_t listed[32];             /* 1: only listed in the header (excluded, not checked) */
    uint8_t bbt[512];               /* DEVICEINFOBBT's payload: 1 bit per block, 1 = good */
    unsigned nusable;
    uint16_t usable[128];
};

static void pool_stop(struct pool *P, const char *fmt, unsigned a, unsigned b)
{
    printf("  STOP: ");
    printf(fmt, a, b);
    printf("\n");
    P->stop = 1;
}

static int slot_of(const struct pool *P, uint32_t blk)
{
    unsigned bank = blk & 1, v = blk / 2;
    if (v < P->pool || v >= P->pool + P->nres)
        return -1;
    return bank * P->nres + (v - P->pool);
}

static uint32_t blk_of(const struct pool *P, unsigned k)
{
    return 2 * (P->pool + k % P->nres) + k / P->nres;
}

static const char *slot_name(uint16_t v)
{
    switch (v) {
    case 0xfff0: return "free";
    case 0xffff: return "dead/special";
    case 0xfffe: return "BORROWED";
    case 0xfffd: return "failed-borrow";
    }
    return v < 4096 ? "replacement" : "INVALID";
}

static int bbt_good(const struct pool *P, uint32_t blk)
{
    return P->bbt[blk / 8] >> (blk % 8) & 1;        /* [I] LSB first; `pool` prints the check */
}

/* ctx_fast (k93lv at boot): in each context block, every page up to the first
 * erased one, then only the first page of each later set of 8 (where iOS starts
 * a write); any of those programmed -> the whole block after all.  Pages are
 * programmed in order, so the skipped ones are erased. */
static int ctx_fast;

/* every page of the four context blocks; iOS's newest copy; the union */
static int scan_contexts(struct pool *P, const char *dumpdir)
{
    static uint8_t ctx[MAX_PAGE];
    static int8_t st[4][256];
    static uint32_t dec[4][256];
    uint8_t m[META_BYTES];
    uint32_t row, first = 0;
    int vce = P->vce;

    for (uint32_t blk = 1; blk < 200 && !first; blk++) {
        row = row_of(&G, blk, 0);
        if (read_phys(vce, row, pdata, m) == 1 && m[8] == 0 && m[9] == 0x80) {
            aesf_cbc(DK_META, row, pdata, ctx, 0x800);
            if (vfl_ctx_ok(ctx))
                first = blk;
        }
    }
    if (!first) {
        pool_stop(P, "no VSVFL context page in blocks 1-199", 0, 0);
        return -1;
    }
    for (int i = 0; i < 4; i++)
        P->cb[i] = le16(ctx + 0x68e + 2 * i);
    printf("  context blocks %u %u %u %u (named by block %u's page 0)\n", P->cb[0], P->cb[1], P->cb[2], P->cb[3], first);
    for (int i = 0; i < 4; i++) {
        uint32_t b = P->cb[i];
        if (!b || b >= G.c->blocks) {
            pool_stop(P, "context block #%u is %u", i, b);
            return -1;
        }
        unsigned valid = 0, er = 0, full = !ctx_fast, seen_erased = 0, skipped = 0;
        unsigned c0[4] = { P->copies, P->unread, P->foreign, P->badsum };
        for (uint32_t pg = 0; pg < G.c->ppb; pg++) {
            if (!full && seen_erased && pg % 8) {
                st[i][pg] = 0;          /* not read: erased (checked at the set's first page) */
                er++;
                skipped++;
                continue;
            }
            row = row_of(&G, b, pg);
            int s = read_phys(vce, row, pdata, m);
            if (!full && seen_erased && s != 2) {
                full = 1;               /* programmed after an erased page: read them all */
                pg = (uint32_t)-1;
                valid = er = seen_erased = skipped = 0;
                P->copies = c0[0], P->unread = c0[1], P->foreign = c0[2], P->badsum = c0[3];
                continue;
            }
            if (s == 2)
                seen_erased = 1;
            if (pg == 0)
                P->page0usn[i] = s == 1 && m[9] == 0x80 ? le32(m) : 0;
            if (s == 2) {
                st[i][pg] = 0;
                er++;
                continue;
            }
            if (s != 1) {
                st[i][pg] = 2;
                P->unread++;
                continue;
            }
            if (m[9] != 0x80) {
                st[i][pg] = 3;
                P->foreign++;
                continue;
            }
            aesf_cbc(DK_META, row, pdata, ctx, 0x800);
            if (!vfl_ctx_ok(ctx)) {
                st[i][pg] = 4;
                P->badsum++;
                continue;
            }
            st[i][pg] = 1;
            valid++;
            unsigned pool = le16(ctx + 0x698), nres = G.c->blocks / BPC - pool;
            if (!P->copies) {
                P->pool = pool;
                P->nres = nres;
                P->min_dec = NONE;
            } else if (pool != P->pool) {
                pool_stop(P, "copies disagree on the pool start (%u, %u)", P->pool, pool);
                return -1;
            }
            if (pool != POOL_START || nres * BPC > NSLOT_MAX) {
                pool_stop(P, "pool starts at vblock %u, %u slots per bank: not the model's", pool, nres);
                return -1;
            }
            for (unsigned k = 0; k < nres * BPC; k++) {
                uint16_t v = le16(ctx + 0x26 + 2 * k);
                if (v >= G.c->blocks && v < 0xfff0) {
                    pool_stop(P, "slot %u holds 0x%04x: not a value the VFL writes", k, v);
                    return -1;
                }
                if (v != FREE_SLOT)
                    P->taken[k] = 1;
            }
            dec[i][pg] = le32(ctx + 4);
            if (dec[i][pg] < P->min_dec) {
                P->min_dec = dec[i][pg];
                P->min_blk = b;
                P->min_pg = pg;
            }
            P->copies++;
            if (dumpdir && (pg % 8 == 0 || st[i][pg - 1] != 1)) {
                char name[400];
                snprintf(name, sizeof(name), "%s/vce%d-b%04u-p%03u.ctx", dumpdir, vce, b, pg);
                FILE *f = fopen(name, "wb");
                if (!f || fwrite(ctx, 1, 0x800, f) != 0x800 || fclose(f))
                    perror(name);
            }
        }
        P->erased += er;
        printf("  context block %u: page-0 usn 0x%08x, %u valid copies, %u erased pages%s\n", b, P->page0usn[i],
               valid, er, skipped ? " (the set starts read, the rest taken as erased)" : "");
    }
    /* iOS's choice (VFL_Open; k93iosnand's vfl_load): the block with the smallest
     * non-zero page-0 usn, in it the last set reachable from set 0 */
    int cur = -1;
    uint32_t usn = NONE;
    for (int i = 0; i < 4; i++)
        if (P->page0usn[i] && P->page0usn[i] <= usn) {
            usn = P->page0usn[i];
            cur = i;
        }
    if (cur < 0) {
        pool_stop(P, "no context block has a readable page 0", 0, 0);
        return -1;
    }
    int newest = -1;
    for (uint32_t set = 0; set < G.c->ppb; set += 8) {
        int erased_first = 0;
        for (uint32_t pg = set; pg < set + 8 && pg < G.c->ppb; pg++) {
            if (st[cur][pg] == 0) {
                erased_first = pg == set;
                break;
            }
            if (st[cur][pg] == 1) {
                newest = pg;
                P->sets++;
                break;
            }
        }
        if (erased_first)
            break;
    }
    if (newest < 0) {
        pool_stop(P, "no valid copy in the current context block %u", P->cb[cur], 0);
        return -1;
    }
    P->newest_blk = P->cb[cur];
    P->newest_pg = newest;
    row = row_of(&G, P->newest_blk, newest);
    if (read_phys(vce, row, pdata, m) != 1) {
        pool_stop(P, "the newest copy (block %u page %u) did not read twice", P->newest_blk, newest);
        return -1;
    }
    aesf_cbc(DK_META, row, pdata, ctx, 0x800);
    if (!vfl_ctx_ok(ctx)) {
        pool_stop(P, "the newest copy (block %u page %u) did not check out twice", P->newest_blk, newest);
        return -1;
    }
    P->newest_inc = le32(ctx);
    P->newest_dec = le32(ctx + 4);
    for (unsigned k = 0; k < P->nres * BPC; k++)
        P->map[k] = le16(ctx + 0x26 + 2 * k);
    for (unsigned b = 0; b < BPC && b < 2; b++)
        P->nf[b] = le16(ctx + 0x16 + 2 * b);
    P->nscrub = le16(ctx + 0x6a0);
    if (P->nscrub > 20) {
        pool_stop(P, "scrub list count %u", P->nscrub, 0);
        return -1;
    }
    for (unsigned k = 0; k < P->nscrub; k++)
        P->scrub[k] = le16(ctx + 0x6a2 + 2 * k);
    for (int k = 0; k < 3; k++)
        P->ftlcb[k] = le16(ctx + 0x69a + 2 * k);
    printf("  %u valid copies (%u unreadable, %u bad checksum, %u other pages); iOS's newest: block %u page %u, "
           "usn_dec 0x%08x usn_inc %u (set %u); smallest usn_dec 0x%08x at block %u page %u\n",
           P->copies, P->unread, P->badsum, P->foreign, P->newest_blk, P->newest_pg, P->newest_dec, P->newest_inc,
           P->sets, P->min_dec, P->min_blk, P->min_pg);
    if (P->min_dec != P->newest_dec)
        pool_stop(P, "iOS's newest copy is not the last written (usn_dec 0x%08x vs 0x%08x)", P->newest_dec, P->min_dec);
    if (le32(ctx + 8) != 2)
        pool_stop(P, "FTL type %u, not YAFTL", le32(ctx + 8), 0);
    printf("  pool from vblock %u, %u slots per bank; next_free %u / %u; FTL control blocks %u %u %u; scrub list:",
           P->pool, P->nres, P->nf[0], P->nf[1], P->ftlcb[0], P->ftlcb[1], P->ftlcb[2]);
    for (unsigned k = 0; k < P->nscrub; k++)
        printf(" %u", P->scrub[k]);
    printf("%s\n", P->nscrub ? "" : " empty");
    return P->stop ? -1 : 0;
}

/* page 0 of every block from the top down to (0x60 << log2(blocks)) / 100 (3932) */
static int scan_special(struct pool *P, const char *dumpdir)
{
    static uint8_t dec[1024];
    uint8_t m[META_BYTES];
    unsigned lg = 0, data = 0;
    while (1u << (lg + 1) <= G.c->blocks)
        lg++;
    unsigned low = (0x60u << lg) / 100;
    FILE *sf = NULL;
    if (dumpdir) {
        char name[400];
        snprintf(name, sizeof(name), "%s/vce%d-special.txt", dumpdir, P->vce);
        sf = fopen(name, "w");
    }
    for (unsigned blk = G.c->blocks - 1; blk >= low && blk < G.c->blocks; blk--) {
        uint32_t row = row_of(&G, blk, 0);
        int s = read_phys(P->vce, row, pdata, m);
        if (s == 2)
            continue;
        if (s != 1) {
            P->nsp_unread++;
            printf("  block %u: page 0 unreadable\n", blk);
            continue;
        }
        data++;
        aesf_cbc(DK_META, row, pdata, dec, sizeof(dec));
        for (size_t t = 0; t < sizeof(TAGS) / sizeof(TAGS[0]); t++) {
            int plain = !strncmp((char *)pdata, TAGS[t], 16), whit = !strncmp((char *)dec, TAGS[t], 16);
            if (!plain && !whit)
                continue;
            const uint8_t *p = plain ? pdata : dec;
            if (P->nspecial < 32) {
                P->special[P->nspecial] = blk;
                P->special_tag[P->nspecial++] = TAGS[t];
            }
            printf("  block %u: %s (%s), payload length %u; header:", blk, TAGS[t], plain ? "plain" : "META AES",
                   le32(p + 0x34));
            for (int o = 0x10; o < 0x38; o += 4)
                printf(" %08x", le32(p + o));
            printf("\n");
            if (sf)
                fprintf(sf, "%u %s\n", blk, TAGS[t]);
            if (t == 3) {
                /* the header's other special blocks, read as u16s and u32s */
                for (int o = 0x18; o < 0x2c + 4; o += 2) {
                    uint32_t v16 = le16(p + o), v32 = o % 4 ? 0 : le32(p + o);
                    for (uint32_t v = v16, k = 0; k < 2; k++, v = v32)
                        if (v >= low && v < G.c->blocks && P->nspecial < 32) {
                            int dup = 0;
                            for (unsigned j = 0; j < P->nspecial; j++)
                                dup |= P->special[j] == v;
                            if (!dup) {
                                P->special[P->nspecial] = v;
                                P->listed[P->nspecial] = 1;
                                P->special_tag[P->nspecial++] = "listed in DEVICEINFOBBT";
                                printf("  block %u: listed in DEVICEINFOBBT's header (+0x%02x)\n", v, o);
                            }
                        }
                }
                if (le32(p + 0x34) >= G.c->blocks / 8 && !P->have_bbt) {
                    memcpy(P->bbt, p + 0x38, G.c->blocks / 8);
                    P->have_bbt = 1;
                }
            }
        }
    }
    if (sf)
        fclose(sf);
    printf("  blocks %u-%u: %u with data on page 0, %u special, %u unreadable\n", low, G.c->blocks - 1, data,
           P->nspecial, P->nsp_unread);
    if (P->have_bbt) {
        unsigned nbad = 0;
        printf("  DEVICEINFOBBT bitmap (LSB first), blocks marked bad:");
        for (uint32_t b = 0; b < G.c->blocks; b++)
            if (!bbt_good(P, b)) {
                if (nbad++ < 48)
                    printf(" %u", b);
            }
        printf("%s (%u)\n", nbad > 48 ? " ..." : "", nbad);
        unsigned sp_bad = 0, sp = 0;
        for (unsigned j = 0; j < P->nspecial; j++)
            if (!P->listed[j]) {
                sp++;
                sp_bad += !bbt_good(P, P->special[j]);
            }
        if (!sp)
            printf("  (no tagged special block on this CE to check the bit order with)\n");
        printf("  special blocks marked bad in it: %u of %u%s\n", sp_bad, sp,
               sp_bad == sp ? " (bit order as assumed)" : " -- bit order NOT as assumed: the bitmap is not used");
        if (sp_bad != sp || !sp)
            P->have_bbt = 0;
    } else {
        printf("  no DEVICEINFOBBT bitmap on this CE\n");
    }
    return 0;
}

/* everything but the blocks' own content (5.5 checks 1-3) */
static int pool_scan(int vce, struct pool *P, const char *dumpdir)
{
    memset(P, 0, sizeof(*P));
    P->vce = vce;
    printf("vCE %d = FMI%d CE%d\n", vce, vce_bus[vce], vce_ce[vce]);
    if (scan_contexts(P, dumpdir))
        return -1;
    scan_special(P, dumpdir);
    for (unsigned j = 0; j < P->nspecial; j++) {
        int k = slot_of(P, P->special[j]);
        if (P->listed[j]) {
            printf("  block %u (listed in DEVICEINFOBBT's header): slot %d = 0x%04x, excluded\n", P->special[j], k,
                   k < 0 ? 0 : P->map[k]);
            continue;
        }
        if (k < 0) {
            printf("  special block %u is outside the pool\n", P->special[j]);
            continue;
        }
        if (P->map[k] != 0xffff)
            pool_stop(P, "special block %u sits in slot %u, which is not 0xFFFF", P->special[j], k);
    }
    for (unsigned bank = 0; bank < BPC && bank < 2; bank++) {
        unsigned lo = WIN_LO[bank], hi = WIN_HI[bank], base = bank * P->nres;
        int maxrep = -1, mintop = -1;
        unsigned nborrow = 0, ndead = 0, nfree = 0, below = 0;
        for (unsigned i = 0; i < P->nres; i++) {
            uint16_t v = P->map[base + i];
            if (v < G.c->blocks)
                maxrep = i;
            if (v == 0xfffe || v == 0xfffd)
                nborrow++;
            if (v == 0xffff)
                ndead++;
            if (v == FREE_SLOT)
                nfree++;
            if (i < lo && !P->taken[base + i])
                below++;
            if (i > hi && (v == 0xfffe || v == 0xfffd) && mintop < 0)
                mintop = i;
        }
        printf("  bank %u: next_free %u, replacements up to slot %d, %u borrowed, %u dead/special, %u free; "
               "window slots %u-%u = blocks %u-%u; %u free slots below it\n", bank, P->nf[bank], maxrep, nborrow,
               ndead, nfree, lo, hi, blk_of(P, base + lo), blk_of(P, base + hi), below);
        printf("    slots:");
        for (unsigned i = 0; i < P->nres; i++) {
            uint16_t v = P->map[base + i];
            printf("%s%s", i % 24 ? "" : "\n     ", v == FREE_SLOT ? (P->taken[base + i] ? " f*" : " . ") :
                                                     v == 0xffff ? " X " : v == 0xfffe ? " B " : v == 0xfffd ? " D " : " r ");
        }
        printf("\n    (. free, f* free now but taken in an older copy, r replacement, X dead/special, B borrowed, "
               "D failed borrow)\n");
        if (maxrep >= (int)lo - MARGIN)
            pool_stop(P, "bank %u: replacements reach slot %u, too close to the window", bank, maxrep);
        if (P->nf[bank] >= lo - MARGIN)
            pool_stop(P, "bank %u: next_free %u is too close to the window", bank, P->nf[bank]);
        for (unsigned i = lo; i <= hi + MARGIN && i < P->nres; i++)
            if (P->map[base + i] == 0xfffe || P->map[base + i] == 0xfffd)
                pool_stop(P, "bank %u: borrowed slot %u in or just above the window", bank, i);
        for (unsigned i = lo; i <= hi && i < P->nres; i++) {
            unsigned k = base + i;
            uint32_t b = blk_of(P, k);
            const char *why = NULL;
            int sp = 0;
            for (unsigned j = 0; j < P->nspecial; j++)
                sp |= P->special[j] == b;
            for (int j = 0; j < 4; j++)
                if (P->cb[j] == b || (j < 3 && P->ftlcb[j] == b))
                    why = "a context block number";
            for (unsigned j = 0; j < P->nscrub; j++)
                if (P->scrub[j] == b)
                    why = "on the scrub list";
            if (P->map[k] != FREE_SLOT)
                why = slot_name(P->map[k]);
            else if (P->taken[k])
                why = "taken in an older copy";
            else if (sp)
                why = "a special block";
            else if (b < 20)
                why = "below block 20";
            else if (P->have_bbt && !bbt_good(P, b))
                why = "bad in DEVICEINFOBBT";
            if (why) {
                printf("    skip slot %u (block %u): %s\n", i, b, why);
                continue;
            }
            if (P->nusable < sizeof(P->usable) / sizeof(P->usable[0]))
                P->usable[P->nusable++] = b;
        }
    }
    return P->stop ? -1 : 0;
}

/* 5.5 check 4: what the block holds.  1 all erased, 2 Linux's (page 0 ours; *last =
 * the last programmed page, pages after it erased), 0 anything else, -1 unreadable */
static int block_state(int vce, uint32_t blk, int *last, int verbose)
{
    static uint8_t dec[MAX_PAGE];
    uint8_t m[META_BYTES];
    int ours = 0, erased = 0, other = 0, bad = 0, lastp = -1, gap = 0;
    for (uint32_t pg = 0; pg < G.c->ppb; pg++) {
        uint32_t row = row_of(&G, blk, pg);
        int s = read_phys(vce, row, pdata, m);
        if (s == 2) {
            erased++;
            continue;
        }
        if (erased)
            gap = 1;            /* a programmed page after an erased one */
        lastp = pg;
        if (s < 0) {
            bad++;
            continue;
        }
        if (pg == 0) {
            aesf_cbc(DK_META, row, pdata, dec, 64);
            ours = is_ours(dec, m, vce, blk);
        }
        if (m[9] != T_LINUX)
            other++;
    }
    if (last)
        *last = lastp;
    if (verbose)
        printf("    block %u: %d erased pages, last programmed page %d, page 0 %s, %d unreadable, %d not Linux's%s\n",
               blk, erased, lastp, ours ? "Linux's" : lastp >= 0 ? "NOT Linux's" : "erased", bad, other,
               gap ? ", programmed page after an erased one" : "");
    if (erased == (int)G.c->ppb)
        return 1;
    if (ours && !other && !gap)
        return 2;
    return bad ? -1 : 0;
}

/* the guard before any write: the pool check of this process, now, and the block's
 * content.  want: 0 = all erased (dry run), 1 = erased or Linux's (erase), 2 =
 * erased from page `first` on and Linux's or erased before it (program) */
static int guard(int vce, uint32_t blk, int want, int first)
{
    static struct pool P;
    int t = trace, last = -1;
    trace = 0;
    printf("guard: vCE %d block %u\n", vce, blk);
    int r = pool_scan(vce, &P, NULL);
    trace = t;
    if (r || P.stop) {
        printf("guard: the pool check failed: nothing is written\n");
        return -1;
    }
    int in = 0;
    for (unsigned i = 0; i < P.nusable; i++)
        in |= P.usable[i] == blk;
    if (!in) {
        printf("guard: block %u is not among the %u usable blocks of vCE %d: nothing is written\n", blk, P.nusable,
               vce);
        return -1;
    }
    trace = 0;
    int s = block_state(vce, blk, &last, 1);
    trace = t;
    if (want == 0 && s != 1) {
        printf("guard: block %u is not all erased: no dry run on it\n", blk);
        return -1;
    }
    if (want == 1 && s != 1 && s != 2) {
        printf("guard: block %u is neither erased nor Linux's: nothing is written\n", blk);
        return -1;
    }
    if (want == 2) {
        if (s == 1 && first != 0) {
            printf("guard: block %u is erased: programming starts at page 0, not %d\n", blk, first);
            return -1;
        }
        if (s == 2 && last != first - 1) {
            printf("guard: block %u: Linux's pages end at %d; the next one to program is %d, not %d\n", blk, last,
                   last + 1, first);
            return -1;
        }
        if (s != 1 && s != 2) {
            printf("guard: block %u is neither erased nor Linux's: nothing is written\n", blk);
            return -1;
        }
    }
    printf("guard: ok -- block %u is a free slot of the window in every context copy, not special, %s\n", blk,
           s == 1 ? "erased" : "Linux's");
    allow_block(vce, blk);
    return 0;
}

/* ---- commands ---------------------------------------------------------------------------- */

static int setup(int all_buses, int vce)
{
    if (load_state() || vsvfl_setup())
        return -1;
    if (BPC != 2 || G.c->blocks != 4096 || G.c->ppb != 256 || VENDOR != 0x150011) {
        fprintf(stderr, "k93nandw: not the geometry this tool knows (32 GB K93: 4096 blocks, 256 pages, "
                "vendor 0x150011, 2 banks)\n");
        return -1;
    }
    if (vce >= (int)NCE) {
        fprintf(stderr, "k93nandw: there are %u virtual CEs\n", NCE);
        return -1;
    }
    for (int b = 0; b < NBUS; b++)
        if ((cemap >> (b * 8)) & 0xff && (all_buses || b == vce_bus[vce]) && !claim(b))
            return -1;
    return 0;
}

static void hold_signals(void)
{
    sigset_t s;
    sigemptyset(&s);
    sigaddset(&s, SIGINT);
    sigaddset(&s, SIGTERM);
    sigaddset(&s, SIGHUP);
    sigaddset(&s, SIGQUIT);
    sigaddset(&s, SIGPIPE);
    sigprocmask(SIG_BLOCK, &s, NULL);
}

static int cmd_wp(void)
{
    int rc = 0;
    if (setup(1, 0))
        return 1;
    for (unsigned v = 0; v < NCE; v++) {
        int bus = vce_bus[v], ce = vce_ce[v];
        uint8_t s1 = 0, s2 = 0, s3 = 0;
        printf("vCE %u = FMI%d CE%d\n", v, bus, ce);
        fmi_reset(bus);
        uint32_t ifc = rd(bus, B_FMC, FMC_IFCTRL);
        int r = status_read(bus, ce, &s1);
        printf("  IFCTRL bit 21 set (0x%08x), status only:\n", ifc | 0x200000);
        if (trace)
            printf("    W FMC%d+0x08 IFCTRL  <- 0x%08x   (T0: WP# test, no command)\n", bus, ifc | 0x200000);
        mmio_wr(bus, B_FMC, FMC_IFCTRL, ifc | 0x200000);
        r |= status_read(bus, ce, &s2);
        wr(bus, B_FMC, FMC_IFCTRL, ifc);
        r |= status_read(bus, ce, &s3);
        fmi_reset(bus);
        printf("  NAND status: bit 21 clear 0x%02x, set 0x%02x, clear again 0x%02x%s\n", s1, s2, s3,
               r ? " (a status read timed out)" : "");
        if (r)
            rc = 1;
        else if (!(s1 & 0x80) && (s2 & 0x80) && !(s3 & 0x80))
            printf("  -> status bit 7 follows IFCTRL bit 21: bit 21 drives WP#, the reads leave the chip protected\n");
        else if ((s1 & 0x80) && (s2 & 0x80))
            printf("  -> bit 7 is set either way: the chip is never write-protected; bit 21 is not WP# (or WP# is "
                   "not wired)\n"), rc = 1;
        else
            printf("  -> unexpected: STOP and look\n"), rc = 1;
    }
    return rc;
}

static int cmd_pool(int argc, char **argv)
{
    static struct pool P;
    const char *dumpdir = NULL;
    int content = 0, want[16] = { 0 }, nwant = 0, rc = 0;
    for (int i = 0; i < argc; i++)
        if (!strcmp(argv[i], "-e"))
            content = 1;
        else if (!strcmp(argv[i], "-d") && i + 1 < argc)
            dumpdir = argv[++i];
        else if (argv[i][0] >= '0' && argv[i][0] <= '9' && nwant < 16)
            want[nwant++] = atoi(argv[i]);
        else
            die("pool: [-e] [-d DIR] [VCE...]");
    if (setup(1, 0))
        return 1;
    if (dumpdir)
        mkdir(dumpdir, 0755);
    trace = 0;
    uint64_t t0 = now_us();
    for (unsigned v = 0; v < NCE; v++) {
        int sel = !nwant;
        for (int i = 0; i < nwant; i++)
            sel |= want[i] == (int)v;
        if (!sel)
            continue;
        int r = pool_scan(v, &P, dumpdir);
        if (r || P.stop) {
            printf("  vCE %u: STOP -- the device does not match the model; nothing may be written here\n\n", v);
            rc = 1;
            continue;
        }
        if (content) {
            unsigned n = 0;
            printf("  content of the %u usable blocks (5.5 check 4):\n", P.nusable);
            for (unsigned i = 0; i < P.nusable; i++) {
                int last, s = block_state(v, P.usable[i], &last, 0);
                if (s == 1 || s == 2)
                    P.usable[n++] = P.usable[i];
                if (s != 1)
                    printf("    block %u: %s\n", P.usable[i], s == 2 ? "Linux's" : s < 0 ? "UNREADABLE pages: skip" :
                                                                       "holds data (not erased, not Linux's): skip");
            }
            P.nusable = n;
        }
        printf("  USABLE vCE %u (%u blocks%s):", v, P.nusable, content ? ", erased or Linux's" : ", content not read");
        for (unsigned i = 0; i < P.nusable; i++)
            printf(" %u", P.usable[i]);
        printf("\n\n");
    }
    fprintf(stderr, "k93nandw: pool in %.1f s\n", (now_us() - t0) / 1e6);
    return rc;
}

static int parse_vce_blk(char **argv, int *vce, uint32_t *blk)
{
    *vce = atoi(argv[0]);
    *blk = strtoul(argv[1], NULL, 0);
    if (*blk >= 4096)
        die("block out of range");
    return 0;
}

static int cmd_dryrun(int argc, char **argv)
{
    static uint8_t plain[MAX_PAGE] __attribute__((aligned(8))), enc[MAX_PAGE] __attribute__((aligned(8)));
    uint8_t meta[12], mw[META_BYTES], nst = 0;
    int vce, mode = META_PRE, room = 1;
    uint32_t blk;
    if (argc < 2)
        die("dryrun VCE BLOCK [-m pre|mid|post] [-r dbg|none]");
    parse_vce_blk(argv, &vce, &blk);
    for (int i = 2; i < argc; i++)
        if (!strcmp(argv[i], "-m") && i + 1 < argc) {
            i++;
            mode = !strcmp(argv[i], "pre") ? META_PRE : !strcmp(argv[i], "mid") ? META_MID :
                   !strcmp(argv[i], "post") ? META_POST : -1;
            if (mode < 0)
                die("-m pre|mid|post");
        } else if (!strcmp(argv[i], "-r") && i + 1 < argc) {
            i++;
            room = strcmp(argv[i], "none") != 0;
        } else {
            die("dryrun VCE BLOCK [-m pre|mid|post] [-r dbg|none]");
        }
    if (setup(0, vce) || guard(vce, blk, 0, 0))
        return 1;
    int bus = vce_bus[vce], ce = vce_ce[vce];
    uint32_t row = row_of(&G, blk, 0);
    pattern(0x93939393u, vce, blk, 0, plain, meta);
    aese_cbc_meta(row, plain, enc, G.c->page);
    meta_unwhiten(meta, mw, row);
    hold_signals();
    trace = 1;
    arm(OP_DRY, vce, blk);
    int r = program_page(bus, ce, row, enc, mw, &nst, mode, room, 1);
    disarm();
    printf("\ndry run %s; NAND status afterwards 0x%02x (bit 7 %s)\n", r ? "FAILED" : "completed", nst,
           nst & 0x80 ? "set: NOT protected" : "clear: protected");
    trace = 0;
    int last, s = block_state(vce, blk, &last, 1);
    printf("block %u afterwards: %s\n", blk, s == 1 ? "still all erased (as it must be)" : "NOT ERASED -- look");
    return r || s != 1;
}

static int cmd_erase(int argc, char **argv)
{
    int vce;
    uint32_t blk;
    uint8_t nst = 0;
    if (argc < 2)
        die("erase VCE BLOCK");
    parse_vce_blk(argv, &vce, &blk);
    if (setup(0, vce) || guard(vce, blk, 1, 0))
        return 1;
    hold_signals();
    trace = 1;
    arm(OP_ERASE, vce, blk);
    uint64_t t0 = now_us();
    int r = erase_block(vce_bus[vce], vce_ce[vce], blk, &nst);
    uint64_t t1 = now_us();
    disarm();
    trace = 0;
    printf("\nerase of vCE %d block %u: %s, NAND status 0x%02x, %.2f ms\n", vce, blk,
           !r ? "ok" : r == -EROFS ? "WRITE-PROTECTED (status bit 7 clear)" : r == -EIO ? "FAILED (status bit 0)" :
                                                                           "bus timeout",
           nst, (t1 - t0) / 1e3);
    int last, s = block_state(vce, blk, &last, 1);
    printf("block %u afterwards: %s\n", blk, s == 1 ? "all erased" : "NOT all erased");
    return r || s != 1;
}

static int check_page(int vce, uint32_t blk, uint32_t pg, uint32_t seed, int *bits, int *fix, int quiet)
{
    static uint8_t want[MAX_PAGE] __attribute__((aligned(8))), dec[MAX_PAGE];
    uint8_t meta[12], m[META_BYTES], raw[META_BYTES], nst;
    struct eccres e;
    uint32_t row = row_of(&G, blk, pg);
    *bits = 0;
    *fix = 0;
    if (read_page(vce_bus[vce], vce_ce[vce], row, pdata, raw, 0, G.sectors, &e, &nst))
        return -1;
    if (e.status == 2)
        return 2;
    if (e.status < 0) {
        if (!quiet)
            printf("    page %u: UNCORRECTABLE\n", pg);
        return -2;
    }
    *fix = e.maxfix;
    meta_unwhiten(raw, m, row);
    aesf_cbc(DK_META, row, pdata, dec, G.c->page);
    pattern(seed, vce, blk, pg, want, meta);
    for (unsigned i = 0; i < G.c->page; i++)
        *bits += __builtin_popcount(dec[i] ^ want[i]);
    int mbits = 0;
    for (int i = 0; i < META_BYTES; i++)
        mbits += __builtin_popcount(m[i] ^ meta[i]);
    if ((*bits || mbits) && !quiet)
        printf("    page %u: %d data bits and %d metadata bits differ from the pattern (ECC corrected %d)\n", pg,
               *bits, mbits, e.maxfix);
    return *bits || mbits ? 0 : 1;
}

static int cmd_program(int argc, char **argv)
{
    static uint8_t plain[MAX_PAGE] __attribute__((aligned(8))), enc[MAX_PAGE] __attribute__((aligned(8)));
    uint8_t meta[12], mw[META_BYTES], nst = 0;
    int vce, rc = 0;
    uint32_t blk;
    if (argc < 4)
        die("program VCE BLOCK FIRST N [SEED]");
    parse_vce_blk(argv, &vce, &blk);
    uint32_t first = strtoul(argv[2], NULL, 0), n = strtoul(argv[3], NULL, 0);
    uint32_t seed = argc > 4 ? strtoul(argv[4], NULL, 0) : 0x93939393u;
    if (setup(0, vce))
        return 1;
    if (!n || first >= G.c->ppb || first + n > G.c->ppb)
        die("pages out of range");
    if (guard(vce, blk, 2, first))
        return 1;
    int bus = vce_bus[vce], ce = vce_ce[vce];
    hold_signals();
    uint64_t tp = 0, t0 = now_us();
    int maxfix = 0;
    for (uint32_t pg = first; pg < first + n; pg++) {
        uint32_t row = row_of(&G, blk, pg);
        pattern(seed, vce, blk, pg, plain, meta);
        aese_cbc_meta(row, plain, enc, G.c->page);
        meta_unwhiten(meta, mw, row);
        trace = pg == first;                    /* the first page with every register */
        arm(OP_PROGRAM, vce, blk);
        uint64_t a = now_us();
        int r = program_page(bus, ce, row, enc, mw, &nst, META_PRE, 1, 0);
        tp += now_us() - a;
        disarm();
        trace = 0;
        if (r) {
            printf("page %u: %s, NAND status 0x%02x -- stopping\n", pg,
                   r == -EROFS ? "WRITE-PROTECTED" : r == -EIO ? "PROGRAM FAILED (status bit 0)" : "bus timeout", nst);
            rc = 1;
            break;
        }
        int bits, fix, c = check_page(vce, blk, pg, seed, &bits, &fix, 0);
        if (fix > maxfix)
            maxfix = fix;
        if (pg == first || c != 1)
            printf("page %u: programmed (status 0x%02x), read back: %s, ECC corrected %d bits\n", pg, nst,
                   c == 1 ? "matches" : c == 2 ? "ERASED" : c == 0 ? "DIFFERS" : "unreadable", fix);
        if (c != 1) {
            rc = 1;
            break;
        }
    }
    printf("\n%u page(s) from %u: %s; %.1f ms per page programming (PIO + tPROG), %.1f s in all; most ECC "
           "corrections in a page %d\n", n, first, rc ? "STOPPED" : "programmed and verified",
           tp / 1e3 / (n ? n : 1), (now_us() - t0) / 1e6, maxfix);
    return rc;
}

static int cmd_check(int argc, char **argv)
{
    int vce, er = 0, ok = 0, bad = 0, unr = 0, maxfix = 0, fixh[32] = { 0 };
    uint32_t blk;
    if (argc < 2)
        die("check VCE BLOCK [SEED]");
    parse_vce_blk(argv, &vce, &blk);
    uint32_t seed = argc > 2 ? strtoul(argv[2], NULL, 0) : 0x93939393u;
    if (setup(0, vce))
        return 1;
    trace = 0;
    for (uint32_t pg = 0; pg < G.c->ppb; pg++) {
        int bits, fix, c = check_page(vce, blk, pg, seed, &bits, &fix, 0);
        if (c == 2)
            er++;
        else if (c == 1)
            ok++;
        else if (c == 0)
            bad++;
        else
            unr++;
        if (c == 1 || c == 0) {
            fixh[fix & 31]++;
            if (fix > maxfix)
                maxfix = fix;
        }
    }
    printf("vCE %d block %u: %d pages match the pattern (seed 0x%08x), %d differ, %d erased, %d unreadable; "
           "most ECC corrections in a page %d; pages by bits corrected:", vce, blk, ok, seed, bad, er, unr, maxfix);
    for (int i = 0; i <= maxfix; i++)
        if (fixh[i])
            printf(" %d:%d", i, fixh[i]);
    printf("\n");
    return bad || unr;
}

static int cmd_selftest_w(void)
{
    static const uint8_t K[16] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 };
    static const uint8_t C[16] = { 0x69, 0xc4, 0xe0, 0xd8, 0x6a, 0x7b, 0x04, 0x30,
                                   0xd8, 0xcd, 0xb7, 0x80, 0x70, 0xb4, 0xc5, 0x5a };
    static uint8_t a[8192] __attribute__((aligned(8))), b[8192], c[8192], d[8192];
    uint8_t rk[176], s[16], meta[12], mw[10], mb[10];
    int bad = cmd_selftest();
    struct chip fake = { { 0x45, 0x2d, 0x94, 0x82 }, 4096, 256, 8192, 640, 0x10, 9, 1 };
    G.c = &fake;
    aes_expand(K, rk);
    for (int i = 0; i < 16; i++)
        s[i] = i * 0x11;
    aes_encrypt_block(rk, s);
    printf("AES-128 FIPS-197 C.1 encrypt:");
    for (int i = 0; i < 16; i++)
        printf(" %02x", s[i]);
    printf("  (expect 69 c4 e0 d8 .. c5 5a)\n");
    bad |= memcmp(s, C, 16) != 0;
    pattern(0x1234, 2, 4000, 7, a, meta);
    aes_cbc_encrypt(RK_META, 0xfa007, a, b, sizeof(a));
    aes_cbc_decrypt(KEY_META, 0xfa007, b, c, sizeof(a));
    aesf_cbc(DK_META, 0xfa007, b, d, sizeof(a));
    printf("CBC encrypt -> decrypt (byte-wise, tables): %s, %s\n", memcmp(a, c, sizeof(a)) ? "DIFFER" : "same",
           memcmp(a, d, sizeof(a)) ? "DIFFER" : "same");
    bad |= memcmp(a, c, sizeof(a)) || memcmp(a, d, sizeof(a));
    aese_cbc_meta(0xfa007, a, c, sizeof(a));
    printf("CBC encrypt, tables vs byte-wise: %s\n", memcmp(b, c, sizeof(a)) ? "DIFFER" : "same");
    bad |= memcmp(b, c, sizeof(a)) != 0;
    {
        uint32_t ek[44];
        for (int i = 0; i < 44; i++)
            ek[i] = GETU32(rk + 4 * i);
        for (int i = 0; i < 16; i++)
            s[i] = i * 0x11;
        aese_block(ek, s, s);
        printf("AES-128 FIPS-197 C.1 encrypt, tables: %s\n", memcmp(s, C, 16) ? "WRONG" : "ok");
        bad |= memcmp(s, C, 16) != 0;
    }
    uint64_t t0 = now_us();
    for (int i = 0; i < 64; i++)
        aes_cbc_encrypt(RK_META, i, a, b, sizeof(a));
    uint64_t t1 = now_us();
    for (int i = 0; i < 64; i++)
        aese_cbc_meta(i, a, b, sizeof(a));
    uint64_t t2 = now_us();
    printf("8 KB page encrypted: byte-wise %.2f ms, tables %.2f ms\n", (t1 - t0) / 64e3, (t2 - t1) / 64e3);
    meta_unwhiten(meta, mw, 0xfa007);
    meta_unwhiten(mw, mb, 0xfa007);
    printf("metadata whitening twice: %s (whitened: %02x %02x .. %02x %02x)\n", memcmp(meta, mb, 10) ? "DIFFERS" : "same",
           mw[0], mw[1], mw[8], mw[9]);
    bad |= memcmp(meta, mb, 10) != 0;
    pattern(0x1234, 2, 4000, 0, a, meta);
    printf("pattern page 0 is ours: %s; type 0x%02x; no tag matches it: ", is_ours(a, meta, 2, 4000) ? "yes" : "NO",
           meta[9]);
    int tag = 0;
    for (size_t t = 0; t < sizeof(TAGS) / sizeof(TAGS[0]); t++)
        tag |= !strncmp((char *)a, TAGS[t], 16);
    printf("%s\n", tag ? "NO" : "yes");
    bad |= !is_ours(a, meta, 2, 4000) || tag || is_ours(a, meta, 2, 4002) || is_ours(a, meta, 1, 4000);
    pattern(0x1234, 2, 4000, 0, b, meta);
    bad |= memcmp(a, b, sizeof(a)) != 0;
    printf("%s\n", bad ? "SELFTEST FAILED" : "write selftest ok");
    return bad;
}

static void usage_w(void)
{
    fprintf(stderr,
            "usage: k93nandw selftest | wp\n"
            "       k93nandw pool [-e] [-d DIR] [VCE...]\n"
            "       k93nandw dryrun VCE BLOCK [-m pre|mid|post] [-r dbg|none]\n"
            "       k93nandw erase VCE BLOCK\n"
            "       k93nandw program VCE BLOCK FIRST N [SEED]\n"
            "       k93nandw check VCE BLOCK [SEED]\n");
}

#if !defined(K93NANDW_TEST) && !defined(K93NANDW_LIB)    /* k93lv.c, a host-side model */
int main(int argc, char **argv)
{
    if (argc < 2) {
        usage_w();
        return 1;
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    whiten_init();
    aes_init();
    aesf_init();
    aes_expand(KEY_META, RK_META);
    aese_init();
    if (aesf_check())
        return 1;
    trace = 0;
    const char *c = argv[1];
    argc -= 2;
    argv += 2;
    if (!strcmp(c, "selftest"))
        return cmd_selftest_w();
    open_hw();
    if (!strcmp(c, "wp")) {
        trace = 1;
        return cmd_wp();
    }
    if (!strcmp(c, "pool"))
        return cmd_pool(argc, argv);
    if (!strcmp(c, "dryrun"))
        return cmd_dryrun(argc, argv);
    if (!strcmp(c, "erase"))
        return cmd_erase(argc, argv);
    if (!strcmp(c, "program"))
        return cmd_program(argc, argv);
    if (!strcmp(c, "check"))
        return cmd_check(argc, argv);
    usage_w();
    return 1;
}
#endif
