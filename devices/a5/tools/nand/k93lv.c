/* k93lv -- the iPad 2's Linux volume: a log-structured block device on the
 * blocks of iOS's VSVFL reserved pool that Linux may own
 * (docs/research/k93-nand-write.md 5.5, proven by its tests T0-T5), served
 * over NBD by the same process that serves iOS's System partition.
 *
 *   k93lv [-j2] serve [PARTITION...]  /dev/nbd0.. = the iOS partitions (default
 *                                     System), read-only, as k93iosnand serve;
 *                                     the next /dev/nbdN = the Linux volume,
 *                                     read-write.  What initramfs/init runs.
 *   k93lv info                        the pool check and the volume, read only
 *   k93lv format                      erase every block of the volume: the next
 *                                     serve starts an empty one
 *   k93lv selftest                    (host build, -DK93LV_SIM) the FTL against
 *                                     an in-memory flash: random writes, trims,
 *                                     remounts and power cuts
 *
 * Where: the window of every virtual CE (bank 0 slots 48-87, bank 1 slots 48-63;
 * 56 blocks of 2 MB per CE, 224 on the 32 GB iPad 2), as the pool check of
 * k93nandw.c finds it at EVERY start: a block is used only while it is a free
 * slot in every copy of its CE's VSVFL context, not a special block, not bad
 * in DEVICEINFOBBT, and erased or Linux's.  k93nandw.c's arm() takes no other
 * block, and its wrw() checks every program and erase against the armed block
 * on the FMC's own registers.
 *
 * The format, in each block (page = 8 KB, two 4 KB sectors):
 *   page 0       header: LMAGIC, 1, vCE, block, 0 (k93nandw's is_ours()),
 *                'LVOL', version, volume uuid, sequence number, erase count,
 *                volume size, CRC32
 *   pages 1-254  data: sectors 2k and 2k+1 of the page, their LBAs in the
 *                page's metadata (lba0, lba1, kind 1, T_LINUX)
 *   page 255     summary: the 508 LBAs of the block, written when it is full
 * every page AES'd with the META key, IV from the row, metadata whitened -- as
 * iOS writes its own non-user pages, so k93nand read -k meta shows them.
 *
 * The map is rebuilt at start: headers (one page per block), then the blocks
 * oldest first, each by its summary, or page by page when it has none (the one
 * open when Linux stopped).  A write is programmed before it is acknowledged;
 * a block is always erased right before it is opened.  Garbage collection
 * takes the full block with the fewest valid sectors when the free ones run
 * low; a quarter of the space is kept for it.  Trims are kept in RAM only (a
 * trimmed sector may come back with old contents after a restart, which a
 * file system does not read).
 *
 * Build (from devices/a5, in the cascadia-build image):
 *   arm-linux-gnueabihf-gcc -static -O2 -Wall -Wno-unused-function -pthread \
 *       -D_FILE_OFFSET_BITS=64 -o build/k93/tools/k93lv tools/nand/k93lv.c
 * Host test:
 *   gcc -O2 -Wall -Wno-unused-function -pthread -D_FILE_OFFSET_BITS=64 \
 *       -DK93NAND_SIM -DK93LV_SIM -o k93lv-sim tools/nand/k93lv.c && ./k93lv-sim selftest
 */
#define K93NANDW_LIB
#include "k93nandw.c"
#include <stddef.h>

#define LV_SEC          4096u
#define LV_PPB          256u                    /* pages per block */
#define LV_DPAGES       254u                    /* data pages 1..254 */
#define LV_SPB          (LV_DPAGES * 2)         /* 508 sectors per block */
#define LV_MAXPEB       256
#define LV_GC_LOW       3                       /* free blocks GC keeps */
#define LV_MAGIC        0x4c4f564cu             /* 'LVOL' */
#define LV_SUMMAGIC     0x4d55534cu             /* 'LSUM' */
#define LV_VERSION      1u
#define K_DATA          0x01
#define K_HEAD          0x02
#define K_SUM           0x03

#define LOC(pi, pg, h)  ((uint32_t)(pi) << 9 | (uint32_t)(pg) << 1 | (h))
#define LOC_PEB(l)      ((l) >> 9)
#define LOC_PG(l)       ((l) >> 1 & 0xff)
#define LOC_H(l)        ((l) & 1)
#define SLOT(pg, h)     (((pg) - 1) * 2 + (h))

enum { PB_FREE, PB_USED, PB_OPEN, PB_BAD };
static const char *const PB_NAME[] __attribute__((unused)) = { "free", "used", "open", "BAD" };

struct peb {
    uint8_t vce, state, nogc, fresh;    /* fresh: erased by this process since */
    uint16_t blk, valid;
    uint32_t ec;                /* erase count; NONE = not known */
    uint64_t seq;               /* this volume's header, 0 = none */
};

struct lvhdr {                  /* page 0, plaintext */
    uint8_t magic[16];
    uint32_t one, vce, blk, page0;
    uint32_t lvmagic, version;
    uint8_t uuid[16];
    uint64_t seq;
    uint32_t ec, nsect, npeb, crc;      /* crc: CRC32 of the 76 bytes before it */
};

struct lvsum {                  /* page 255, plaintext */
    uint32_t magic, version;
    uint8_t uuid[16];
    uint64_t seq;
    uint32_t lba[LV_SPB];
    uint32_t crc;               /* of everything before it */
};

