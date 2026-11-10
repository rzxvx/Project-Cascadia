/* k93iosnand -- the iPad 2's own iOS partitions, read-only, as Linux block devices.
 *
 * NAND -> k93nand.c's page reads (PIO, BCH, metadata unwhitening, AES) ->
 * VSVFL (virtual page -> CE, row) -> YAFTL's map, rebuilt from the
 * superblocks' tables of contents -> disk0 (8 KB logical pages) -> LwVM -> NBD.
 * docs/research/k93-nand.md has every layer; this is its section 11, step 1.
 *
 * The FTL's own context is not parsed.  As iBEC's "full NAND R/O restore"
 * does (YAFTL_Open 0x9ff30616): every superblock is classified by its first
 * readable page, the user-data ones are taken newest first (the sequence number
 * every page of a superblock carries), and each one's table of contents (its
 * last page: one LPN per user page, META key, IV = row) fills in the LPNs not
 * seen yet, later offsets first.  A superblock without a readable table (the
 * one still being filled) is read page by page.  So the map costs about two
 * page reads per superblock, a few seconds, and is not cached.
 *
 *   k93iosnand vfl              the VSVFL context of each CE: bad blocks, replacements
 *   k93iosnand map              rebuild the map, print a summary
 *   k93iosnand parts            the LwVM partitions on disk0
 *   k93iosnand read LPN [N]     N x 8 KB of disk0 to stdout, decrypted
 *   k93iosnand bench NAME [MB]  read MB of a partition one bus at a time, then
 *                               both at once (-j2's way): the same bytes?  how fast?
 *   k93iosnand serve [NAME...]  attach LwVM partitions (default: System) to
 *                               /dev/nbd0, /dev/nbd1 ... and serve them until
 *                               SIGTERM; read-only, writes are refused
 *
 * Needs `k93nand id` to have been run once this boot (its k93nand.state: the
 * ce-bitmap and the chip).  Nothing here can program or erase: the only NAND
 * operations are k93nand.c's, whose allowlist has none.
 *
 * Bad blocks: the VSVFL context of each CE (its newest copy, checksummed) lists
 * the blocks it replaced and with which reserved block; vpn_loc follows it.
 * Every page read is still checked against its own metadata (type, LPN), so a
 * page that is not where the map says fails with EIO instead of returning
 * someone else's data.
 *
 * `serve` is what the iPad 2's NAND root stands on (initramfs/init): the map
 * is cut down to the served partitions (~0.6 MB for System), requests are
 * capped at the server's buffer, and nothing on the request path allocates.
 *
 * Build (from devices/a5, in the cascadia-build image):
 *   arm-linux-gnueabihf-gcc -static -O2 -Wall -pthread -D_FILE_OFFSET_BITS=64 \
 *       -o build/k93/tools/k93iosnand tools/nand/k93iosnand.c
 */
#define main k93nand_main
#include "k93nand.c"
#undef main

#include <arpa/inet.h>
#include <endian.h>
#include <linux/nbd.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/wait.h>

#define NONE            0xffffffffu
#define T_USER          0x10
#define T_BTOC          0x08

/* ---- AES-128-CBC decryption with tables ---------------------------------------
 * The equivalent inverse cipher (as rijndael-alg-fst.c): four 1 KB tables, the
 * decryption round keys computed once per key.  k93nand.c's byte-wise AES, with
 * its bit-loop GF multiplications, spends milliseconds on every 8 KB page, more
 * than the NAND read; main() checks the two against each other. */
static uint32_t Td[4][256];
static uint32_t DK_USER[44], DK_META[44];

#define GETU32(p)       ((uint32_t)(p)[0] << 24 | (uint32_t)(p)[1] << 16 | (uint32_t)(p)[2] << 8 | (p)[3])

static void aesf_key(const uint8_t key[16], uint32_t dk[44])
{
    uint8_t rk[176];
    uint32_t ek[44];
    aes_expand(key, rk);
    for (int i = 0; i < 44; i++)
        ek[i] = GETU32(rk + 4 * i);
    for (int r = 0; r <= 10; r++)                       /* round keys in reverse */
        for (int k = 0; k < 4; k++)
            dk[4 * r + k] = ek[4 * (10 - r) + k];
    for (int i = 4; i < 40; i++) {                      /* InvMixColumns on rounds 1-9 */
        uint32_t w = dk[i];
        dk[i] = Td[0][SBOX[w >> 24]] ^ Td[1][SBOX[w >> 16 & 0xff]] ^ Td[2][SBOX[w >> 8 & 0xff]] ^
                Td[3][SBOX[w & 0xff]];
    }
}

static void aesf_init(void)
{
    for (int x = 0; x < 256; x++) {
        uint8_t s = ISBOX[x];
        uint32_t w = (uint32_t)gmul(s, 14) << 24 | (uint32_t)gmul(s, 9) << 16 | (uint32_t)gmul(s, 13) << 8 |
                     gmul(s, 11);
        Td[0][x] = w;
        Td[1][x] = w >> 8 | w << 24;
        Td[2][x] = w >> 16 | w << 16;
        Td[3][x] = w >> 24 | w << 8;
    }
    aesf_key(KEY_USER, DK_USER);
    aesf_key(KEY_META, DK_META);
}