static struct peb peb[LV_MAXPEB];
static uint32_t p2l[LV_MAXPEB][LV_SPB];        /* what each sector slot holds (NONE: nothing) */
static uint32_t l2p[LV_MAXPEB * LV_SPB];        /* sector -> LOC, NONE: never written / trimmed */
static unsigned npeb, nsect;
static uint8_t uuid[16];
static uint64_t maxseq;
static int head = -1, in_gc, lv_ro;
static unsigned head_pg;
static struct {
    unsigned long long rd, wr, pages, erases, gcs, gcmoved, progfail, erasefail, readerr, trims;
} lst;

static uint32_t crc32(const void *p, size_t n)
{
    static uint32_t T[256];
    const uint8_t *b = p;
    if (!T[1])
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++)
                c = c & 1 ? 0xedb88320u ^ c >> 1 : c >> 1;
            T[i] = c;
        }
    uint32_t c = 0xffffffffu;
    while (n--)
        c = T[(c ^ *b++) & 0xff] ^ c >> 8;
    return c ^ 0xffffffffu;
}

static void mkmeta(uint8_t meta[12], uint32_t l0, uint32_t l1, uint8_t kind)
{
    memset(meta, 0xff, 12);
    memcpy(meta, &l0, 4);
    memcpy(meta + 4, &l1, 4);
    meta[8] = kind;
    meta[9] = T_LINUX;
}

/* ---- the flash under the FTL: one page or one block at a time -------------------
 * fl_read: 1 ok (plain = decrypted data, m = unwhitened metadata), 2 erased,
 * < 0 unreadable.  fl_program / fl_erase: 0 or < 0. */
#ifndef K93LV_SIM
static int fl_read(unsigned pi, unsigned pg, uint8_t *plain, uint8_t m[META_BYTES])
{
    static uint8_t raw[MAX_PAGE] __attribute__((aligned(8)));
    uint32_t row = row_of(&G, peb[pi].blk, pg);
    int s = read_phys(peb[pi].vce, row, raw, m);
    if (s == 1 && plain)
        aesf_cbc(DK_META, row, raw, plain, G.c->page);
    return s;
}

static int fl_program(unsigned pi, unsigned pg, const uint8_t *plain, const uint8_t meta[12])
{
    static uint8_t enc[MAX_PAGE] __attribute__((aligned(8)));
    uint8_t mw[META_BYTES], nst = 0;
    const struct peb *b = &peb[pi];
    uint32_t row = row_of(&G, b->blk, pg);
    aese_cbc_meta(row, plain, enc, G.c->page);
    meta_unwhiten(meta, mw, row);
    arm(OP_PROGRAM, b->vce, b->blk);
    int r = program_page(vce_bus[b->vce], vce_ce[b->vce], row, enc, mw, &nst, META_PRE, 1, 0);
    disarm();
    if (r)
        fprintf(stderr, "k93lv: program vCE %u block %u page %u failed (%d, status 0x%02x)\n", b->vce, b->blk, pg, r,
                nst);
    return r;
}

static int fl_erase(unsigned pi)
{
    uint8_t nst = 0;
    const struct peb *b = &peb[pi];
    arm(OP_ERASE, b->vce, b->blk);
    int r = erase_block(vce_bus[b->vce], vce_ce[b->vce], b->blk, &nst);
    disarm();
    if (r)
        fprintf(stderr, "k93lv: erase vCE %u block %u failed (%d, status 0x%02x)\n", b->vce, b->blk, r, nst);
    return r;
}
#else
/* an in-memory flash that enforces what the NAND does: program only erased
 * pages, in order within a block; a power cut leaves the page being written torn */
static uint8_t *sim_data;               /* npeb * 256 pages * (8192 data + 12 meta) */
static uint8_t sim_st[LV_MAXPEB][LV_PPB];       /* 0 erased, 1 programmed, 2 torn */
static long sim_ops_left = -1;          /* power cut after this many programs/erases */
static int sim_dead;
static unsigned sim_pagesz = 8192;

static uint8_t *sim_page(unsigned pi, unsigned pg)
{
    return sim_data + ((size_t)pi * LV_PPB + pg) * (sim_pagesz + 12);
}

static int fl_read(unsigned pi, unsigned pg, uint8_t *plain, uint8_t m[META_BYTES])
{
    if (sim_dead)
        return -1;
    if (sim_st[pi][pg] == 0) {
        memset(m, 0xff, META_BYTES);
        return 2;
    }
    if (sim_st[pi][pg] == 2)
        return -0x24;
    if (plain)
        memcpy(plain, sim_page(pi, pg), sim_pagesz);
    memcpy(m, sim_page(pi, pg) + sim_pagesz, META_BYTES);
    return 1;
}

/* 0: the operation happens; 1: the power goes during it; -1: it is gone already */
static int sim_tick(void)
{
    if (sim_dead)
        return -1;
    if (sim_ops_left == 0) {
        sim_dead = 1;
        return 1;
    }
    if (sim_ops_left > 0)
        sim_ops_left--;
    return 0;
}

static int fl_program(unsigned pi, unsigned pg, const uint8_t *plain, const uint8_t meta[12])
{
    if (sim_st[pi][pg] != 0) {
        fprintf(stderr, "SIM: FTL BUG: program of block %u page %u, not erased\n", pi, pg);
        abort();
    }
    for (unsigned k = pg + 1; k < LV_PPB; k++)
        if (sim_st[pi][k]) {
            fprintf(stderr, "SIM: FTL BUG: block %u page %u programmed after page %u\n", pi, pg, k);
            abort();
        }
    int t = sim_tick();
    if (t) {
        if (t > 0)
            sim_st[pi][pg] = 2;         /* the cut came during this program */
        return -1;
    }
    memcpy(sim_page(pi, pg), plain, sim_pagesz);
    memcpy(sim_page(pi, pg) + sim_pagesz, meta, 12);
    sim_st[pi][pg] = 1;
    return 0;
}

static int fl_erase(unsigned pi)
{
    int t = sim_tick();
    if (t) {
        for (unsigned k = 0; t > 0 && k < LV_PPB; k++)     /* an interrupted erase: anything */
            if (sim_st[pi][k] == 1 && k % 3 == 0)
                sim_st[pi][k] = 2;
        return -1;
    }
    memset(sim_st[pi], 0, LV_PPB);
    return 0;
}
#endif

/* ---- the map ------------------------------------------------------------------------ */

static unsigned nfree(void)
{
    unsigned n = 0;
    for (unsigned i = 0; i < npeb; i++)
        n += peb[i].state == PB_FREE;
    return n;
}

static uint32_t avg_ec(void)
{
    uint64_t sum = 0;
    unsigned n = 0;
    for (unsigned i = 0; i < npeb; i++)
        if (peb[i].ec != NONE) {
            sum += peb[i].ec;
            n++;
        }
    return n ? (uint32_t)(sum / n) : 0;
}

/* a sector now lives at loc; its old place stops counting */
static void place(uint32_t lba, uint32_t loc)
{
    p2l[LOC_PEB(loc)][SLOT(LOC_PG(loc), LOC_H(loc))] = lba;
    if (lba == NONE || lba >= nsect)
        return;
    uint32_t old = l2p[lba];
    if (old != NONE)
        peb[LOC_PEB(old)].valid--;
    l2p[lba] = loc;
    peb[LOC_PEB(loc)].valid++;
}

/* ---- page cache for reads: one page, keyed by block, page and sequence ------------------ */

static struct {
    int pi, pg;
    uint64_t seq;
    uint8_t m[META_BYTES];
    uint8_t data[MAX_PAGE] __attribute__((aligned(8)));
} rc = { -1, -1, 0, { 0 }, { 0 } };

static const uint8_t *read_slot(uint32_t loc, uint32_t lba)
{
    unsigned pi = LOC_PEB(loc), pg = LOC_PG(loc), h = LOC_H(loc);
    if (rc.pi != (int)pi || rc.pg != (int)pg || rc.seq != peb[pi].seq) {
        rc.pi = -1;
        int s = fl_read(pi, pg, rc.data, rc.m);
        if (s != 1 || rc.m[8] != K_DATA || rc.m[9] != T_LINUX) {
            if (lst.readerr++ < 20)
                fprintf(stderr, "k93lv: sector %u at block %u (vCE %u %u) page %u: status %d kind 0x%02x type 0x%02x\n",
                        lba, pi, peb[pi].vce, peb[pi].blk, pg, s, rc.m[8], rc.m[9]);
            return NULL;
        }
        rc.pi = pi;
        rc.pg = pg;
        rc.seq = peb[pi].seq;
    }
    if (le32(rc.m + 4 * h) != lba) {
        if (lst.readerr++ < 20)
            fprintf(stderr, "k93lv: sector %u: block %u page %u half %u holds sector %u\n", lba, pi, pg, h,
                    le32(rc.m + 4 * h));
        return NULL;
    }
    return rc.data + h * LV_SEC;
}

/* ---- writing: the head block, sealing, opening, GC ------------------------------------------ */

static int lv_gc(void);

static int seal(void)
{
    static uint8_t page[MAX_PAGE] __attribute__((aligned(8)));
    struct lvsum *s = (struct lvsum *)page;
    uint8_t meta[12];
    memset(page, 0, sizeof(page));
    s->magic = LV_SUMMAGIC;
    s->version = LV_VERSION;
    memcpy(s->uuid, uuid, 16);
    s->seq = peb[head].seq;
    memcpy(s->lba, p2l[head], sizeof(s->lba));
    s->crc = crc32(s, offsetof(struct lvsum, crc));
    mkmeta(meta, (uint32_t)s->seq, (uint32_t)(s->seq >> 32), K_SUM);
    int r = fl_program(head, LV_PPB - 1, page, meta);   /* without it the next start reads the pages */
    if (r)
        lst.progfail++;
    peb[head].state = PB_USED;
    head = -1;
    return r;
}

static int pick_free(void)
{
    int best = -1;
    uint32_t avg = avg_ec(), bec = 0;
    for (unsigned i = 0; i < npeb; i++) {
        if (peb[i].state != PB_FREE)
            continue;
        uint32_t ec = peb[i].ec == NONE ? avg : peb[i].ec;
        if (best < 0 || ec < bec) {
            best = i;
            bec = ec;
        }
    }
    return best;
}