static void aesf_block(const uint32_t *rk, const uint8_t *in, uint32_t o[4])
{
    uint32_t s0 = GETU32(in) ^ rk[0], s1 = GETU32(in + 4) ^ rk[1];
    uint32_t s2 = GETU32(in + 8) ^ rk[2], s3 = GETU32(in + 12) ^ rk[3], t0, t1, t2, t3;
    for (int r = 1; r < 10; r++) {
        rk += 4;
        t0 = Td[0][s0 >> 24] ^ Td[1][s3 >> 16 & 0xff] ^ Td[2][s2 >> 8 & 0xff] ^ Td[3][s1 & 0xff] ^ rk[0];
        t1 = Td[0][s1 >> 24] ^ Td[1][s0 >> 16 & 0xff] ^ Td[2][s3 >> 8 & 0xff] ^ Td[3][s2 & 0xff] ^ rk[1];
        t2 = Td[0][s2 >> 24] ^ Td[1][s1 >> 16 & 0xff] ^ Td[2][s0 >> 8 & 0xff] ^ Td[3][s3 & 0xff] ^ rk[2];
        t3 = Td[0][s3 >> 24] ^ Td[1][s2 >> 16 & 0xff] ^ Td[2][s1 >> 8 & 0xff] ^ Td[3][s0 & 0xff] ^ rk[3];
        s0 = t0, s1 = t1, s2 = t2, s3 = t3;
    }
    rk += 4;
#define IS(x, n)        ((uint32_t)ISBOX[(x) >> (n) & 0xff] << (n))
    o[0] = (IS(s0, 24) | IS(s3, 16) | IS(s2, 8) | IS(s1, 0)) ^ rk[0];
    o[1] = (IS(s1, 24) | IS(s0, 16) | IS(s3, 8) | IS(s2, 0)) ^ rk[1];
    o[2] = (IS(s2, 24) | IS(s1, 16) | IS(s0, 8) | IS(s3, 0)) ^ rk[2];
    o[3] = (IS(s3, 24) | IS(s2, 16) | IS(s1, 8) | IS(s0, 0)) ^ rk[3];
#undef IS
}

/* as aes_cbc_decrypt(key, seed, ...); in == out is fine */
static void aesf_cbc(const uint32_t *dk, uint32_t seed, const uint8_t *in, uint8_t *out, size_t len)
{
    uint8_t iv[16], c[16];
    uint32_t o[4];
    iv_for(seed, iv);
    for (size_t p = 0; p + 16 <= len; p += 16) {
        memcpy(c, in + p, 16);
        aesf_block(dk, c, o);
        for (int i = 0; i < 16; i++)
            out[p + i] = (uint8_t)(o[i / 4] >> (24 - 8 * (i % 4))) ^ iv[i];
        memcpy(iv, c, 16);
    }
}

static int aesf_check(void)
{
    uint8_t in[256], a[256], b[256];
    uint32_t x = 0x93939393u;
    for (int i = 0; i < 256; i++) {
        x = x * 1103515245u + 12345u;
        in[i] = x >> 16;
    }
    for (int k = 0; k < 2; k++) {
        aes_cbc_decrypt(k ? KEY_META : KEY_USER, 0x1234567u + k, in, a, sizeof(in));
        aesf_cbc(k ? DK_META : DK_USER, 0x1234567u + k, in, b, sizeof(in));
        if (memcmp(a, b, sizeof(a))) {
            fprintf(stderr, "k93iosnand: table AES disagrees with k93nand.c's (key %d)\n", k);
            return -1;
        }
    }
    return 0;
}

/* ---- VSVFL geometry (docs/research/k93-nand.md section 7) ---------------- */

static unsigned NCE, BPC, TB, PPSB, NSB;
static uint32_t VENDOR;
static int vce_bus[16], vce_ce[16];

static int vsvfl_setup(void)
{
    int nce[NBUS] = { 0, 0 };
    NCE = 0;
    for (int k = 0; k < NCE_BUS; k++)
        for (int b = 0; b < NBUS; b++)
            if (cemap & 1u << (b * 8 + k)) {
                vce_bus[NCE] = b;
                vce_ce[NCE] = k;
                NCE++;
                nce[b]++;
            }
    int nbus_used = (nce[0] > 0) + (nce[1] > 0);
    const struct board *bd = NULL;
    for (size_t i = 0; i < sizeof(BOARDS) / sizeof(BOARDS[0]); i++)
        if (!memcmp(BOARDS[i].id, chip_id, 4) && BOARDS[i].nbus == nbus_used &&
            BOARDS[i].nce == (nce[0] ? nce[0] : nce[1]))
            bd = &BOARDS[i];
    if (!bd) {
        fprintf(stderr, "k93iosnand: no board entry for this chip and CE count\n");
        return -1;
    }
    VENDOR = bd->vendor;
    BPC = vsvfl_banks(VENDOR);
    switch (VENDOR) {   /* the block conversions known (k93-nand.md section 7) */
    case 0x100014: case 0x120014: case 0x140014: case 0x150011:
    case 0x010012: case 0x110012: case 0x130012: case 0x150010: case 0x160010:
    case 0x010001:
        break;
    default:
        fprintf(stderr, "k93iosnand: VSVFL vendor type 0x%06x: block conversion not known\n", VENDOR);
        return -1;
    }
    TB = NCE * BPC;
    PPSB = G.c->ppb * TB;
    NSB = G.c->blocks / BPC;    /* until the VFL context says how many are usable */
    return 0;
}

/* (bank within the CE, virtual block) -> physical block on the CE */
static uint32_t conv(uint32_t sub, uint32_t vblock)
{
    switch (VENDOR) {
    case 0x100014: case 0x120014: case 0x140014: case 0x150011:
        return 2 * vblock + (sub & 1);                  /* even blocks bank 0, odd bank 1 */
    case 0x010001:
        return vblock;
    default:                                            /* bank 1 is the CE's upper half */
        return vblock + (sub & 1) * (G.c->blocks / 2);
    }
}

/* one physical page of a virtual CE: status 1 ok, 2 erased, < 0 unreadable;
 * m = unwhitened metadata */
static uint8_t pdata[MAX_PAGE] __attribute__((aligned(8)));

static int read_phys(int vce, uint32_t row, uint8_t *data, uint8_t m[META_BYTES])
{
    struct eccres e;
    uint8_t raw[META_BYTES], nst;
    if (read_page(vce_bus[vce], vce_ce[vce], row, data, raw, 0, G.sectors, &e, &nst))
        return -1;
    if (e.status == 2)
        memset(m, 0xff, META_BYTES);
    else
        meta_unwhiten(raw, m, row);
    return e.status;
}