/* head = a block erased just now, with this volume's header on page 0 */
static int open_new(void)
{
    static uint8_t page[MAX_PAGE] __attribute__((aligned(8)));
    if (!in_gc && nfree() <= LV_GC_LOW) {
        lv_gc();
        if (head >= 0)
            return 0;           /* GC left a head with room */
    }
    for (;;) {
        int pi = pick_free();
        if (pi < 0) {
            fprintf(stderr, "k93lv: no free block (%s)\n", in_gc ? "during GC" : "GC found nothing");
            return -1;
        }
        struct peb *b = &peb[pi];
        if (!b->fresh) {        /* a free block found at the start: its content is not trusted */
            uint32_t ec = (b->ec == NONE ? avg_ec() : b->ec) + 1;
            if (fl_erase(pi)) {
                lst.erasefail++;
                b->state = PB_BAD;
                continue;
            }
            lst.erases++;
            b->ec = ec;
        }
        b->fresh = 0;
        b->valid = 0;
        memset(p2l[pi], 0xff, sizeof(p2l[pi]));
        b->seq = ++maxseq;
        memset(page, 0, sizeof(page));
        struct lvhdr *h = (struct lvhdr *)page;
        memcpy(h->magic, LMAGIC, 16);
        h->one = 1;
        h->vce = b->vce;
        h->blk = b->blk;
        h->page0 = 0;
        h->lvmagic = LV_MAGIC;
        h->version = LV_VERSION;
        memcpy(h->uuid, uuid, 16);
        h->seq = b->seq;
        h->ec = b->ec;
        h->nsect = nsect;
        h->npeb = npeb;
        h->crc = crc32(h, offsetof(struct lvhdr, crc));
        uint8_t meta[12];
        mkmeta(meta, (uint32_t)b->seq, (uint32_t)(b->seq >> 32), K_HEAD);
        if (fl_program(pi, 0, page, meta)) {
            lst.progfail++;
            b->state = PB_BAD;
            b->seq = 0;
            continue;
        }
        b->state = PB_OPEN;
        head = pi;
        head_pg = 1;
        return 0;
    }
}

/* one data page: sectors la (bytes 0-4095) and lb (4096-8191), NONE for none */
static int put_page(uint32_t la, uint32_t lb, const uint8_t *plain)
{
    uint8_t meta[12];
    mkmeta(meta, la, lb, K_DATA);
    for (int tries = 0; tries < 8; tries++) {
        if (head < 0 && open_new())
            return -1;
        unsigned pi = head, pg = head_pg++;
        if (fl_program(pi, pg, plain, meta)) {
            /* the block is failing: keep what it holds, write on elsewhere */
            lst.progfail++;
            peb[pi].state = PB_USED;
            head = -1;
            continue;
        }
        lst.pages++;
        place(la, LOC(pi, pg, 0));
        place(lb, LOC(pi, pg, 1));
        if (head_pg > LV_DPAGES)
            seal();
        return 0;
    }
    return -1;
}

static int victim(void)
{
    int best = -1;
    for (unsigned i = 0; i < npeb; i++) {
        const struct peb *b = &peb[i];
        if (b->state != PB_USED || (int)i == head || b->nogc || b->valid >= LV_SPB)
            continue;
        if (best < 0 || b->valid < peb[best].valid)
            best = i;
    }
    return best;
}

/* copy what is still valid in block v to the head, then v is free */
static int gc_move(int v)
{
    static uint8_t out[MAX_PAGE] __attribute__((aligned(8)));
    static uint8_t buf[LV_SEC] __attribute__((aligned(8)));
    uint32_t pend = NONE;
    for (unsigned pg = 1; pg <= LV_DPAGES; pg++)
        for (unsigned h = 0; h < 2; h++) {
            uint32_t lba = p2l[v][SLOT(pg, h)];
            if (lba == NONE || lba >= nsect || l2p[lba] != LOC(v, pg, h))
                continue;
            const uint8_t *d = read_slot(LOC(v, pg, h), lba);
            if (!d) {
                fprintf(stderr, "k93lv: GC: block %d is not readable at page %u; left as it is\n", v, pg);
                peb[v].nogc = 1;
                if (pend != NONE) {
                    memset(out + LV_SEC, 0, LV_SEC);
                    if (put_page(pend, NONE, out))
                        return -1;
                }
                return -1;
            }
            memcpy(buf, d, LV_SEC);
            if (pend == NONE) {
                memcpy(out, buf, LV_SEC);
                pend = lba;
            } else {
                memcpy(out + LV_SEC, buf, LV_SEC);
                if (put_page(pend, lba, out))
                    return -1;
                pend = NONE;
            }
            lst.gcmoved++;
        }
    if (pend != NONE) {
        memset(out + LV_SEC, 0, LV_SEC);
        if (put_page(pend, NONE, out))
            return -1;
    }
    if (peb[v].valid)
        fprintf(stderr, "k93lv: GC: block %d still counts %u valid sectors\n", v, peb[v].valid);
    /* erased now, not when it is reused: a trimmed sector's last copy must not
     * come back from it after a restart and make it "in use" again */
    lst.gcs++;
    uint32_t ec = (peb[v].ec == NONE ? avg_ec() : peb[v].ec) + 1;
    peb[v].seq = 0;
    memset(p2l[v], 0xff, sizeof(p2l[v]));
    if (fl_erase(v)) {
        lst.erasefail++;
        peb[v].state = PB_BAD;
        return 0;
    }
    lst.erases++;
    peb[v].ec = ec;
    peb[v].state = PB_FREE;
    peb[v].fresh = 1;
    return 0;
}

static int lv_gc(void)
{
    in_gc = 1;
    int r = 0;
    while (nfree() <= LV_GC_LOW) {
        int v = victim();
        if (v < 0) {
            r = -1;
            break;
        }
        if (gc_move(v)) {
            r = -1;
            if (nfree() == 0)
                break;
        }
    }
    in_gc = 0;
    return r;
}

/* ---- the block device ---------------------------------------------------------------------- */

static int lv_read(uint64_t off, uint8_t *buf, uint32_t len)
{
    if (off % LV_SEC || len % LV_SEC || off / LV_SEC + len / LV_SEC > nsect)
        return -1;
    for (uint32_t i = 0, s = off / LV_SEC; i < len / LV_SEC; i++, s++) {
        uint32_t loc = l2p[s];
        if (loc == NONE) {
            memset(buf + (size_t)i * LV_SEC, 0, LV_SEC);
            continue;
        }
        const uint8_t *d = read_slot(loc, s);
        if (!d)
            return -1;
        memcpy(buf + (size_t)i * LV_SEC, d, LV_SEC);
    }
    lst.rd += len;
    return 0;
}

static int lv_write(uint64_t off, const uint8_t *buf, uint32_t len)
{
    static uint8_t tail[MAX_PAGE] __attribute__((aligned(8)));
    if (lv_ro || off % LV_SEC || len % LV_SEC || off / LV_SEC + len / LV_SEC > nsect)
        return -1;
    uint32_t s = off / LV_SEC, n = len / LV_SEC;
    for (uint32_t i = 0; i < n; i += 2) {
        int r;
        if (i + 1 < n) {
            r = put_page(s + i, s + i + 1, buf + (size_t)i * LV_SEC);
        } else {
            memcpy(tail, buf + (size_t)i * LV_SEC, LV_SEC);
            memset(tail + LV_SEC, 0, LV_SEC);
            r = put_page(s + i, NONE, tail);
        }
        if (r)
            return -1;
    }
    lst.wr += len;
    return 0;
}

static int lv_flush(void)
{
    return 0;                   /* every write is on the flash before it is acknowledged */
}

static int lv_trim(uint64_t off, uint32_t len)
{
    uint64_t a = (off + LV_SEC - 1) / LV_SEC, b = (off + len) / LV_SEC;
    if (b > nsect)
        b = nsect;
    for (uint64_t s = a; s < b; s++)
        if (l2p[s] != NONE) {
            peb[LOC_PEB(l2p[s])].valid--;
            l2p[s] = NONE;
            lst.trims++;
        }
    return 0;
}

/* ---- start: headers, then the blocks oldest first ---------------------------------------------- */

static int by_seq(const void *a, const void *b)
{
    uint64_t x = peb[*(const int *)a].seq, y = peb[*(const int *)b].seq;
    return x < y ? -1 : x > y;
}

static void random_bytes(uint8_t *p, size_t n)
{
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0 || read(fd, p, n) != (ssize_t)n) {
        uint64_t x = now_us() ^ (uint64_t)getpid() << 32;
        for (size_t i = 0; i < n; i++) {
            x = x * 6364136223846793005ull + 1442695040888963407ull;
            p[i] = x >> 56;
        }
    }
    if (fd >= 0)
        close(fd);
}