/* ---- the VSVFL context of each virtual CE: bad blocks and their replacements
 * (section 7; VFL_Open 0x9ff33120, _RemapBlock 0x9ff32e10) ------------------- */

#define VFL_MAPN        820

static struct vfl {
    uint16_t map[VFL_MAPN];     /* reserved slot i -> the bad block it replaces */
    unsigned nmap, nres, usable, pool, nrep;
    uint32_t usn, blk, page;
} V[16];

static int vfl_ctx_ok(const uint8_t *c)
{
    uint32_t sum = 0, x = 0;
    for (int i = 0; i < 510; i++) {
        uint32_t w = le32(c + 4 * i);
        sum += w;
        x ^= w;
    }
    return le32(c + 0x7f8) == sum + 0xaabbccddu && le32(c + 0x7fc) == (x ^ 0xaabbccddu);
}

static uint32_t le16(const uint8_t *p) { return p[0] | p[1] << 8; }

/* page 0 of blocks 1..199: the first context page names the 4 context blocks;
 * the current one has the smallest non-zero usn on its page 0 (it counts down
 * from 0xffffffff: a context that never moved has that); in it, sets of 8
 * copies at pages 0, 8, 16 ..., the last one written is the newest. */
static int vfl_load(int vce, int verbose)
{
    static uint8_t ctx[MAX_PAGE], best[MAX_PAGE];
    uint8_t m[META_BYTES];
    uint32_t row, first = 0, cb[4], cur = 0, usn = NONE;
    struct vfl *v = &V[vce];

    for (uint32_t blk = 1; blk < 200 && !first; blk++) {
        row = row_of(&G, blk, 0);
        if (read_phys(vce, row, pdata, m) == 1 && m[8] == 0 && m[9] == 0x80) {
            aesf_cbc(DK_META, row, pdata, ctx, G.c->page);
            if (vfl_ctx_ok(ctx))
                first = blk;
        }
    }
    if (!first) {
        fprintf(stderr, "k93iosnand: vCE %d: no VFL context page in blocks 1-199\n", vce);
        return -1;
    }
    for (int i = 0; i < 4; i++) {
        cb[i] = le16(ctx + 0x68e + 2 * i);
        if (!cb[i] || cb[i] >= G.c->blocks)
            continue;
        if (read_phys(vce, row_of(&G, cb[i], 0), pdata, m) == 1 && m[9] == 0x80 && le32(m) && le32(m) <= usn) {
            usn = le32(m);
            cur = cb[i];
        }
    }
    if (!cur) {
        fprintf(stderr, "k93iosnand: vCE %d: none of the context blocks %u %u %u %u is readable\n", vce, cb[0], cb[1],
                cb[2], cb[3]);
        return -1;
    }
    int have = 0;
    for (uint32_t set = 0; set < G.c->ppb; set += 8) {
        int st = 0, erased = 0;
        for (uint32_t pg = set; pg < set + 8 && pg < G.c->ppb; pg++) {
            row = row_of(&G, cur, pg);
            st = read_phys(vce, row, pdata, m);
            if (st == 2) {
                erased = pg == set;
                break;
            }
            if (st != 1 || m[9] != 0x80)
                continue;
            aesf_cbc(DK_META, row, pdata, ctx, G.c->page);
            if (!vfl_ctx_ok(ctx))
                continue;
            memcpy(best, ctx, 0x800);
            v->page = pg;
            have = 1;
            break;
        }
        if (erased)
            break;
    }
    if (!have) {
        fprintf(stderr, "k93iosnand: vCE %d: no good copy in context block %u\n", vce, cur);
        return -1;
    }
    v->usn = usn;
    v->blk = cur;
    v->usable = le16(best + 0x696);
    v->pool = le16(best + 0x698);
    if (le32(best + 8) != 2 || v->pool > G.c->blocks / BPC || v->usable > v->pool) {
        fprintf(stderr, "k93iosnand: vCE %d: context FTL type %u, usable %u, pool start %u: not YAFTL as known\n", vce,
                le32(best + 8), v->usable, v->pool);
        return -1;
    }
    v->nres = G.c->blocks / BPC - v->pool;
    v->nmap = v->nres * BPC < VFL_MAPN ? v->nres * BPC : VFL_MAPN;
    v->nrep = 0;
    for (unsigned i = 0; i < v->nmap; i++) {
        v->map[i] = le16(best + 0x26 + 2 * i);
        v->nrep += v->map[i] < G.c->blocks;
    }
    if (verbose) {
        fprintf(stderr, "k93iosnand: vCE %d (FMI%d CE%d): context block %u page %u (usn 0x%x; blocks %u %u %u %u), "
                "usable %u, pool from %u (%u per bank), %u block(s) replaced:", vce, vce_bus[vce], vce_ce[vce],
                v->blk, v->page, v->usn, cb[0], cb[1], cb[2], cb[3], v->usable, v->pool, v->nres, v->nrep);
        for (unsigned i = 0; i < v->nmap; i++)
            if (v->map[i] < G.c->blocks)
                fprintf(stderr, " %u->%u", v->map[i], conv(i / v->nres, v->pool + i % v->nres));
        fprintf(stderr, "\n");
    }
    return 0;
}

static int vfl_open(int verbose)
{
    unsigned usable = NONE;
    for (unsigned c = 0; c < NCE; c++) {
        if (vfl_load(c, verbose))
            return -1;
        if (V[c].usable < usable)
            usable = V[c].usable;
    }
    NSB = usable;
    return 0;
}

static uint32_t remap(int vce, uint32_t pblk)
{
    const struct vfl *v = &V[vce];
    for (unsigned i = 0; i < v->nmap; i++)
        if (v->map[i] == pblk)
            return conv(i / v->nres, v->pool + i % v->nres);
    return pblk;
}