static int lv_mount(int verbose)
{
    static uint8_t page[MAX_PAGE] __attribute__((aligned(8)));
    static int order[LV_MAXPEB];
    uint8_t m[META_BYTES];
    unsigned nhdr = 0, nbad = 0, nsum = 0, nscan = 0, other = 0;
    int have = 0;
    uint32_t vns = 0;
    uint64_t t0 = now_us();

    memset(l2p, 0xff, sizeof(l2p));
    maxseq = 0;
    head = -1;
    rc.pi = -1;
    for (unsigned pi = 0; pi < npeb; pi++) {
        struct peb *b = &peb[pi];
        memset(p2l[pi], 0xff, sizeof(p2l[pi]));
        b->valid = 0;
        b->seq = 0;
        b->ec = NONE;
        b->nogc = 0;
        b->fresh = 0;
        int s = fl_read(pi, 0, page, m);
        if (s == 2) {
            b->state = PB_FREE;
            continue;
        }
        if (s != 1 || m[9] != T_LINUX || !is_ours(page, m, b->vce, b->blk)) {
            b->state = PB_BAD;          /* not erased, not Linux's: never touched */
            nbad++;
            fprintf(stderr, "k93lv: vCE %u block %u: page 0 is %s -- not used\n", b->vce, b->blk,
                    s != 1 ? "unreadable" : "not Linux's");
            continue;
        }
        struct lvhdr h;
        memcpy(&h, page, sizeof(h));
        if (m[8] != K_HEAD || h.lvmagic != LV_MAGIC || h.version != LV_VERSION ||
            h.crc != crc32(&h, offsetof(struct lvhdr, crc))) {
            b->state = PB_FREE;         /* Linux's, not a volume block (k93nandw's test pattern) */
            other++;
            continue;
        }
        if (!have) {
            memcpy(uuid, h.uuid, 16);
            vns = h.nsect;
            have = 1;
        } else if (memcmp(uuid, h.uuid, 16) || h.nsect != vns) {
            fprintf(stderr, "k93lv: blocks of two volumes (vCE %u block %u): `k93lv format`\n", b->vce, b->blk);
            return -1;
        }
        b->state = PB_USED;
        b->seq = h.seq;
        b->ec = h.ec;
        if (h.seq > maxseq)
            maxseq = h.seq;
        order[nhdr++] = pi;
    }
    unsigned usable = npeb - nbad;
    unsigned cap = usable > LV_GC_LOW + 2 ? (usable - LV_GC_LOW - 2) * LV_SPB : 0;
    if (!have) {
        random_bytes(uuid, 16);
        nsect = (uint32_t)((uint64_t)usable * LV_SPB * 3 / 4) & ~255u;    /* a quarter kept for GC */
        if (verbose)
            fprintf(stderr, "k93lv: no volume yet: a new one of %u MB on %u blocks\n", nsect / 256, usable);
    } else {
        nsect = vns;
    }
    if (!nsect || nsect > cap) {
        fprintf(stderr, "k93lv: the volume (%u sectors) does not fit %u usable blocks; read-only\n", nsect, usable);
        lv_ro = 1;
        if (!nsect)
            return -1;
    }
    qsort(order, nhdr, sizeof(order[0]), by_seq);
    for (unsigned k = 0; k < nhdr; k++) {
        int pi = order[k];
        const struct lvsum *s = (const struct lvsum *)page;
        if (fl_read(pi, LV_PPB - 1, page, m) == 1 && m[8] == K_SUM && s->magic == LV_SUMMAGIC &&
            s->version == LV_VERSION && s->seq == peb[pi].seq && !memcmp(s->uuid, uuid, 16) &&
            s->crc == crc32(s, offsetof(struct lvsum, crc))) {
            static uint32_t lba[LV_SPB];
            memcpy(lba, s->lba, sizeof(lba));
            for (unsigned pg = 1; pg <= LV_DPAGES; pg++)
                for (unsigned h = 0; h < 2; h++)
                    place(lba[SLOT(pg, h)], LOC(pi, pg, h));
            nsum++;
            continue;
        }
        /* no summary: the block open when Linux stopped (or a failing one) */
        nscan++;
        for (unsigned pg = 1; pg <= LV_DPAGES; pg++) {
            int st = fl_read(pi, pg, NULL, m);
            if (st == 2)
                break;
            if (st != 1 || m[8] != K_DATA || m[9] != T_LINUX)
                continue;               /* torn by a power cut: never acknowledged */
            place(le32(m), LOC(pi, pg, 0));
            place(le32(m + 4), LOC(pi, pg, 1));
        }
    }
    uint32_t avg = avg_ec();
    unsigned used = 0, nvalid = 0;
    for (unsigned pi = 0; pi < npeb; pi++) {
        if (peb[pi].state == PB_USED && !peb[pi].valid)
            peb[pi].state = PB_FREE;   /* everything in it was written again later */
        used += peb[pi].state == PB_USED;
        nvalid += peb[pi].valid;
    }
    if (verbose)
        fprintf(stderr, "k93lv: volume %02x%02x%02x%02x..., %u MB (%u sectors): %u blocks -- %u in use (%u by summary, "
                "%u read page by page), %u free, %u not used; %u sectors written (%u MB), last sequence %llu, "
                "erase counts ~%u (%.1f s)\n", uuid[0], uuid[1], uuid[2], uuid[3], nsect / 256, nsect, npeb, used,
                nsum, nscan, nfree(), nbad, nvalid, nvalid / 256, (unsigned long long)maxseq, avg,
                (now_us() - t0) / 1e6);
    (void)other;
    return 0;
}

/* ---- the selftest (host, in-memory flash) ---------------------------------------------- */
#ifdef K93LV_SIM
static uint64_t xs = 0x9e3779b97f4a7c15ull;
static uint64_t rnd(void)
{
    xs ^= xs << 13;
    xs ^= xs >> 7;
    xs ^= xs << 17;
    return xs;
}

/* a sector's content: its number and version, then bytes made from them */
static void fill(uint8_t *p, uint32_t lba, uint32_t ver)
{
    uint32_t x = lba * 2654435761u ^ ver * 40503u ^ 0x5bd1e995u;
    memcpy(p, &lba, 4);
    memcpy(p + 4, &ver, 4);
    for (unsigned i = 8; i < LV_SEC; i += 4) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        memcpy(p + i, &x, 4);
    }
}

/* ver[s]: last acknowledged version (0 = never written: zeroes), alt[s]: also
 * acceptable (the request a power cut interrupted, or anything after a trim) */
static uint32_t *ver, *alt;
static uint8_t *trimmed;

static int verify_all(const char *when)
{
    static uint8_t got[LV_SEC], want[LV_SEC];
    unsigned bad = 0;
    for (uint32_t s = 0; s < nsect; s++) {
        if (lv_read((uint64_t)s * LV_SEC, got, LV_SEC)) {
            if (bad++ < 5)
                printf("  %s: sector %u: read error\n", when, s);
            continue;
        }
        if (trimmed[s])
            continue;
        uint32_t v;
        memcpy(&v, got + 4, 4);
        int ok = 0;
        uint32_t cand[2] = { ver[s], alt[s] };
        for (int k = 0; k < (alt[s] ? 2 : 1) && !ok; k++) {
            if (cand[k] == 0)
                memset(want, 0, LV_SEC);
            else
                fill(want, s, cand[k]);
            ok = !memcmp(got, want, LV_SEC);
        }
        if (!ok && bad++ < 5)
            printf("  %s: sector %u: version %u, wanted %u%s%u\n", when, s, v, ver[s], alt[s] ? " or " : "",
                   alt[s] ? alt[s] : 0);
    }
    return bad;
}

static int sim_restart(void)
{
    sim_dead = 0;
    sim_ops_left = -1;
    return lv_mount(0);
}