/* vpn -> (virtual CE, row) */
static void vpn_loc(uint32_t vpn, int *vce, uint32_t *row)
{
    uint32_t vblock = vpn / PPSB, off = vpn % PPSB;
    uint32_t vbank = off % TB, vpage = off / TB;
    *vce = vbank % NCE;
    *row = row_of(&G, remap(*vce, conv(vbank / NCE, vblock)), vpage);
}

/* one virtual page: status 1 ok, 2 erased, < 0 unreadable; m = unwhitened metadata */
static int read_vpn(uint32_t vpn, uint8_t *data, uint8_t m[META_BYTES], uint32_t *rowp)
{
    int vce;
    uint32_t row;
    vpn_loc(vpn, &vce, &row);
    if (rowp)
        *rowp = row;
    return read_phys(vce, row, data, m);
}

/* ---- YAFTL's map, from the tables of contents ----------------------------
 * LPN -> vpn, in blocks of 1024 LPNs (8 MB of disk0), each one page, made when
 * its first LPN turns up: all of a 32 GB disk0 is 16 MB of map, while serving
 * System needs System's ~150 blocks, so serve() lets the others go. */

#define MB_SHIFT        10
#define MB_LPNS         (1u << MB_SHIFT)

static uint32_t **l2v;          /* NMB block pointers, NULL = nothing mapped there */
static uint32_t NLPN, NMB;
static uint32_t mapped, maxlpn, nmapblocks;

static uint32_t l2v_get(uint32_t lpn)
{
    const uint32_t *b = lpn < NLPN ? l2v[lpn >> MB_SHIFT] : NULL;
    return b ? b[lpn & (MB_LPNS - 1)] : NONE;
}

struct sbrec {
    uint32_t sb, usn;
    uint8_t type;
};

static int by_usn_desc(const void *a, const void *b)
{
    const struct sbrec *x = a, *y = b;
    return x->usn < y->usn ? 1 : x->usn > y->usn ? -1 : 0;
}

static void take(uint32_t lpn, uint32_t vpn)
{
    if (lpn >= NLPN)
        return;
    uint32_t **bp = &l2v[lpn >> MB_SHIFT];
    if (!*bp) {
        void *p = mmap(NULL, MB_LPNS * 4, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED) {
            fprintf(stderr, "k93iosnand: out of memory for the map\n");
            exit(1);
        }
        memset(p, 0xff, MB_LPNS * 4);
        *bp = p;
        nmapblocks++;
    }
    uint32_t *e = &(*bp)[lpn & (MB_LPNS - 1)];
    if (*e != NONE)
        return;
    *e = vpn;
    mapped++;
    if (lpn > maxlpn)
        maxlpn = lpn;
}

static int build_map(int verbose)
{
    static uint8_t dec[MAX_PAGE];
    uint8_t m[META_BYTES];
    struct sbrec *sbs = calloc(NSB, sizeof(*sbs));
    unsigned nuser = 0, nfree = 0, nother = 0, nbad = 0, nopen = 0;
    uint64_t t0 = now_us();

    NLPN = (uint32_t)((uint64_t)G.c->blocks * G.c->ppb * NCE);
    NMB = (NLPN + MB_LPNS - 1) >> MB_SHIFT;
    l2v = calloc(NMB, sizeof(*l2v));
    if (!sbs || !l2v) {
        fprintf(stderr, "k93iosnand: out of memory\n");
        return -1;
    }

    /* 1. classify: the first readable page of each superblock (one per bank) */
    for (uint32_t sb = 0; sb < NSB; sb++) {
        int st = -1;
        for (uint32_t off = 0; off < TB && st < 1; off++)
            st = read_vpn(sb * PPSB + off, pdata, m, NULL);
        if (st < 1) {
            nbad++;
            continue;
        }
        if (st == 2 || m[9] == 0xff) {
            nfree++;
            continue;
        }
        if (m[9] != T_USER) {
            nother++;
            continue;
        }
        sbs[nuser++] = (struct sbrec){ sb, le32(m + 4), m[9] };
    }
    if (verbose)
        fprintf(stderr, "k93iosnand: %u superblocks of %u pages: %u user, %u free, %u other, %u unreadable (%.1f s)\n",
                NSB, PPSB, nuser, nfree, nother, nbad, (now_us() - t0) / 1e6);

    /* 2. newest first; in each, its table of contents, later offsets first */
    qsort(sbs, nuser, sizeof(*sbs), by_usn_desc);
    for (unsigned i = 0; i < nuser; i++) {
        uint32_t sb = sbs[i].sb, row;
        int st = read_vpn(sb * PPSB + PPSB - 1, pdata, m, &row);
        if (st == 1 && m[9] == T_BTOC) {
            aesf_cbc(DK_META, row, pdata, dec, G.c->page);
            const uint8_t *t = dec;
            for (int off = (int)PPSB - 2; off >= 0; off--) {
                uint32_t lpn = le32(t + 4 * off);
                if (lpn != NONE)
                    take(lpn, sb * PPSB + off);
            }
            continue;
        }
        /* no table: the superblock still being filled -- page by page */
        nopen++;
        for (int off = (int)PPSB - 2; off >= 0; off--) {
            st = read_vpn(sb * PPSB + off, pdata, m, NULL);
            if (st == 1 && m[9] == T_USER)
                take(le32(m), sb * PPSB + off);
        }
    }
    free(sbs);
    if (verbose)
        fprintf(stderr, "k93iosnand: %u LPNs mapped (highest 0x%x, %u KB of map), %u superblock(s) read page by "
                "page (%.1f s)\n", mapped, maxlpn, nmapblocks * MB_LPNS * 4 / 1024, nopen, (now_us() - t0) / 1e6);
    return 0;
}

/* ---- disk0: one logical page, decrypted ---------------------------------- */

#define PCACHE 16
static struct { uint32_t lpn; uint8_t data[MAX_PAGE]; } pcache[PCACHE] __attribute__((aligned(8)));
static unsigned pcache_next;
static unsigned bad_reads;
static pthread_mutex_t pcache_mu = PTHREAD_MUTEX_INITIALIZER;