static int cmd_selftest_lv(void)
{
    static uint8_t buf[64 * LV_SEC];
    unsigned fails = 0, cuts = 0, rounds = 0, nrounds = 400;
    if (getenv("K93LV_SEED"))
        xs ^= strtoull(getenv("K93LV_SEED"), NULL, 0) * 0x2545f4914f6cdd1dull;
    if (getenv("K93LV_ROUNDS"))
        nrounds = strtoul(getenv("K93LV_ROUNDS"), NULL, 0);
    npeb = 40;
    sim_pagesz = 8192;
    sim_data = calloc((size_t)npeb * LV_PPB, sim_pagesz + 12);
    for (unsigned i = 0; i < npeb; i++) {
        peb[i].vce = i % 4;
        peb[i].blk = 4000 + i / 4;
    }
    if (!sim_data || lv_mount(1))
        return 1;
    ver = calloc(nsect, 4);
    alt = calloc(nsect, 4);
    trimmed = calloc(nsect, 1);
    uint32_t nextver = 1;
    printf("SIM: %u blocks, volume %u sectors (%u MB)\n", npeb, nsect, nsect / 256);
    for (rounds = 0; rounds < nrounds; rounds++) {
        int cut = rounds % 7 == 6;
        if (cut)
            sim_ops_left = rnd() % 300;
        /* a burst of writes and trims: hot sectors, cold ones, big and small */
        for (int k = 0; k < 120 && !sim_dead; k++) {
            uint32_t n = rnd() % 4 == 0 ? 1 + rnd() % 64 : 1 + rnd() % 3;
            uint32_t s = rnd() % 3 ? rnd() % (nsect / 8) : rnd() % (nsect - n);
            if (s + n > nsect)
                s = nsect - n;
            if (rnd() % 20 == 0) {
                lv_trim((uint64_t)s * LV_SEC, n * LV_SEC);
                for (uint32_t i = 0; i < n; i++)
                    trimmed[s + i] = 1;
                continue;
            }
            uint32_t v = nextver++;
            for (uint32_t i = 0; i < n; i++)
                fill(buf + (size_t)i * LV_SEC, s + i, v);
            for (uint32_t i = 0; i < n; i++)
                alt[s + i] = v;                 /* in flight */
            if (lv_write((uint64_t)s * LV_SEC, buf, n * LV_SEC)) {
                if (!sim_dead) {
                    printf("  round %u: write failed without a power cut\n", rounds);
                    fails++;
                }
                break;                          /* the cut: this request stays "in flight" */
            }
            for (uint32_t i = 0; i < n; i++) {
                ver[s + i] = v;
                alt[s + i] = 0;
                trimmed[s + i] = 0;
            }
        }
        if (cut || rounds % 10 == 9) {
            cuts += cut;
            if (sim_restart()) {
                printf("  round %u: the volume did not mount\n", rounds);
                return 1;
            }
        }
        if (rounds % 25 == 24 || cut) {
            unsigned b = verify_all(cut ? "after a power cut" : "after a restart");
            if (b) {
                printf("  round %u: %u bad sector(s)\n", rounds, b);
                fails++;
                if (fails > 3)
                    break;
            }
            for (uint32_t s = 0; s < nsect; s++)
                if (alt[s]) {                   /* settle what the cut decided */
                    uint8_t got[LV_SEC];
                    uint32_t v;
                    lv_read((uint64_t)s * LV_SEC, got, LV_SEC);
                    memcpy(&v, got + 4, 4);
                    if (v == alt[s])
                        ver[s] = alt[s];
                    alt[s] = 0;
                }
        }
    }
    unsigned b = verify_all("at the end");
    printf("SIM: %u rounds, %u power cuts; %llu pages programmed, %llu erases, %llu GC passes moving %llu sectors, "
           "%llu trims; %u failure(s), %u bad sector(s) at the end\n", rounds, cuts, lst.pages, lst.erases, lst.gcs,
           lst.gcmoved, lst.trims, fails, b);
    unsigned maxec = 0, minec = ~0u;
    for (unsigned i = 0; i < npeb; i++) {
        if (peb[i].ec != NONE && peb[i].ec > maxec)
            maxec = peb[i].ec;
        if (peb[i].ec != NONE && peb[i].ec < minec)
            minec = peb[i].ec;
    }
    printf("SIM: erase counts %u..%u; %s\n", minec, maxec, fails || b ? "SELFTEST FAILED" : "FTL selftest ok");
    return fails || b;
}
#endif

/* ---- on the iPad: the pool check, then the blocks of the volume ------------------------------ */

#ifndef K93LV_SIM
static int lv_blocks(void)
{
    static struct pool P;
    npeb = 0;
    ctx_fast = 1;
    int t = trace;
    trace = 0;
    for (unsigned v = 0; v < NCE; v++) {
        if (pool_scan(v, &P, NULL) || P.stop) {
            fprintf(stderr, "k93lv: vCE %u: the pool check failed: none of its blocks is used\n", v);
            continue;
        }
        for (unsigned i = 0; i < P.nusable && npeb < LV_MAXPEB; i++) {
            peb[npeb].vce = v;
            peb[npeb].blk = P.usable[i];
            allow_block(v, P.usable[i]);
            npeb++;
        }
    }
    trace = t;
    fprintf(stderr, "k93lv: %u blocks passed the pool check\n", npeb);
    return npeb ? 0 : -1;
}

static const struct export_ops LV_OPS = { lv_read, lv_write, lv_flush, lv_trim, NULL };