/* raw: a page of scratch for the read; one per thread (-j2) */
static int read_lpn_with(uint32_t lpn, uint8_t *out, uint8_t *raw)
{
    uint8_t m[META_BYTES];
    pthread_mutex_lock(&pcache_mu);
    for (unsigned i = 0; i < PCACHE; i++)
        if (pcache[i].lpn == lpn && lpn != NONE) {
            memcpy(out, pcache[i].data, G.c->page);
            pthread_mutex_unlock(&pcache_mu);
            return 0;
        }
    pthread_mutex_unlock(&pcache_mu);
    uint32_t vpn = l2v_get(lpn);
    if (vpn == NONE) {                          /* never written (or trimmed): zeroes */
        memset(out, 0, G.c->page);
        return 0;
    }
    int st = read_vpn(vpn, raw, m, NULL);
    if (st != 1 || m[9] != T_USER || le32(m) != lpn) {
        if (bad_reads++ < 20)
            fprintf(stderr, "k93iosnand: LPN 0x%x at vpn 0x%x: status %d type 0x%02x lpn 0x%x -- EIO\n",
                    lpn, vpn, st, m[9], le32(m));
        return -1;
    }
    aesf_cbc(DK_USER, lpn, raw, out, G.c->page);
    pthread_mutex_lock(&pcache_mu);
    unsigned k = pcache_next++ % PCACHE;
    pcache[k].lpn = lpn;
    memcpy(pcache[k].data, out, G.c->page);
    pthread_mutex_unlock(&pcache_mu);
    return 0;
}

static int read_lpn(uint32_t lpn, uint8_t *out)
{
    return read_lpn_with(lpn, out, pdata);
}

/* ---- LwVM (section 9: the mini's format, chunk = 2^n >= media / 1024) ----- */

#define LWVM_HDR        0xf000
#define LWVM_FREE       0xf3ff

struct part {
    char name[40];
    uint64_t size;
    uint32_t nchunks;
    uint16_t phys[1024];
};
static struct part parts[12];
static int nparts;
static uint64_t lwvm_chunk;

static int lwvm_load(void)
{
    static uint8_t h[MAX_PAGE];
    static const uint8_t magic[2][16] = {
        { 0x6a, 0x90, 0x88, 0xcf, 0x8a, 0xfd, 0x63, 0x0a, 0xe3, 0x51, 0xe2, 0x48, 0x87, 0xe0, 0xb9, 0x8b },
        { 0xb1, 0x89, 0xa5, 0x19, 0x4f, 0x59, 0x4b, 0x1d, 0xad, 0x44, 0x1e, 0x12, 0x7a, 0xaf, 0x45, 0x39 },
    };
    if (read_lpn(0, h) || (memcmp(h, magic[0], 16) && memcmp(h, magic[1], 16))) {
        fprintf(stderr, "k93iosnand: no LwVM header at LPN 0:");
        for (int i = 0; i < 16; i++)
            fprintf(stderr, " %02x", h[i]);
        fprintf(stderr, "\n");
        return -1;
    }
    uint64_t media;
    uint32_t n;
    memcpy(&media, h + 0x20, 8);
    memcpy(&n, h + 0x28, 4);
    for (lwvm_chunk = 1 << 20; lwvm_chunk * 1024 < media; lwvm_chunk <<= 1)
        ;
    const uint8_t *map = h + 0x800;
    for (uint32_t i = 0; i < n && i < 12; i++) {
        struct part *p = &parts[i];
        const uint8_t *r = h + 0x200 + i * 0x80;
        uint64_t begin, end;
        memcpy(&begin, r + 32, 8);
        memcpy(&end, r + 40, 8);
        for (int c = 0; c < 36; c++)            /* UTF-16LE, ASCII in practice */
            p->name[c] = r[56 + 2 * c];
        p->name[36] = 0;
        p->nchunks = 0;
        for (int c = 0; c < 1024; c++) {
            uint16_t v = map[2 * c] | map[2 * c + 1] << 8;
            if (v == LWVM_HDR || v == LWVM_FREE || v >> 12 != i)
                continue;
            if ((v & 0xfff) < 1024)
                p->phys[v & 0xfff] = c;
            if ((v & 0xfff) + 1u > p->nchunks)
                p->nchunks = (v & 0xfff) + 1;
        }
        p->size = end - begin;
        if (p->size > p->nchunks * lwvm_chunk)
            p->size = p->nchunks * lwvm_chunk;
    }
    nparts = n < 12 ? n : 12;
    return 0;
}

static struct part *part_by_name(const char *name)
{
    for (int i = 0; i < nparts; i++)
        if (!strcmp(parts[i].name, name))
            return &parts[i];
    return NULL;
}

/* ---- both buses at once (-j2) --------------------------------------------
 * FMI0 and FMI1 are two controllers, and a superblock stripes its pages over
 * all four CEs, two on each bus, so a request's pages alternate between them:
 * the pages behind FMI1 go to a second thread while this one reads FMI0's.
 * k93nand.c's read path touches only its own bus's registers and its
 * arguments (its globals are set once, at start); each thread has its own raw
 * page buffer, and the page cache is shared under a mutex.  `-j2`: the
 * init's NAND root and `ios` run with it.  Checked on the iPad (2026-11-09):
 * `k93iosnand -j2 bench System 1244` -- all of System read one bus at a time
 * (7.2 MB/s) and both at once (12.2 MB/s), the same bytes. */

struct pjob {
    uint32_t lpn, in, n;        /* n bytes from offset in of the page */
    uint8_t *dst;
};
#define PJOBS           160     /* > the pages of one capped request (1 MB + 1) */

static int parallel;
static pthread_mutex_t w_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t w_go = PTHREAD_COND_INITIALIZER, w_done = PTHREAD_COND_INITIALIZER;
static struct pjob wjobs[PJOBS];
static int wn, wbusy, werr;