static int lv_start(void)
{
    whiten_init();
    aes_init();
    aesf_init();
    aes_expand(KEY_META, RK_META);
    aese_init();
    if (aesf_check())
        return -1;
    trace = 0;
    open_hw();
    if (load_state() || vsvfl_setup())
        return -1;
    if (BPC != 2 || G.c->blocks != 4096 || G.c->ppb != LV_PPB || G.c->page != 2 * LV_SEC || VENDOR != 0x150011) {
        fprintf(stderr, "k93lv: not the geometry this volume knows\n");
        return -1;
    }
    for (int b = 0; b < NBUS; b++)
        if ((cemap >> (b * 8)) & 0xff && !claim(b))
            return -1;
    return 0;
}

static int cmd_serve(char **names, int n)
{
    static struct export ex[12];
    static char *deflt[] = { "System" };
    int nex = 0;
    for (unsigned i = 0; i < PCACHE; i++)
        pcache[i].lpn = NONE;
    if (vfl_open(0) || build_map(1) || lwvm_load())
        return 1;
    if (!n) {
        names = deflt;
        n = 1;
    }
    for (int i = 0; i < n && nex < 11; i++) {
        struct part *p = part_by_name(names[i]);
        if (!p) {
            fprintf(stderr, "k93iosnand: no LwVM partition %s\n", names[i]);
            continue;
        }
        memset(&ex[nex], 0, sizeof(ex[nex]));
        ex[nex++].p = p;
    }
    if (lv_blocks() == 0 && lv_mount(1) == 0) {
        memset(&ex[nex], 0, sizeof(ex[nex]));
        ex[nex].ops = &LV_OPS;
        ex[nex].size = (uint64_t)nsect * LV_SEC;
        ex[nex].name = lv_ro ? "Linux volume (read-only: does not fit)" : "Linux volume";
        nex++;
    } else {
        fprintf(stderr, "k93lv: no Linux volume this time\n");
    }
    if (!nex)
        return 1;
    if (parallel)
        start_worker();
    int r = serve_exports(ex, nex);
    fprintf(stderr, "k93lv: %llu MB read, %llu MB written (%llu pages), %llu erases, %llu GC passes (%llu sectors "
            "moved), %llu program / %llu erase failures, %llu read errors\n", lst.rd >> 20, lst.wr >> 20, lst.pages,
            lst.erases, lst.gcs, lst.gcmoved, lst.progfail, lst.erasefail, lst.readerr);
    return r;
}

static int cmd_info(void)
{
    if (lv_blocks() || lv_mount(1))
        return 1;
    unsigned hist[6] = { 0 };
    for (unsigned i = 0; i < npeb; i++)
        if (peb[i].state == PB_USED)
            hist[peb[i].valid * 5 / LV_SPB]++;
    printf("Linux volume: %u MB, %u blocks (%u free); used blocks by valid share 0-20%%: %u, -40%%: %u, -60%%: %u, "
           "-80%%: %u, -100%%: %u\n", nsect / 256, npeb, nfree(), hist[0], hist[1], hist[2], hist[3], hist[4] + hist[5]);
    for (unsigned i = 0; i < npeb; i++)
        if (peb[i].state != PB_FREE)
            printf("  vCE %u block %u: %s, seq %llu, ec %d, %u valid\n", peb[i].vce, peb[i].blk, PB_NAME[peb[i].state],
                   (unsigned long long)peb[i].seq, peb[i].ec == NONE ? -1 : (int)peb[i].ec, peb[i].valid);
    return 0;
}

static int cmd_format(void)
{
    unsigned n = 0, skip = 0;
    if (lv_blocks())
        return 1;
    for (unsigned i = 0; i < npeb; i++) {
        int last, s = block_state(peb[i].vce, peb[i].blk, &last, 0);
        if (s == 1)
            continue;
        if (s != 2) {
            fprintf(stderr, "k93lv: vCE %u block %u is neither erased nor Linux's: left alone\n", peb[i].vce,
                    peb[i].blk);
            skip++;
            continue;
        }
        if (fl_erase(i))
            skip++;
        else
            n++;
    }
    printf("k93lv: %u block(s) erased, %u left alone; the next serve starts an empty volume\n", n, skip);
    return skip ? 1 : 0;
}
#endif

int main(int argc, char **argv)
{
    if (argc > 2 && !strcmp(argv[1], "-j2")) {
        parallel = 1;
        argv++;
        argc--;
    }
    if (argc < 2) {
        fprintf(stderr, "usage: k93lv [-j2] serve [PARTITION...] | info | format | selftest\n");
        return 1;
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
#ifdef K93LV_SIM
    if (!strcmp(argv[1], "selftest"))
        return cmd_selftest_lv();
    fprintf(stderr, "k93lv: the host build only has `selftest`\n");
    return 1;
#else
    if (!strcmp(argv[1], "selftest")) {
        fprintf(stderr, "k93lv: selftest is the host build's (-DK93NAND_SIM -DK93LV_SIM)\n");
        return 1;
    }
    if (lv_start())
        return 1;
    if (!strcmp(argv[1], "serve"))
        return cmd_serve(argv + 2, argc - 2);
    if (!strcmp(argv[1], "info"))
        return cmd_info();
    if (!strcmp(argv[1], "format"))
        return cmd_format();
    fprintf(stderr, "k93lv: unknown command %s\n", argv[1]);
    return 1;
#endif
}