static int run_job(const struct pjob *j, uint8_t *raw, uint8_t *page)
{
    if (j->in == 0 && j->n == G.c->page)
        return read_lpn_with(j->lpn, j->dst, raw);
    if (read_lpn_with(j->lpn, page, raw))
        return -1;
    memcpy(j->dst, page + j->in, j->n);
    return 0;
}

static void *worker_main(void *arg)
{
    static uint8_t raw[MAX_PAGE] __attribute__((aligned(8))), page[MAX_PAGE] __attribute__((aligned(8)));
    (void)arg;
    pthread_mutex_lock(&w_mu);
    for (;;) {
        while (!wbusy)
            pthread_cond_wait(&w_go, &w_mu);
        int n = wn, err = 0;
        pthread_mutex_unlock(&w_mu);
        for (int i = 0; i < n; i++)
            err |= run_job(&wjobs[i], raw, page);
        pthread_mutex_lock(&w_mu);
        werr = err;
        wbusy = 0;
        pthread_cond_signal(&w_done);
    }
    return NULL;
}

static int start_worker(void)
{
    pthread_t t;
    if (pthread_create(&t, NULL, worker_main, NULL)) {
        fprintf(stderr, "k93iosnand: no second thread: one bus at a time\n");
        parallel = 0;
        return -1;
    }
    pthread_detach(t);
    return 0;
}

static int lpn_bus(uint32_t lpn)
{
    uint32_t vpn = l2v_get(lpn);
    if (vpn == NONE)
        return 0;
    return vce_bus[(vpn % PPSB % TB) % NCE];
}

/* len bytes of a partition from off, any alignment */
static int read_part(struct part *p, uint64_t off, uint8_t *out, uint32_t len)
{
    static uint8_t page[MAX_PAGE] __attribute__((aligned(8)));
    static struct pjob mine[PJOBS];
    uint32_t pg = G.c->page;
    int err = 0;
    while (len && !err) {
        int nm = 0, nw = 0;
        /* a window of pages: each to the thread of its bus */
        while (len && nm < PJOBS && nw < PJOBS) {
            uint64_t c = off / lwvm_chunk;
            if (c >= p->nchunks)
                return -1;
            uint64_t d0 = p->phys[c] * lwvm_chunk + off % lwvm_chunk;
            struct pjob j = { (uint32_t)(d0 / pg), (uint32_t)(d0 % pg), 0, out };
            j.n = pg - j.in < len ? pg - j.in : len;
            if (parallel && lpn_bus(j.lpn) == 1)
                wjobs[nw++] = j;
            else
                mine[nm++] = j;
            out += j.n;
            off += j.n;
            len -= j.n;
        }
        if (nw) {
            pthread_mutex_lock(&w_mu);
            wn = nw;
            wbusy = 1;
            pthread_cond_signal(&w_go);
            pthread_mutex_unlock(&w_mu);
        }
        for (int i = 0; i < nm; i++)
            err |= run_job(&mine[i], pdata, page);
        if (nw) {
            pthread_mutex_lock(&w_mu);
            while (wbusy)
                pthread_cond_wait(&w_done, &w_mu);
            err |= werr;
            pthread_mutex_unlock(&w_mu);
        }
    }
    return err ? -1 : 0;
}

/* the same stretch of a partition read one bus at a time and both at once:
 * the two must hash the same, and say how fast each was */
static int bench(const char *name, uint32_t mb)
{
    static uint8_t buf[1 << 20];
    struct part *p = part_by_name(name);
    if (!p) {
        fprintf(stderr, "k93iosnand: no LwVM partition %s\n", name);
        return 1;
    }
    if ((uint64_t)mb << 20 > p->size)
        mb = p->size >> 20;
    uint64_t h[2];
    double secs[2];
    for (int pass = 0; pass < 2; pass++) {
        parallel = pass;
        if (pass && start_worker())
            return 1;
        for (unsigned i = 0; i < PCACHE; i++)
            pcache[i].lpn = NONE;
        uint64_t t0 = now_us(), x = 0xcbf29ce484222325ull;
        for (uint32_t i = 0; i < mb; i++) {
            if (read_part(p, (uint64_t)i << 20, buf, sizeof(buf)))
                return 1;
            for (size_t k = 0; k < sizeof(buf); k++)
                x = (x ^ buf[k]) * 0x100000001b3ull;
        }
        secs[pass] = (now_us() - t0) / 1e6;
        h[pass] = x;
        printf("%s: %u MB of %s in %.1f s, %.1f MB/s, hash %016llx\n", pass ? "both buses" : "one bus  ",
               mb, name, secs[pass], mb / secs[pass], (unsigned long long)x);
    }
    printf("%s\n", h[0] == h[1] ? "SAME bytes" : "DIFFERENT -- -j2 is not safe");
    return h[0] == h[1] ? 0 : 2;
}

/* ---- NBD (as tools/nand/iosnand.c) --------------------------------------- */

static volatile sig_atomic_t stop;
static void on_signal(int s) { (void)s; stop = 1; }

static int read_full(int fd, void *buf, size_t n)
{
    for (size_t got = 0; got < n; ) {
        ssize_t r = read(fd, (uint8_t *)buf + got, n - got);
        if (r <= 0)
            return -1;
        got += r;
    }
    return 0;
}

static int write_full(int fd, const void *buf, size_t n)
{
    for (size_t put = 0; put < n; ) {
        ssize_t r = write(fd, (const uint8_t *)buf + put, n - put);
        if (r <= 0)
            return -1;
        put += r;
    }
    return 0;
}

/* An export is an LwVM partition (read-only, read_part) or, with ops, a volume
 * someone else implements -- tools/nand/k93lv.c's Linux volume, read-write. */
struct export_ops {
    int (*read)(uint64_t off, uint8_t *buf, uint32_t len);
    int (*write)(uint64_t off, const uint8_t *buf, uint32_t len);
    int (*flush)(void);
    int (*trim)(uint64_t off, uint32_t len);
    void (*idle)(void);                         /* the queue was quiet for a second */
};

struct export {
    struct part *p;
    const struct export_ops *ops;
    uint64_t size;
    const char *name;
    int nbd, sock;
    pid_t child;
    char dev[24];
};

static int attach(struct export *e, int idx)
{
    int sv[2];
    snprintf(e->dev, sizeof(e->dev), "/dev/nbd%d", idx);
    e->nbd = open(e->dev, O_RDWR);
    if (e->nbd < 0) {
        perror(e->dev);
        return -1;
    }
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv)) {
        perror("socketpair");
        return -1;
    }
    ioctl(e->nbd, NBD_CLEAR_SOCK);
    if (e->p) {
        e->size = e->p->size;
        e->name = e->p->name;
    }
    unsigned long flags = NBD_FLAG_HAS_FLAGS |
                          (e->ops && e->ops->write ? NBD_FLAG_SEND_FLUSH | (e->ops->trim ? NBD_FLAG_SEND_TRIM : 0)
                                                   : NBD_FLAG_READ_ONLY);
    if (ioctl(e->nbd, NBD_SET_BLKSIZE, 4096UL) ||
        ioctl(e->nbd, NBD_SET_SIZE_BLOCKS, (unsigned long)(e->size / 4096)) ||
        ioctl(e->nbd, NBD_SET_FLAGS, flags) ||
        ioctl(e->nbd, NBD_SET_SOCK, sv[0])) {
        perror("nbd ioctl");
        return -1;
    }
    e->child = fork();
    if (e->child == 0) {
        /* only its device: an inherited server end of an earlier export's
         * socket would keep that socket open after the server closed it, and
         * that export's NBD_DO_IT (and the server's waitpid) would never return */
        for (int fd = 3; fd < 1024; fd++)
            if (fd != e->nbd)
                close(fd);
        ioctl(e->nbd, NBD_DO_IT);
        ioctl(e->nbd, NBD_CLEAR_QUE);
        ioctl(e->nbd, NBD_CLEAR_SOCK);
        _exit(0);
    }
    close(sv[0]);
    e->sock = sv[1];
    fprintf(stderr, "k93iosnand: %s = %s, %llu MB, %s\n", e->dev, e->name, (unsigned long long)(e->size >> 20),
            e->ops && e->ops->write ? "read-write" : "read-only");
    return 0;
}

#define SERVE_BUF       (1 << 20)

static int serve_one(struct export *e)
{
    static uint8_t sbuf[SERVE_BUF];
    uint8_t *buf = sbuf;
    struct nbd_request rq;
    struct nbd_reply rp = { htonl(NBD_REPLY_MAGIC), 0, { 0 } };
    if (read_full(e->sock, &rq, sizeof(rq)) || ntohl(rq.magic) != NBD_REQUEST_MAGIC)
        return -1;
    memcpy(rp.handle, rq.handle, sizeof(rp.handle));
    uint32_t type = ntohl(rq.type) & 0xffff, len = ntohl(rq.len);
    uint64_t from = be64toh(rq.from);
    switch (type) {
    case NBD_CMD_READ: {
        int r = 0;
        if (len > sizeof(sbuf) && !(buf = malloc(len)))
            rp.error = htonl(ENOMEM);
        else if (from + len > e->size || (e->ops ? e->ops->read(from, buf, len) : read_part(e->p, from, buf, len)))
            rp.error = htonl(EIO);
        if (write_full(e->sock, &rp, sizeof(rp)) || (!rp.error && write_full(e->sock, buf, len)))
            r = -1;
        if (buf != sbuf)
            free(buf);
        return r;
    }
    case NBD_CMD_DISC:
        return -1;
    case NBD_CMD_WRITE: {
        int ok = e->ops && e->ops->write && len <= sizeof(sbuf) && from + len <= e->size;
        for (uint32_t left = len, at = 0; left; ) {   /* refused: swallow the data */
            uint32_t n = left < sizeof(sbuf) ? left : sizeof(sbuf);
            if (read_full(e->sock, buf + (ok ? at : 0), n))
                return -1;
            left -= n;
            at += n;
        }
        if (!ok)
            rp.error = htonl(e->ops && e->ops->write ? EINVAL : EPERM);
        else if (e->ops->write(from, buf, len))
            rp.error = htonl(EIO);
        break;
    }
    case NBD_CMD_FLUSH:
        if (e->ops && e->ops->flush && e->ops->flush())
            rp.error = htonl(EIO);
        break;
    case NBD_CMD_TRIM:
        if (!e->ops || !e->ops->trim || from + len > e->size)
            rp.error = htonl(EPERM);
        else if (e->ops->trim(from, len))
            rp.error = htonl(EIO);
        break;
    default:
        rp.error = htonl(EPERM);
    }
    return write_full(e->sock, &rp, sizeof(rp));
}

/* keep the map blocks that LwVM's header and the served partitions need */
static void map_trim(struct export *ex, int nex)
{
    uint8_t want[1024] = { 1 };                 /* chunk 0: the LwVM header */
    for (int i = 0; i < nex; i++)
        for (uint32_t k = 0; ex[i].p && k < ex[i].p->nchunks; k++)
            if (ex[i].p->phys[k] < 1024)
                want[ex[i].p->phys[k]] = 1;
    uint64_t span = (uint64_t)MB_LPNS * G.c->page;
    unsigned kept = 0;
    for (uint32_t b = 0; b < NMB; b++) {
        if (!l2v[b])
            continue;
        uint64_t c0 = b * span / lwvm_chunk, c1 = ((b + 1) * span - 1) / lwvm_chunk;
        int keep = 0;
        for (uint64_t c = c0; c <= c1 && c < 1024; c++)
            keep |= want[c];
        if (keep) {
            kept++;
            continue;
        }
        munmap(l2v[b], MB_LPNS * 4);
        l2v[b] = NULL;
    }
    fprintf(stderr, "k93iosnand: map trimmed to what is served: %u KB\n", kept * MB_LPNS * 4 / 1024);
}

/* requests no bigger than serve_one's buffer: no allocation while the kernel
 * waits on a page-in (this can be the root's own disk) */
static void cap_requests(int idx)
{
    char path[64];
    snprintf(path, sizeof(path), "/sys/block/nbd%d/queue/max_sectors_kb", idx);
    FILE *f = fopen(path, "w");
    if (f) {
        fprintf(f, "%d\n", SERVE_BUF >> 10);
        fclose(f);
    }
}

static int serve_exports(struct export *ex, int nex);

static int serve(char **names, int n)
{
    static char *deflt[] = { "System" };
    static struct export ex[12];
    int nex = 0;
    if (!n) {
        names = deflt;
        n = 1;
    }
    for (int i = 0; i < n && nex < 12; i++) {
        struct part *p = part_by_name(names[i]);
        if (!p) {
            fprintf(stderr, "k93iosnand: no LwVM partition %s\n", names[i]);
            continue;
        }
        memset(&ex[nex], 0, sizeof(ex[nex]));
        ex[nex++].p = p;
    }
    if (!nex)
        return 1;
    return serve_exports(ex, nex);
}

/* ex[i].p (LwVM partitions) or ex[i].ops + size + name; /dev/nbd<i> each */
static int serve_exports(struct export *ex, int nex)
{
    map_trim(ex, nex);
    for (int i = 0; i < nex; i++) {
        cap_requests(i);
        if (attach(&ex[i], i))
            return 1;
    }
    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);
    signal(SIGPIPE, SIG_IGN);
    struct pollfd pf[12];
    while (!stop) {
        for (int i = 0; i < nex; i++)
            pf[i] = (struct pollfd){ ex[i].sock, POLLIN, 0 };
        int nr = poll(pf, nex, 1000);
        if (nr < 0)
            continue;
        if (nr == 0)
            for (int i = 0; i < nex; i++)
                if (ex[i].ops && ex[i].ops->idle && ex[i].sock >= 0)
                    ex[i].ops->idle();
        for (int i = 0; i < nex; i++)
            if (pf[i].revents && ex[i].sock >= 0 && serve_one(&ex[i])) {
                close(ex[i].sock);
                ex[i].sock = -1;
            }
        int alive = 0;
        for (int i = 0; i < nex; i++)
            alive += ex[i].sock >= 0;
        if (!alive)
            break;
    }
    for (int i = 0; i < nex; i++) {           /* every export down first, then wait */
        if (ex[i].ops && ex[i].ops->flush)
            ex[i].ops->flush();
        ioctl(ex[i].nbd, NBD_DISCONNECT);
        if (ex[i].sock >= 0)
            close(ex[i].sock);
    }
    for (int i = 0; i < nex; i++)
        waitpid(ex[i].child, NULL, 0);
    fprintf(stderr, "k93iosnand: stopped\n");
    return 0;
}

/* ---- main ---------------------------------------------------------------- */

/* K93IOSNAND_LIB: included by tools/nand/k93nandw.c for the reader above */
#ifndef K93IOSNAND_LIB
int main(int argc, char **argv)
{
    if (argc > 2 && !strcmp(argv[1], "-j2")) {
        parallel = 1;
        argv++;
        argc--;
    }
    if (argc < 2) {
        fprintf(stderr, "usage: k93iosnand [-j2] vfl | map | parts | read LPN [N] | bench PARTITION [MB] |\n"
                "                  serve [PARTITION...]\n");
        return 1;
    }
    trace = 0;
    setvbuf(stdout, NULL, _IOLBF, 0);
    whiten_init();
    aes_init();
    aesf_init();
    if (aesf_check())
        return 1;
    open_hw();
    if (load_state() || vsvfl_setup())
        return 1;
    for (int b = 0; b < NBUS; b++)
        if ((cemap >> (b * 8)) & 0xff && !claim(b))
            return 1;
    for (unsigned i = 0; i < PCACHE; i++)
        pcache[i].lpn = NONE;
    if (vfl_open(!strcmp(argv[1], "vfl")))
        return 1;
    fprintf(stderr, "k93iosnand: %u CEs x %u banks, %u usable superblocks of %u pages (%u MB)\n", NCE, BPC, NSB,
            PPSB, PPSB * (G.c->page >> 10) >> 10);
    if (!strcmp(argv[1], "vfl"))
        return 0;
    if (build_map(1))
        return 1;
    if (!strcmp(argv[1], "map"))
        return 0;
    if (lwvm_load())
        return 1;
    if (!strcmp(argv[1], "parts")) {
        printf("LwVM: %d partition(s), chunks of %llu MB\n", nparts, (unsigned long long)(lwvm_chunk >> 20));
        for (int i = 0; i < nparts; i++)
            printf("%-10s %6llu MB  %u chunks\n", parts[i].name, (unsigned long long)(parts[i].size >> 20),
                   parts[i].nchunks);
        return 0;
    }
    if (!strcmp(argv[1], "read") && argc >= 3) {
        static uint8_t b[MAX_PAGE];
        uint32_t lpn = strtoul(argv[2], NULL, 0), n = argc > 3 ? strtoul(argv[3], NULL, 0) : 1;
        for (uint32_t i = 0; i < n; i++)
            if (read_lpn(lpn + i, b) || write_full(1, b, G.c->page))
                return 1;
        return 0;
    }
    if (!strcmp(argv[1], "bench") && argc >= 3)
        return bench(argv[2], argc > 3 ? strtoul(argv[3], NULL, 0) : 64);
    if (!strcmp(argv[1], "serve")) {
        if (parallel)
            start_worker();
        return serve(argv + 2, argc - 2);
    }
    fprintf(stderr, "k93iosnand: unknown command %s\n", argv[1]);
    return 1;
}
#endif
