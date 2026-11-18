/* k93yaftl -- iOS's own FTL (YAFTL) on the iPad 2's NAND, from Linux: what it
 * has on the flash, and (to come) writing into it the way iOS does, so that
 * Linux can use LwVM partitions of its own (tools/k93-part.sh) and write to
 * iOS's System.  docs/research/k93-nand.md 8 is the read side; the write
 * side's reverse engineering is build/k93/research-write/yaftl-write-re.md.
 *
 *   k93yaftl pages SB [FROM [N [BYTES]]]
 *                              read only: SB's written pages from FROM, their
 *                              metadata and first BYTES bytes decrypted
 *   k93yaftl [-j2] serve System:rw Linux:rw Swap:rw
 *                              the NAND server initramfs/init runs: /dev/nbd0..
 *                              = those partitions (":rw" through iOS's FTL, only
 *                              for System, Linux and Swap: never Data), then
 *                              k93lv's Linux volume;
 *                              below 48 free superblocks it gives space back
 *                              (the GC; `echo N > /run/ios/gc` asks for N now)
 *   k93yaftl ctl               read only: the newest context set, and whether
 *                              iOS's "stale" marker follows it
 *   k93yaftl write-test START SEED [--dry] [--sb N]
 *                              WRITES: the marker (if missing), then one free
 *                              superblock erased and filled with LPNs START..
 *                              START+2046 (a pattern from SEED) and its table
 *                              of contents, every page read back
 *   k93yaftl check START SEED  read only: those LPNs through the reader's map
 *   k93yaftl write-lpns START FILE [--expect OLD] [--dry]
 *                              WRITES FILE's 8 KB pages to LPNs START.. (the
 *                              marker first if needed); --expect: LPN START
 *                              must read as OLD's first page now
 *   k93yaftl lwvm-apply NEW OLD [--dry]
 *                              WRITES the LwVM table NEW to LPN 0 and its backup
 *                              LPN 4095 -- only if LPN 0 holds OLD and NEW passes
 *                              iOS's own check (tools/k93-part.sh plans it)
 *   k93yaftl inspect [SB...]   read only: YAFTL's control superblocks (their
 *                              context pages), the index superblocks, the
 *                              newest user superblocks (per-page metadata,
 *                              the table of contents decrypted), and SB...
 *
 * Needs `k93nand id` this boot and the buses free (a RAM root: no k93lv).
 *
 * Build (from devices/a5, in the cascadia-build image):
 *   arm-linux-gnueabihf-gcc -static -O2 -Wall -Wno-unused-function -pthread \
 *       -D_FILE_OFFSET_BITS=64 -o build/k93/tools/k93yaftl tools/nand/k93yaftl.c
 */
#define K93LV_LIB
#include "k93lv.c"
#include <sys/mman.h>
#include <sys/prctl.h>

#define T_INDEX         0x04
#define T_CTX           0x20

static void hexline(const char *tag, const uint8_t *p, unsigned n)
{
    printf("%s", tag);
    for (unsigned i = 0; i < n; i++)
        printf("%s%02x", i && !(i % 4) ? " " : "", p[i]);
    printf("  |");
    for (unsigned i = 0; i < n; i++)
        putchar(p[i] >= 32 && p[i] < 127 ? p[i] : '.');
    printf("|\n");
}

static void meta_line(uint32_t vpn, int st, const uint8_t *m)
{
    if (st == 2)
        printf("    vpn %8u  erased\n", vpn);
    else if (st < 1)
        printf("    vpn %8u  unreadable (%d)\n", vpn, st);
    else
        printf("    vpn %8u  lpn %08x  usn %08x  b8 %02x  type %02x\n", vpn, le32(m), le32(m + 4), m[8], m[9]);
}

/* every page of a superblock: runs of the same type, the context pages shown */
static void control_sb(uint32_t sb)
{
    static uint8_t dec[MAX_PAGE];
    uint8_t m[META_BYTES];
    uint32_t row;
    int last_type = -1, run = 0;
    uint32_t run_from = 0, written = 0, lastctx = NONE;
    printf("  control superblock %u:\n", sb);
    for (uint32_t off = 0; off <= PPSB; off++) {
        int st = off < PPSB ? read_vpn(sb * PPSB + off, pdata, m, &row) : -9;
        int ty = st == 1 ? m[9] : st == 2 ? 0x100 : st == -9 ? 0x200 : 0x300;
        if (ty != last_type) {
            if (run)
                printf("    off %4u-%4u  %s%s\n", run_from, off - 1,
                       last_type == 0x100 ? "erased" : last_type == 0x300 ? "unreadable" : "type ",
                       last_type < 0x100 ? (char[8]){ "0123456789abcdef"[last_type >> 4],
                                                      "0123456789abcdef"[last_type & 15], 0 } : "");
            last_type = ty;
            run_from = off;
            run = 1;
        }
        if (st == 1) {
            written++;
            if (m[9] == T_CTX)
                lastctx = off;
        }
    }
    printf("    %u pages written\n", written);
    if (lastctx != NONE) {
        uint32_t vpn = sb * PPSB + lastctx;
        int st = read_vpn(vpn, pdata, m, &row);
        meta_line(vpn, st, m);
        aesf_cbc(DK_META, row, pdata, dec, G.c->page);
        for (unsigned o = 0; o < 0x100; o += 32) {
            char tag[32];
            snprintf(tag, sizeof tag, "      ctx +%03x  ", o);
            hexline(tag, dec + o, 32);
        }
    }
}

static void user_sb(uint32_t sb)
{
    static uint8_t dec[MAX_PAGE];
    uint8_t m[META_BYTES];
    uint32_t row;
    printf("  superblock %u:\n", sb);
    for (uint32_t off = 0; off < PPSB; off++) {
        if (off == 8 && PPSB > 24)
            off = PPSB - 8, printf("    ...\n");
        int st = read_vpn(sb * PPSB + off, pdata, m, &row);
        meta_line(sb * PPSB + off, st, m);
    }
    int st = read_vpn(sb * PPSB + PPSB - 1, pdata, m, &row);
    if (st != 1 || (m[9] & ~4) != T_BTOC) {
        printf("    no table of contents (last page: status %d type %02x)\n", st, st == 1 ? m[9] : 0xff);
        return;
    }
    aesf_cbc(DK_META, row, pdata, dec, G.c->page);
    unsigned listed = 0, ff = 0, mism = 0, checked = 0;
    for (uint32_t off = 0; off + 1 < PPSB; off++) {
        uint32_t l = le32(dec + 4 * off);
        if (l == NONE)
            ff++;
        else
            listed++;
    }
    /* the table against the pages' own metadata, for the first 64 offsets */
    for (uint32_t off = 0; off < 64 && off + 1 < PPSB; off++) {
        uint8_t mm[META_BYTES];
        if (read_vpn(sb * PPSB + off, pdata, mm, NULL) == 1) {
            checked++;
            mism += le32(mm) != le32(dec + 4 * off);
        }
    }
    printf("    table of contents: %u LPNs listed, %u FFFFFFFF; vs metadata of the first %u pages: %u differ\n",
           listed, ff, checked, mism);
    hexline("      toc +0000  ", dec, 32);
    unsigned tail = 4 * (PPSB - 1);
    printf("      bytes %u..%u after the list:", tail, G.c->page - 1);
    for (unsigned i = tail; i < G.c->page && i < tail + 64; i++)
        printf(" %02x", dec[i]);
    printf("\n");
}

static int cmd_inspect(char **argv, int argc)
{
    uint8_t m[META_BYTES];
    if (vfl_open(0))
        return 1;
    printf("YAFTL: %u superblocks of %u pages (%u CEs x %u banks); control superblocks %u %u %u (vCE0's context;"
           " the others:", NSB, PPSB, NCE, BPC, V[0].ctrl[0], V[0].ctrl[1], V[0].ctrl[2]);
    for (unsigned c = 1; c < NCE; c++)
        printf(" %u %u %u", V[c].ctrl[0], V[c].ctrl[1], V[c].ctrl[2]);
    printf(")\n");
    uint32_t umax = 0, umaxsb = NONE, nu = 0, nf = 0, top[3] = { NONE, NONE, NONE }, topu[3] = { 0, 0, 0 };
    for (uint32_t sb = 0; sb < NSB; sb++) {
        int st = -1;
        for (uint32_t off = 0; off < TB && st < 1; off++)
            st = read_vpn(sb * PPSB + off, pdata, m, NULL);
        if (st == 2 || (st == 1 && m[9] == 0xff)) {
            nf++;
            continue;
        }
        if (st != 1) {
            printf("  sb %u: first pages unreadable\n", sb);
            continue;
        }
        uint32_t u = le32(m + 4);
        if (m[9] == T_USER) {
            nu++;
            if (u > umax)
                umax = u, umaxsb = sb;
            for (int k = 0; k < 3; k++)
                if (top[k] == NONE || u > topu[k]) {
                    for (int j = 2; j > k; j--)
                        top[j] = top[j - 1], topu[j] = topu[j - 1];
                    top[k] = sb, topu[k] = u;
                    break;
                }
        } else {
            printf("  sb %4u: first page type %02x lpn %08x usn %08x b8 %02x%s\n", sb, m[9], le32(m), u, m[8],
                   sb == V[0].ctrl[0] || sb == V[0].ctrl[1] || sb == V[0].ctrl[2] ? "  (control)" : "");
        }
    }
    printf("user superblocks %u (newest usn %08x in sb %u), free %u\n", nu, umax, umaxsb, nf);
    for (int i = 0; i < 3; i++)
        if (V[0].ctrl[i] < NSB)
            control_sb(V[0].ctrl[i]);
    for (int k = 0; k < 3; k++)
        if (top[k] != NONE)
            user_sb(top[k]);
    for (int i = 0; i < argc; i++)
        user_sb((uint32_t)strtoul(argv[i], NULL, 0));
    return 0;
}

/* ======================================================================================
 * The write side.  Everything below follows what iOS's own pages on this flash show
 * (logs/k93-yaftl-inspect-1.log, logs/k93-yaftl-ctx1038.log, 2026-11-15):
 *  - user page: data AES-128-CBC with the user key, IV from the LPN; metadata {LPN,
 *    weaveSeq, 0, 0x10}; weaveSeq is a global counter that only grows (one value per
 *    write batch), a superblock's pages in vpn order;
 *  - table of contents, the superblock's last page: the 2047 LPNs then ff ff ff ff,
 *    META key, IV = row, metadata {0, 0, 0, 0x08};
 *  - context: sets of pages ("CX01" header first, +0x2c = pages in the set, +0x10 =
 *    the weaveSeq W every page of the set carries) appended to a control superblock;
 *    when iOS writes again after saving one, it first appends ONE page with weaveSeq
 *    W + 1, a copy of the set's page 2 -- a set with nothing after it is the one a
 *    mount may trust.  So before Linux writes, it appends that page exactly so.
 * ====================================================================================== */

static uint32_t EK_USER[44];
static void aese_cbc(const uint32_t *ek, uint32_t seed, const uint8_t *in, uint8_t *out, size_t len)
{
    uint8_t iv[16], b[16];
    iv_for(seed, iv);
    for (size_t o = 0; o + 16 <= len; o += 16) {
        for (int i = 0; i < 16; i++)
            b[i] = in[o + i] ^ iv[i];
        aese_block(ek, b, out + o);
        memcpy(iv, out + o, 16);
    }
}

static void enc_init(void)
{
    uint8_t rk[176];
    aese_init();
    aes_expand(KEY_USER, rk);
    for (int i = 0; i < 44; i++)
        EK_USER[i] = GETU32(rk + 4 * i);
}

/* vpn -> virtual CE, physical block, row (vpn_loc's arithmetic, with the block kept) */
static void vpn_where(uint32_t vpn, int *vce, uint32_t *pblk, uint32_t *row, int *remapped)
{
    uint32_t vblock = vpn / PPSB, off = vpn % PPSB;
    uint32_t vbank = off % TB, vpage = off / TB;
    *vce = vbank % NCE;
    uint32_t b0 = conv(vbank / NCE, vblock);
    *pblk = remap(*vce, b0);
    *remapped = *pblk != b0;
    *row = row_of(&G, *pblk, vpage);
}

static int is_ctrl(uint32_t sb)
{
    for (unsigned c = 0; c < NCE; c++)
        for (int i = 0; i < 3; i++)
            if (V[c].ctrl[i] == sb)
                return 1;
    return 0;
}

/* the control state: the newest context set, whether a page follows it, the next free page */
struct ctl {
    uint32_t sb, set_off, set_n, W, next, page2_off;
    int marked;
};

static int ctl_find(struct ctl *C, int verbose)
{
    static uint8_t dec[MAX_PAGE];
    uint8_t m[META_BYTES];
    uint32_t row;
    memset(C, 0, sizeof(*C));
    C->sb = NONE;
    for (unsigned c = 0; c < NCE; c++)
        for (int i = 0; i < 3; i++) {
            uint32_t sb = V[c].ctrl[i], seen = 0;
            for (unsigned c2 = 0; c2 < c * 3 + i; c2++)
                seen |= V[c2 / 3].ctrl[c2 % 3] == sb;
            if (sb >= NSB || seen)
                continue;
            uint32_t next = PPSB;
            for (uint32_t off = 0; off < PPSB; off++) {
                int st = read_vpn(sb * PPSB + off, pdata, m, &row);
                if (st == 2) {
                    next = off;
                    break;
                }
                if (st != 1 || m[9] != T_CTX)
                    continue;
                aesf_cbc(DK_META, row, pdata, dec, 64);
                if (memcmp(dec, "CX01", 4))
                    continue;
                uint32_t W = le32(dec + 0x10), n = le32(dec + 0x2c);
                if (le32(m + 4) == W && (C->sb == NONE || W > C->W)) {
                    C->sb = sb;
                    C->set_off = off;
                    C->set_n = n;
                    C->W = W;
                }
            }
            if (C->sb == sb)
                C->next = next;
            if (verbose)
                printf("  control superblock %u: %u pages written\n", sb, next);
        }
    if (C->sb == NONE) {
        fprintf(stderr, "k93yaftl: no context set (CX01) in any control superblock\n");
        return -1;
    }
    /* the set must be whole: n pages with weaveSeq W, page 2 of it the marker's model */
    for (uint32_t k = 0; k < C->set_n; k++) {
        int st = read_vpn(C->sb * PPSB + C->set_off + k, pdata, m, NULL);
        if (st != 1 || m[9] != T_CTX || le32(m + 4) != C->W) {
            fprintf(stderr, "k93yaftl: the newest set (sb %u from %u, %u pages) is not whole at page %u\n", C->sb,
                    C->set_off, C->set_n, C->set_off + k);
            return -1;
        }
    }
    C->page2_off = C->set_off + 2;
    /* iOS's slot scan (yaftl-write-re.md 1.2): slots every n + 1 pages from 0, the first one whose
     * marker page (slot + n) is erased is used.  So every earlier slot's marker must be programmed. */
    if (C->set_n != 10 || C->set_off % (C->set_n + 1)) {
        fprintf(stderr, "k93yaftl: the newest set is at page %u with %u pages: not the 11-page slots this tool knows\n",
                C->set_off, C->set_n);
        return -1;
    }
    for (uint32_t s = 0; s < C->set_off; s += C->set_n + 1) {
        int st = read_vpn(C->sb * PPSB + s + C->set_n, pdata, m, NULL);
        if (st == 2) {
            fprintf(stderr, "k93yaftl: an older slot (page %u) has no marker: iOS would read that one\n", s);
            return -1;
        }
    }
    /* which control superblock each CE's list selects (1.1: the highest page-0 usn, erased ignored) */
    for (unsigned c = 0; c < NCE; c++) {
        uint32_t best = NONE, bu = 0;
        for (int i = 0; i < 3; i++) {
            uint32_t sb = V[c].ctrl[i];
            if (sb >= NSB)
                continue;
            if (read_vpn(sb * PPSB, pdata, m, NULL) == 1 && le32(m + 4) != NONE && (best == NONE || le32(m + 4) > bu))
                best = sb, bu = le32(m + 4);
        }
        if (verbose)
            printf("  vCE %u's list %u %u %u selects superblock %u%s\n", c, V[c].ctrl[0], V[c].ctrl[1], V[c].ctrl[2],
                   best, best == C->sb ? "" : "  (NOT the one with the newest context)");
    }
    uint32_t after = C->set_off + C->set_n;
    if (after < C->next) {
        int st = read_vpn(C->sb * PPSB + after, pdata, m, NULL);
        C->marked = st == 1 && m[9] == T_CTX && le32(m + 4) == C->W + 1;
        if (!C->marked) {
            fprintf(stderr, "k93yaftl: page %u after the newest set is not iOS's marker (status %d type %02x usn %08x)"
                    "\n", after, st, m[9], le32(m + 4));
            return -1;
        }
    } else if (after != C->next) {
        fprintf(stderr, "k93yaftl: the control superblock's next page %u is not right after the set (%u)\n",
                C->next, after);
        return -1;
    }
    if (verbose)
        printf("context: superblock %u, set at %u (%u pages), W %08x; %s; next free page %u\n", C->sb, C->set_off,
               C->set_n, C->W, C->marked ? "ALREADY followed by the marker (iOS will restore)"
                                         : "nothing after it (iOS would quick-mount it)", C->next);
    return 0;
}

/* encoder check against iOS's own pages: decrypt + re-encrypt == what is on the flash */
static int enc_check(uint32_t user_vpn, uint32_t meta_vpn)
{
    static uint8_t raw[MAX_PAGE], dec[MAX_PAGE], enc[MAX_PAGE];
    uint8_t m[META_BYTES];
    uint32_t row;
    int bad = 0;
    if (read_vpn(user_vpn, raw, m, &row) != 1 || m[9] != T_USER)
        return fprintf(stderr, "k93yaftl: vpn %u is not a readable user page\n", user_vpn), -1;
    aesf_cbc(DK_USER, le32(m), raw, dec, G.c->page);
    aese_cbc(EK_USER, le32(m), dec, enc, G.c->page);
    bad |= memcmp(enc, raw, G.c->page) != 0;
    if (read_vpn(meta_vpn, raw, m, &row) != 1)
        return fprintf(stderr, "k93yaftl: vpn %u unreadable\n", meta_vpn), -1;
    aesf_cbc(DK_META, row, raw, dec, G.c->page);
    aese_cbc(EK_META, row, dec, enc, G.c->page);
    bad |= (memcmp(enc, raw, G.c->page) != 0) << 1;
    printf("encoder check: user page vpn %u %s, META page vpn %u %s\n", user_vpn, bad & 1 ? "DIFFERS" : "identical",
           meta_vpn, bad & 2 ? "DIFFERS" : "identical");
    return bad ? -1 : 0;
}

/* a free superblock Linux may take: every bank's first page erased, not a control
 * superblock, no physical block below 20 (VFL contexts) */
static int sb_free_ok(uint32_t sb, int verbose)
{
    uint8_t m[META_BYTES];
    if (sb < 1 || sb >= NSB || is_ctrl(sb))
        return 0;
    for (uint32_t b = 0; b < TB; b++) {
        int vce, rem;
        uint32_t pblk, row;
        vpn_where(sb * PPSB + b, &vce, &pblk, &row, &rem);
        if (pblk < 20 || pblk >= G.c->blocks)
            return 0;
        if (read_phys(vce, row, pdata, m) != 2)
            return 0;
        if (verbose)
            printf("    bank %u: vCE %d block %u%s\n", b, vce, pblk, rem ? " (replacement)" : "");
    }
    return 1;
}

/* the test pattern of an LPN: "CascadiaYAFTLtst", lpn, seed, then xorshift32 words */
static void lpn_pattern(uint32_t lpn, uint32_t seed, uint8_t *page)
{
    uint32_t *w = (uint32_t *)page, x = seed ^ lpn * 0x9e3779b9u;
    if (!x)
        x = 0x93;
    memcpy(page, "CascadiaYAFTLtst", 16);
    w[4] = lpn;
    w[5] = seed;
    for (unsigned i = 6; i < G.c->page / 4; i++) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        w[i] = x;
    }
}

static int prog_vpn(uint32_t vpn, const uint8_t *enc, const uint8_t meta[12], int erasable_ok)
{
    int vce, rem;
    uint32_t pblk, row;
    uint8_t mw[META_BYTES], nst = 0;
    (void)erasable_ok;
    vpn_where(vpn, &vce, &pblk, &row, &rem);
    meta_unwhiten(meta, mw, row);
    arm(OP_PROGRAM, vce, pblk);
    int r = program_page(vce_bus[vce], vce_ce[vce], row, enc, mw, &nst, META_PRE, 1, 0);
    disarm();
    if (r)
        fprintf(stderr, "k93yaftl: program of vpn %u (vCE %d block %u row 0x%x) failed: %d, status 0x%02x\n", vpn, vce,
                pblk, row, r, nst);
    return r;
}

/* ---- the flash under the write engine and its GC: the iPad's -- or, in K93YAFTL_SIM (the host test,
 * tools/nand/k93yaftl-sim.c), a model of it that iOS's restore is checked against */
#ifdef K93YAFTL_SIM
static int yv_read(uint32_t vpn, uint8_t *raw, uint8_t m[META_BYTES], uint32_t *rowp);
static uint32_t yv_row(uint32_t vpn);
static int yv_prog(uint32_t vpn, const uint8_t *enc, const uint8_t meta[12]);
static int yv_erase(uint32_t sb);
static int yv_free_ok(uint32_t sb);
#else
static int yv_read(uint32_t vpn, uint8_t *raw, uint8_t m[META_BYTES], uint32_t *rowp)
{
    return read_vpn(vpn, raw, m, rowp);
}

static uint32_t yv_row(uint32_t vpn)
{
    int vce, rem;
    uint32_t pblk, row;
    vpn_where(vpn, &vce, &pblk, &row, &rem);
    return row;
}

static int yv_prog(uint32_t vpn, const uint8_t *enc, const uint8_t meta[12])
{
    return prog_vpn(vpn, enc, meta, 1);
}

/* every bank's block of SB erased, each one's first page reading erased after it */
static int yv_erase(uint32_t sb)
{
    uint8_t m[META_BYTES];
    for (uint32_t b = 0; b < TB; b++) {
        int vce, rem;
        uint32_t pblk, r2;
        uint8_t nst = 0;
        vpn_where(sb * PPSB + b, &vce, &pblk, &r2, &rem);
        allow_block(vce, pblk);
        arm(OP_ERASE, vce, pblk);
        int r = erase_block(vce_bus[vce], vce_ce[vce], pblk, &nst);
        disarm();
        if (r || read_phys(vce, r2, pdata, m) != 2)
            return fprintf(stderr, "k93yaftl: erase of vCE %d block %u failed (%d, status 0x%02x)\n", vce, pblk, r,
                           nst), -1;
    }
    return 0;
}

static int yv_free_ok(uint32_t sb)
{
    return sb_free_ok(sb, 0);
}
#endif

static int verify_vpn(uint32_t vpn, const uint8_t *plain, const uint8_t meta[12], const uint32_t *dk, uint32_t seed)
{
    static uint8_t raw[MAX_PAGE], dec[MAX_PAGE];
    uint8_t m[META_BYTES];
    uint32_t row;
    int st = yv_read(vpn, raw, m, &row);
    if (st != 1)
        return fprintf(stderr, "k93yaftl: vpn %u reads back with status %d\n", vpn, st), -1;
    if (memcmp(m, meta, 10))
        return fprintf(stderr, "k93yaftl: vpn %u metadata reads back differently\n", vpn), -1;
    aesf_cbc(dk, dk == DK_META ? row : seed, raw, dec, G.c->page);
    if (memcmp(dec, plain, G.c->page))
        return fprintf(stderr, "k93yaftl: vpn %u data reads back differently\n", vpn), -1;
    return 0;
}

static void put_meta(uint8_t meta[12], uint32_t lpn, uint32_t usn, uint8_t type)
{
    memset(meta, 0xff, 12);
    memcpy(meta, &lpn, 4);
    memcpy(meta + 4, &usn, 4);
    meta[8] = 0;
    meta[9] = type;
}

/* write-test START SEED [--dry]: the marker, then one whole superblock of user pages
 * for LPNs START..START+2046 (the test pattern) and its table of contents */
static int cmd_write_test(char **argv, int argc)
{
    static uint8_t plain[MAX_PAGE] __attribute__((aligned(8))), enc[MAX_PAGE] __attribute__((aligned(8)));
    static uint32_t toc[MAX_PAGE / 4];
    uint8_t meta[12], m[META_BYTES];
    uint32_t row;
    struct ctl C;
    if (argc < 2)
        return fprintf(stderr, "usage: k93yaftl write-test START SEED [--dry] [--sb N]\n"), 1;
    uint32_t start = strtoul(argv[0], NULL, 0), seed = strtoul(argv[1], NULL, 0), want_sb = NONE;
    int dry = 0;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--dry"))
            dry = 1;
        else if (!strcmp(argv[i], "--sb") && i + 1 < argc)
            want_sb = strtoul(argv[++i], NULL, 0);
    }
    uint32_t nuser = PPSB - 1;
    if (vfl_open(0) || build_map(1))
        return 1;
    if (start + nuser > NLPN || start < 1)
        return fprintf(stderr, "k93yaftl: LPNs out of range\n"), 1;
    if (ctl_find(&C, 1))
        return 1;
    /* the encoder, on iOS's own pages: the first user page of the control set's open superblock, and page 2 */
    uint32_t user_vpn = NONE;
    for (uint32_t sb = NSB - 1; sb > 0 && user_vpn == NONE; sb--)
        if (read_vpn(sb * PPSB, pdata, m, NULL) == 1 && m[9] == T_USER)
            user_vpn = sb * PPSB;
    if (user_vpn == NONE || enc_check(user_vpn, C.sb * PPSB + C.page2_off))
        return 1;
    /* weaveSeq (yaftl-write-re.md 9): above every first-page usn, every page of the newest data and index
     * superblocks and every control page (the marker takes W + 1), plus a margin */
    uint32_t maxu = C.W + 1, newest_d = NONE, newest_i = NONE, nd = 0, ni = 0, nfree = 0;
    for (uint32_t s = 0; s < NSB; s++) {
        int st = -1;
        for (uint32_t off = 0; off + 1 < PPSB && st < 1; off++)
            if ((st = read_vpn(s * PPSB + off, pdata, m, NULL)) == 2 && off == 0)
                break;
        if (st == 2) {
            nfree += !is_ctrl(s);
            continue;
        }
        if (st != 1)
            continue;
        uint32_t u = le32(m + 4);
        if (u != NONE && u > maxu && !is_ctrl(s))
            maxu = u;
        if (m[9] == T_USER && (newest_d == NONE || u > nd))
            newest_d = s, nd = u;
        if (m[9] == T_INDEX && (newest_i == NONE || u > ni))
            newest_i = s, ni = u;
    }
    for (int k = 0; k < 2; k++) {
        uint32_t s = k ? newest_i : newest_d;
        for (uint32_t off = 0; s != NONE && off < PPSB; off++)
            if (read_vpn(s * PPSB + off, pdata, m, NULL) == 1 && le32(m + 4) != NONE && le32(m + 4) > maxu)
                maxu = le32(m + 4);
    }
    uint32_t usn = maxu + 16;
    printf("weaveSeq: newest context %08x, highest elsewhere %08x (newest data superblock %u, index %u) -> %08x; "
           "%u free superblocks\n", C.W, maxu, newest_d, newest_i, usn, nfree);
    if (nfree < 32)
        return fprintf(stderr, "k93yaftl: only %u free superblocks: not taking one\n", nfree), 1;
    /* the superblock */
    uint32_t sb = NONE;
    if (want_sb != NONE)
        sb = sb_free_ok(want_sb, 0) ? want_sb : NONE;
    else
        for (uint32_t s = NSB - 1; s > 0 && sb == NONE; s--)
            if (sb_free_ok(s, 0))
                sb = s;
    if (sb == NONE)
        return fprintf(stderr, "k93yaftl: no free superblock passes the checks\n"), 1;
    printf("plan: %s; superblock %u for LPNs 0x%x..0x%x (seed 0x%x) with weaveSeq %08x, its table of contents at "
           "page %u\n", C.marked ? "no marker needed" : "marker at control page", sb, start, start + nuser - 1, seed,
           usn, PPSB - 1);
    if (!C.marked)
        printf("      marker: superblock %u page %u, weaveSeq %08x, a copy of page %u\n", C.sb, C.next, C.W + 1,
               C.page2_off);
    sb_free_ok(sb, 1);
    for (uint32_t i = 0; i < nuser; i++) {
        uint32_t v = l2v_get(start + i);
        if (v != NONE && i < 4)
            printf("      LPN 0x%x is mapped now (vpn %u): it will be replaced\n", start + i, v);
    }
    if (dry) {
        printf("dry run: nothing written\n");
        return 0;
    }
    hold_signals();
    /* 1. the marker */
    if (!C.marked) {
        int vce, rem;
        uint32_t pblk, r2;
        if (read_vpn(C.sb * PPSB + C.page2_off, pdata, m, &row) != 1)
            return 1;
        aesf_cbc(DK_META, row, pdata, plain, G.c->page);
        vpn_where(C.sb * PPSB + C.next, &vce, &pblk, &r2, &rem);
        if (read_phys(vce, r2, pdata, m) != 2)
            return fprintf(stderr, "k93yaftl: the marker's page is not erased\n"), 1;
        allow_block(vce, pblk);
        aese_cbc(EK_META, r2, plain, enc, G.c->page);
        put_meta(meta, 0, C.W + 1, T_CTX);
        if (prog_vpn(C.sb * PPSB + C.next, enc, meta, 0) ||
            verify_vpn(C.sb * PPSB + C.next, plain, meta, DK_META, 0))
            return 1;
        printf("marker written and read back: superblock %u page %u\n", C.sb, C.next);
    }
    /* 2. the superblock's blocks: allowed, erased */
    for (uint32_t b = 0; b < TB; b++) {
        int vce, rem;
        uint32_t pblk, r2;
        uint8_t nst = 0;
        vpn_where(sb * PPSB + b, &vce, &pblk, &r2, &rem);
        allow_block(vce, pblk);
        arm(OP_ERASE, vce, pblk);
        int r = erase_block(vce_bus[vce], vce_ce[vce], pblk, &nst);
        disarm();
        if (r || read_phys(vce, r2, pdata, m) != 2)
            return fprintf(stderr, "k93yaftl: erase of vCE %d block %u failed (%d, status 0x%02x)\n", vce, pblk, r,
                           nst), 1;
    }
    printf("superblock %u: its %u blocks erased\n", sb, TB);
    /* 3. the user pages, in vpn order */
    uint64_t t0 = now_us();
    for (uint32_t off = 0; off < nuser; off++) {
        uint32_t lpn = start + off;
        lpn_pattern(lpn, seed, plain);
        aese_cbc(EK_USER, lpn, plain, enc, G.c->page);
        put_meta(meta, lpn, usn, T_USER);
        if (prog_vpn(sb * PPSB + off, enc, meta, 1) || verify_vpn(sb * PPSB + off, plain, meta, DK_USER, lpn))
            return fprintf(stderr, "k93yaftl: stopped at page %u\n", off), 1;
        toc[off] = lpn;
    }
    printf("%u user pages programmed and read back (%.1f s)\n", nuser, (now_us() - t0) / 1e6);
    /* 4. the table of contents */
    memset(plain, 0xff, G.c->page);
    memcpy(plain, toc, 4 * nuser);
    {
        int vce, rem;
        uint32_t pblk, r2;
        vpn_where(sb * PPSB + nuser, &vce, &pblk, &r2, &rem);
        aese_cbc(EK_META, r2, plain, enc, G.c->page);
        put_meta(meta, 0, 0, T_BTOC);
        if (prog_vpn(sb * PPSB + nuser, enc, meta, 1) || verify_vpn(sb * PPSB + nuser, plain, meta, DK_META, 0))
            return 1;
    }
    printf("table of contents written; superblock %u closed\n", sb);
    return 0;
}

/* check START SEED: through the reader's map (rebuilt here, as the server does), every LPN of
 * the test reads as its pattern */
static int cmd_ycheck(char **argv, int argc)
{
    static uint8_t got[MAX_PAGE], want[MAX_PAGE];
    if (argc < 2)
        return 1;
    uint32_t start = strtoul(argv[0], NULL, 0), seed = strtoul(argv[1], NULL, 0), bad = 0, n = PPSB - 1;
    if (vfl_open(0) || build_map(1))
        return 1;
    for (uint32_t i = 0; i < n; i++) {
        lpn_pattern(start + i, seed, want);
        if (read_lpn(start + i, got) || memcmp(got, want, G.c->page))
            if (bad++ < 5)
                fprintf(stderr, "LPN 0x%x: not the pattern (vpn %u)\n", start + i, l2v_get(start + i));
    }
    printf("check: %u of %u LPNs from 0x%x read back as the pattern\n", n - bad, n, start);
    return bad != 0;
}

/* ---- the write engine: what the NBD server runs, and write-lpns --------------------------------------
 * yw_init: map, context, weaveSeq base.  yw_open: the marker first (once), then a free superblock,
 * its blocks erased.  yw_put: the next page of it, read back.  yw_close: the table of contents when
 * the superblock is full; a partial one stays open (iOS's restore takes its pages up to the first
 * hole, then copies them out: yaftl-write-re.md 3.4, 3.6).  In the server, below gc_low free
 * superblocks, yw_put first gives some back (the GC, further down). */

struct yw {
    struct ctl C;
    uint32_t base;              /* next weaveSeq */
    uint32_t usn, sb, off;      /* the open superblock */
    uint32_t toc[MAX_PAGE / 4];
    int marked, open, verbose, dry, verify, in_gc;
    uint32_t nfree, last_sb;
    uint64_t pages, sbs, failed;
};

/* what yw_init saw of each superblock: 0 nothing it uses, 1 free, 2 data (first readable page T_USER) */
static uint8_t *sbkind;
/* the free superblocks never taken: iOS's next boot needs them for its restore and GC */
static uint32_t yw_keep = 32;

/* the GC's view (NULL: no GC -- the one-shot commands): per superblock, whose it is and how many of
 * its pages are the current copy of their LPN */
enum { SB_NOT = 0, SB_CLOSED = 1, SB_OPEN = 2, SB_GIVEN_UP = 3 };
static uint8_t *sbstate;
static uint16_t *sbvalid;
static uint32_t gc_low = 48;
static struct { uint64_t runs, victims, moved, failed; } gst;

static void l2v_set(uint32_t lpn, uint32_t vpn)
{
    take(lpn, vpn);                            /* makes the map block */
    l2v[lpn >> MB_SHIFT][lpn & (MB_LPNS - 1)] = vpn;
    pthread_mutex_lock(&pcache_mu);             /* the reader's page cache must not keep the old copy */
    for (unsigned i = 0; i < PCACHE; i++)
        if (pcache[i].lpn == lpn)
            pcache[i].lpn = NONE;
    pthread_mutex_unlock(&pcache_mu);
}

static int yw_init(struct yw *y, int verbose)
{
    uint8_t m[META_BYTES];
    memset(y, 0, sizeof(*y));
    y->verbose = verbose;
    y->verify = 1;
    y->sb = NONE;
    y->last_sb = NSB;
    if (ctl_find(&y->C, verbose))
        return -1;
    y->marked = y->C.marked;
    if (!sbkind)
        sbkind = calloc(NSB, 1);
    uint32_t maxu = y->C.W + 1, nd = 0, ni = 0, newest_d = NONE, newest_i = NONE;
    for (uint32_t s = 0; s < NSB; s++) {
        int st = -1;
        for (uint32_t off = 0; off + 1 < PPSB && st < 1; off++)
            if ((st = read_vpn(s * PPSB + off, pdata, m, NULL)) == 2 && off == 0)
                break;
        if (st == 2) {
            y->nfree += !is_ctrl(s);
            if (sbkind)
                sbkind[s] = 1;
            continue;
        }
        if (st != 1)
            continue;
        uint32_t u = le32(m + 4);
        if (u != NONE && u > maxu && !is_ctrl(s))
            maxu = u;
        if (m[9] == T_USER && sbkind)
            sbkind[s] = 2;
        if (m[9] == T_USER && (newest_d == NONE || u > nd))
            newest_d = s, nd = u;
        if (m[9] == T_INDEX && (newest_i == NONE || u > ni))
            newest_i = s, ni = u;
    }
    for (int k = 0; k < 2; k++) {
        uint32_t s = k ? newest_i : newest_d;
        for (uint32_t off = 0; s != NONE && off < PPSB; off++)
            if (read_vpn(s * PPSB + off, pdata, m, NULL) == 1 && le32(m + 4) != NONE && le32(m + 4) > maxu)
                maxu = le32(m + 4);
    }
    y->base = maxu + 16;
    if (verbose)
        printf("weaveSeq: newest context %08x, highest elsewhere %08x (newest data superblock %u, index %u) -> from "
               "%08x; %u free superblocks\n", y->C.W, maxu, newest_d, newest_i, y->base, y->nfree);
    return 0;
}

static int yw_mark(struct yw *y)
{
    static uint8_t plain[MAX_PAGE] __attribute__((aligned(8))), enc[MAX_PAGE] __attribute__((aligned(8)));
    uint8_t meta[12], m[META_BYTES];
    uint32_t row, r2, pblk;
    int vce, rem;
    if (y->marked)
        return 0;
    if (y->dry) {
        printf("dry run: would write the marker at superblock %u page %u\n", y->C.sb, y->C.next);
        return 0;
    }
    if (read_vpn(y->C.sb * PPSB + y->C.page2_off, pdata, m, &row) != 1)
        return -1;
    aesf_cbc(DK_META, row, pdata, plain, G.c->page);
    vpn_where(y->C.sb * PPSB + y->C.next, &vce, &pblk, &r2, &rem);
    if (read_phys(vce, r2, pdata, m) != 2)
        return fprintf(stderr, "k93yaftl: the marker's page is not erased\n"), -1;
    allow_block(vce, pblk);
    aese_cbc(EK_META, r2, plain, enc, G.c->page);
    put_meta(meta, 0, y->C.W + 1, T_CTX);
    if (prog_vpn(y->C.sb * PPSB + y->C.next, enc, meta, 0) ||
        verify_vpn(y->C.sb * PPSB + y->C.next, plain, meta, DK_META, 0))
        return -1;
    y->marked = 1;
    if (y->verbose)
        printf("marker written and read back: superblock %u page %u\n", y->C.sb, y->C.next);
    return 0;
}

static int yw_open(struct yw *y, uint32_t want)
{
    uint32_t sb = NONE;
    if (y->nfree < yw_keep)
        return fprintf(stderr, "k93yaftl: only %u free superblocks: not taking one\n", y->nfree), -1;
    if (want != NONE)
        sb = yv_free_ok(want) ? want : NONE;
    else
        for (uint32_t k = 1; k < NSB && sb == NONE; k++) {
            uint32_t s = (y->last_sb + NSB - k) % NSB;  /* downwards from the last one, round */
            if (s && yv_free_ok(s))
                sb = s;
        }
    if (sb == NONE)
        return fprintf(stderr, "k93yaftl: no free superblock passes the checks\n"), -1;
    y->last_sb = sb;
    if (y->verbose) {
        printf("superblock %u, weaveSeq %08x%s\n", sb, y->base, y->sbs ? "" : ":");
#ifndef K93YAFTL_SIM
        if (!y->sbs)                            /* the first one's banks; the others are alike */
            sb_free_ok(sb, 1);
#endif
    }
    if (yw_mark(y))
        return -1;
    if (!y->dry && yv_erase(sb))
        return -1;
    y->sb = sb;
    y->sbs++;
    y->usn = y->base;
    y->base += 16;
    y->off = 0;
    y->open = 1;
    y->nfree--;
    if (sbstate) {
        sbstate[sb] = SB_OPEN;
        sbvalid[sb] = 0;
    }
    memset(y->toc, 0xff, sizeof(y->toc));
    return 0;
}

static int yw_close(struct yw *y);
static void yw_gc(struct yw *y);

/* a GC copy read back: the page as it was programmed, byte for byte */
static int verify_raw(uint32_t vpn, const uint8_t *raw, const uint8_t meta[12])
{
    static uint8_t got[MAX_PAGE] __attribute__((aligned(8)));
    uint8_t m[META_BYTES];
    int st = yv_read(vpn, got, m, NULL);
    if (st != 1)
        return fprintf(stderr, "k93yaftl: vpn %u reads back with status %d\n", vpn, st), -1;
    if (memcmp(m, meta, 10))
        return fprintf(stderr, "k93yaftl: vpn %u metadata reads back differently\n", vpn), -1;
    if (memcmp(got, raw, G.c->page))
        return fprintf(stderr, "k93yaftl: vpn %u data reads back differently\n", vpn), -1;
    return 0;
}

/* the next page of the open superblock for LPN: PLAIN, encrypted here -- or RAW, a page as the flash
 * holds it (the GC's copies: the data's encryption follows the LPN, not where the page sits) */
static int yw_put_enc(struct yw *y, uint32_t lpn, const uint8_t *plain, const uint8_t *raw)
{
    static uint8_t enc[MAX_PAGE] __attribute__((aligned(8)));
    uint8_t meta[12];
    if (lpn >= NLPN || lpn >= 3933468u)
        return fprintf(stderr, "k93yaftl: LPN 0x%x is past the disk\n", lpn), -1;
    if (!y->open) {
        if (sbstate && !y->in_gc && !y->dry && y->nfree < gc_low)
            yw_gc(y);                           /* may leave a superblock open, holding its copies */
        if (!y->open && yw_open(y, NONE))
            return -1;
    }
    uint32_t vpn = y->sb * PPSB + y->off;
    if (!y->dry) {
        const uint8_t *e = raw;
        if (!e) {
            aese_cbc(EK_USER, lpn, plain, enc, G.c->page);
            e = enc;
        }
        put_meta(meta, lpn, y->usn, T_USER);
        if (yv_prog(vpn, e, meta) ||
            (y->verify && (raw ? verify_raw(vpn, raw, meta) : verify_vpn(vpn, plain, meta, DK_USER, lpn)))) {
            /* never leave a hole to write past: iOS's restore takes the newest open superblock only up to its
             * first bad page (yaftl-write-re.md 3.4).  This one is given up; the next write opens another. */
            fprintf(stderr, "k93yaftl: page %u of superblock %u failed: superblock given up\n", y->off, y->sb);
            if (sbstate)
                sbstate[y->sb] = SB_GIVEN_UP;
            y->open = 0;
            y->failed++;
            return -1;
        }
        uint32_t old = l2v_get(lpn);
        l2v_set(lpn, vpn);
        if (sbstate) {
            sbvalid[y->sb]++;
            if (old != NONE && sbstate[old / PPSB] && sbvalid[old / PPSB])
                sbvalid[old / PPSB]--;
        }
        y->pages++;
    }
    y->toc[y->off++] = lpn;
    if (y->off == PPSB - 1)
        yw_close(y);                            /* a failed table gives the superblock up, not this page */
    return 0;
}

static int yw_put(struct yw *y, uint32_t lpn, const uint8_t *plain)
{
    return yw_put_enc(y, lpn, plain, NULL);
}

static int yw_close(struct yw *y)
{
    static uint8_t plain[MAX_PAGE] __attribute__((aligned(8))), enc[MAX_PAGE] __attribute__((aligned(8)));
    uint8_t meta[12];
    if (!y->open || y->off != PPSB - 1)
        return 0;                               /* partial: stays open, no table */
    if (!y->dry) {
        uint32_t vpn = y->sb * PPSB + y->off;
        memcpy(plain, y->toc, G.c->page);
        aese_cbc(EK_META, yv_row(vpn), plain, enc, G.c->page);
        put_meta(meta, 0, 0, T_BTOC);
        if (yv_prog(vpn, enc, meta) || verify_vpn(vpn, plain, meta, DK_META, 0)) {
            /* its pages stay mapped and readable; with no table iOS's restore reads them one by one and
             * copies them out (3.4, 3.6) -- and the next write must not land on the table's page */
            fprintf(stderr, "k93yaftl: superblock %u's table of contents failed: superblock given up\n", y->sb);
            if (sbstate)
                sbstate[y->sb] = SB_GIVEN_UP;
            y->open = 0;
            y->failed++;
            return -1;
        }
    }
    if (y->verbose)
        printf("superblock %u full: table of contents written\n", y->sb);
    if (sbstate)
        sbstate[y->sb] = SB_CLOSED;
    y->open = 0;
    return 0;
}

/* ---- room within a session: the GC -------------------------------------------------------------------
 * iOS frees space only when it runs (its restore after a Linux session GCs and erases: yaftl-write-re.md
 * 3.6).  Until then every superblock this server fills stays used, so a session writing more than the
 * free superblocks hold (~27 GB at the 1722 free of 2026-11-16) would run into the yw_keep left for iOS.
 * So below gc_low free superblocks, the superblocks whose every page belongs to a partition written here
 * (Linux, Swap -- not System: this session's and older ones alike) give space back: the one with the fewest current
 * pages first, those copied as they are into the open superblock, then its blocks erased.  The copies
 * carry a newer weaveSeq than the victim, so iOS's restore takes them wherever it stops -- before the
 * erase both copies exist and the newer wins, after it only that one is left.  A superblock whose
 * current page does not read back as listed is left alone, and iOS's own (any page of System, Data or
 * LwVM's header) are never touched.  K93YAFTL_GC_LOW=N (>= 40) moves the mark, for tests. */

static uint8_t rwchunk[1024];   /* the LwVM chunks (disk0 / lwvm_chunk) of the partitions written here and
                                 * Linux's alone (Linux, Swap): a superblock holding a page of System's is
                                 * never moved, whoever wrote it */

static int lpn_ours(uint32_t lpn)
{
    uint64_t c = (uint64_t)lpn * G.c->page / lwvm_chunk;
    return c < 1024 && rwchunk[c];
}

/* SB's table of contents, decrypted into TOC -- or, for a superblock given up (no table), each page's
 * LPN from its own metadata (NONE where a page does not read as a user page) */
static int gc_toc(uint32_t sb, uint32_t *toc)
{
    static uint8_t raw[MAX_PAGE] __attribute__((aligned(8)));
    uint8_t m[META_BYTES];
    uint32_t row;
    if (sbstate && sbstate[sb] == SB_GIVEN_UP) {
        for (uint32_t off = 0; off + 1 < PPSB; off++)
            toc[off] = yv_read(sb * PPSB + off, raw, m, NULL) == 1 && m[9] == T_USER ? le32(m) : NONE;
        return 0;
    }
    if (yv_read(sb * PPSB + PPSB - 1, raw, m, &row) != 1 || m[9] != T_BTOC)
        return -1;
    aesf_cbc(DK_META, row, raw, (uint8_t *)toc, G.c->page);
    return 0;
}

/* which data superblocks hold only our partitions' pages, and how many of them are current (rwchunk
 * and the map first) */
static void gc_setup(void)
{
    static uint32_t toc[MAX_PAGE / 4];
    const char *e = getenv("K93YAFTL_GC_LOW");
    if (e && atoi(e) >= 40)
        gc_low = atoi(e);
    sbstate = calloc(NSB, 1);
    sbvalid = calloc(NSB, sizeof(*sbvalid));
    if (!sbstate || !sbvalid) {
        free(sbstate);
        free(sbvalid);
        sbstate = NULL;
        fprintf(stderr, "k93yaftl: no memory for the GC: no GC this time\n");
        return;
    }
    unsigned n = 0;
    uint64_t cur = 0;
    for (uint32_t sb = 0; sb < NSB; sb++) {
        if (!sbkind || sbkind[sb] != 2 || is_ctrl(sb) || gc_toc(sb, toc))
            continue;
        uint32_t v = 0, ours = 1;
        for (uint32_t off = 0; off + 1 < PPSB && ours; off++) {
            uint32_t lpn = toc[off];
            if (lpn == NONE)
                continue;
            if (!lpn_ours(lpn))
                ours = 0;
            else
                v += l2v_get(lpn) == sb * PPSB + off;
        }
        if (!ours)
            continue;
        sbstate[sb] = SB_CLOSED;
        sbvalid[sb] = v;
        n++;
        cur += v;
    }
    fprintf(stderr, "k93yaftl: GC: %u superblock(s) hold only the written partitions' pages (%llu MB of them "
            "current); space is given back below %u free superblocks\n", n,
            (unsigned long long)(cur * G.c->page >> 20), gc_low);
}

/* the candidate (full, or given up) with the fewest current pages -- none with 3/4 or more: too little
 * to gain */
static uint32_t gc_pick(void)
{
    uint32_t best = NONE;
    for (uint32_t sb = 0; sb < NSB; sb++)
        if ((sbstate[sb] == SB_CLOSED || sbstate[sb] == SB_GIVEN_UP) && sbvalid[sb] < (PPSB - 1) * 3 / 4 &&
            (best == NONE || sbvalid[sb] < sbvalid[best]))
            best = sb;
    return best;
}

static int gc_one(struct yw *y, uint32_t v)
{
    static uint32_t toc[MAX_PAGE / 4];
    static uint8_t raw[MAX_PAGE] __attribute__((aligned(8)));
    uint8_t m[META_BYTES];
    uint32_t moved = 0;
    if (gc_toc(v, toc)) {
        sbstate[v] = SB_NOT;
        return fprintf(stderr, "k93yaftl: GC: superblock %u's table of contents does not read: left alone\n", v),
               -1;
    }
    for (uint32_t off = 0; off + 1 < PPSB; off++) {
        uint32_t lpn = toc[off], vpn = v * PPSB + off;
        if (lpn == NONE || l2v_get(lpn) != vpn)
            continue;
        if (yv_read(vpn, raw, m, NULL) != 1 || m[9] != T_USER || le32(m) != lpn) {
            sbstate[v] = SB_NOT;
            return fprintf(stderr, "k93yaftl: GC: superblock %u page %u (LPN 0x%x) does not read back as listed: "
                           "left alone\n", v, off, lpn), -1;
        }
        if (yw_put_enc(y, lpn, NULL, raw))
            return -1;                          /* the copy's place failed, not this one: again later */
        moved++;
    }
    /* nothing of it may be current any more -- checked on the map itself, not on the count */
    for (uint32_t off = 0; off + 1 < PPSB; off++)
        if (toc[off] != NONE && l2v_get(toc[off]) == v * PPSB + off) {
            sbstate[v] = SB_NOT;
            return fprintf(stderr, "k93yaftl: GC: superblock %u page %u still current after the copies: left "
                           "alone\n", v, off), -1;
        }
    sbstate[v] = SB_NOT;
    sbvalid[v] = 0;
    if (yv_erase(v))
        return -1;
    y->nfree++;
    gst.victims++;
    gst.moved += moved;
    if (y->verbose)
        printf("GC: superblock %u: %u current page(s) moved, then erased; %u free\n", v, moved, y->nfree);
    return 0;
}

/* up to MAX victims while fewer than UNTIL superblocks are free */
static void yw_gc_run(struct yw *y, int max, uint32_t until)
{
    y->in_gc = 1;
    gst.runs++;
    if (yw_mark(y) == 0)                        /* nothing is erased before iOS's marker (there already) */
        for (int k = 0; k < max && y->nfree < until; k++) {
            uint32_t v = gc_pick();
            if (v == NONE)
                break;
            if (gc_one(y, v))
                gst.failed++;
        }
    y->in_gc = 0;
}

/* at a superblock boundary, below gc_low free: up to 16 victims, until gc_low + 8 are free */
static void yw_gc(struct yw *y)
{
    yw_gc_run(y, 16, gc_low + 8);
}

/* `echo N > /run/ios/gc` on the iPad: N superblocks given back at the next quiet second, however many
 * are free (tests; the GC proper starts by itself below gc_low).  Through a descriptor of /run/ios taken
 * at the start: the server outlives initramfs/init's switch_root, after which its own /run is the old
 * root's empty directory and the tmpfs is the new root's (mount --move). */
static int gc_dirfd = -1;

static void gc_poke(struct yw *y)
{
    char b[16] = { 0 };
    if (!sbstate || y->in_gc || y->dry || gc_dirfd < 0)
        return;
    int fd = openat(gc_dirfd, "gc", O_RDONLY);
    if (fd < 0)
        return;
    ssize_t r = read(fd, b, sizeof(b) - 1);
    close(fd);
    unlinkat(gc_dirfd, "gc", 0);
    int n = r > 0 ? atoi(b) : 0;
    if (n <= 0 || n > 64)
        return;
    uint64_t v0 = gst.victims, m0 = gst.moved;
    yw_gc_run(y, n, NSB);
    fprintf(stderr, "k93yaftl: GC asked for %d: %llu superblock(s) given back, %llu pages moved; %u free\n", n,
            (unsigned long long)(gst.victims - v0), (unsigned long long)(gst.moved - m0), y->nfree);
}

/* write-lpns START FILE [--expect OLD] [--dry]: FILE's 8 KB pages to LPNs START.., in a fresh
 * superblock; with --expect, LPN START must read as OLD's first page now (the LwVM table's guard) */
static int cmd_write_lpns(char **argv, int argc)
{
    static uint8_t cur[MAX_PAGE], buf[MAX_PAGE];
    struct yw y;
    const char *expect = NULL;
    int dry = 0;
    if (argc < 2)
        return fprintf(stderr, "usage: k93yaftl write-lpns START FILE [--expect OLD] [--dry]\n"), 1;
    uint32_t start = strtoul(argv[0], NULL, 0);
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--dry"))
            dry = 1;
        else if (!strcmp(argv[i], "--expect") && i + 1 < argc)
            expect = argv[++i];
    }
    FILE *f = fopen(argv[1], "rb");
    if (!f)
        return perror(argv[1]), 1;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len <= 0 || len % G.c->page || len / G.c->page > (long)(PPSB - 1))
        return fprintf(stderr, "k93yaftl: %s: %ld bytes, not 1..2047 pages of 8192\n", argv[1], len), 1;
    uint32_t n = len / G.c->page;
    if (vfl_open(0) || build_map(1) || yw_init(&y, 1))
        return 1;
    y.dry = dry;
    if (expect) {
        FILE *e = fopen(expect, "rb");
        if (!e || fread(buf, 1, G.c->page, e) != G.c->page)
            return fprintf(stderr, "k93yaftl: cannot read %s\n", expect), 1;
        fclose(e);
        if (read_lpn(start, cur) || memcmp(cur, buf, G.c->page))
            return fprintf(stderr, "k93yaftl: LPN 0x%x is not what %s says it is now: stopping\n", start, expect), 1;
        printf("LPN 0x%x reads as %s: ok\n", start, expect);
    }
    if (dry)
        printf("dry run: %u page(s) for LPNs 0x%x..0x%x\n", n, start, start + n - 1);
    else
        hold_signals();
    for (uint32_t i = 0; i < n; i++) {
        if (fread(buf, 1, G.c->page, f) != G.c->page)
            return 1;
        if (yw_put(&y, start + i, buf))
            return 1;
    }
    fclose(f);
    if (yw_close(&y))
        return 1;
    if (!dry) {
        if (!(f = fopen(argv[1], "rb")))
            return perror(argv[1]), 1;
        unsigned bad = 0;
        for (uint32_t i = 0; i < n; i++) {
            if (fread(buf, 1, G.c->page, f) != G.c->page || read_lpn(start + i, cur) || memcmp(cur, buf, G.c->page))
                bad++;
        }
        fclose(f);
        printf("%u page(s) written to LPNs 0x%x.. in superblock %u (%s); read back through the map: %u differ\n", n,
               start, y.sb, y.open ? "left open, as iOS leaves its current one" : "closed", bad);
        return bad != 0;
    }
    return 0;
}

/* ---- the LwVM table: iOS's own checks, and writing both copies -------------------------------
 * build/k93/research-write/lwvm-re.md: LightweightVolumeManager (iOS 6.1.3, kext 0x8069f000) refuses
 * a table failing any of these and then publishes no partitions at all (no root: the Apple logo for
 * good).  tools/k93-lwvm.py's kernel_check is the same; this one runs on the iPad, right before the
 * write.  The header has a backup at the end of chunk 0 (LPN 4095 here), used only when the main
 * one cannot be read: both get the new table. */

static uint64_t le64(const uint8_t *p)
{
    return (uint64_t)le32(p) | (uint64_t)le32(p + 4) << 32;
}

static const char *lwvm_check(const uint8_t *h, uint64_t media)
{
    static char msg[160];
    static const uint8_t MCRC[16] = { 0x6a, 0x90, 0x88, 0xcf, 0x8a, 0xfd, 0x63, 0x0a,
                                      0xe3, 0x51, 0xe2, 0x48, 0x87, 0xe0, 0xb9, 0x8b };
    static const uint8_t MOLD[16] = { 0xb1, 0x89, 0xa5, 0x19, 0x4f, 0x59, 0x4b, 0x1d,
                                      0xad, 0x44, 0x1e, 0x12, 0x7a, 0xaf, 0x45, 0x39 };
    static uint8_t z[0x1000];
    const uint32_t block = 8192;
    if (memcmp(h, MCRC, 16) && memcmp(h, MOLD, 16))
        return "LwVM: media header mismatch";
    if (!memcmp(h, MCRC, 16)) {
        memcpy(z, h, 0x1000);
        memset(z + 0x2c, 0, 4);
        if (crc32(z, 0x1000) != le32(h + 0x2c))
            return "media header failed validation (CRC)";
    }
    if (le64(h + 0x20) != media)
        return "LwVM: media size mismatch";
    unsigned shift = 0;
    for (uint64_t x = (media - 1) >> 10; x; x >>= 1)
        shift++;
    uint64_t ch = 1ull << shift, n = media >> shift, first = 2 * block + 0x4000, last = first + ((n - 8) << shift);
    uint32_t cnt = le32(h + 0x28);
    const uint8_t *m8 = h + 0x800;
#define MAPV(c) ((uint32_t)m8[2 * (c)] | (uint32_t)m8[2 * (c) + 1] << 8)
    if (cnt > 8)
        return "LwVM: illegal partition count";
    for (uint32_t i = 0; i < cnt; i++) {
        const uint8_t *r = h + 0x200 + i * 0x80;
        uint64_t b = le64(r + 0x20), e = le64(r + 0x28);
        int tz = 1, iz = 1;
        for (int k = 0; k < 16; k++)
            tz &= !r[k], iz &= !r[16 + k];
        if (tz || iz || b < first || e > last || e <= b || b % block || e % block) {
            snprintf(msg, sizeof msg, "LwVM: partition %u: type/identity zero, out of %#llx-%#llx, empty or not "
                     "aligned", i, (unsigned long long)first, (unsigned long long)last);
            return msg;
        }
        for (uint32_t j = 0; j < i; j++) {
            const uint8_t *q = h + 0x200 + j * 0x80;
            if (e > le64(q + 0x20) && b < le64(q + 0x28)) {
                snprintf(msg, sizeof msg, "LwVM: partition %u overlaps partition %u", i, j);
                return msg;
            }
        }
        uint64_t need = (e - 1 - b + ch) >> shift;
        for (uint64_t L = 0; L < need; L++) {
            uint32_t want = (uint32_t)(L & 0x3ff) | (i & 0xf) << 12, seen = 0;
            for (uint64_t c = 1; c < n; c++)
                seen += MAPV(c) == want;
            if (seen != 1) {
                snprintf(msg, sizeof msg, "LwVM: partition %u missing chunk 0x%04llx", i, (unsigned long long)L);
                return msg;
            }
        }
        for (unsigned c = 0; c < 1024; c++)
            if (MAPV(c) >> 12 == i && (MAPV(c) & 0x3ff) >= need) {
                snprintf(msg, sizeof msg, "kernel PANIC at boot: partition %u's logical chunk %u", i, MAPV(c) & 0x3ff);
                return msg;
            }
    }
    for (uint32_t i = cnt; i < 8; i++)
        for (int k = 0; k < 0x30; k++)
            if (h[0x200 + i * 0x80 + k]) {
                snprintf(msg, sizeof msg, "LwVM: partition %u orphaned", i);
                return msg;
            }
    if (MAPV(0) != 0xf000)
        return "LwVM: chunk zero not marked reserved";
    for (uint64_t c = 1; c < n; c++) {
        uint32_t v = MAPV(c), pi = v >> 12;
        if (v == 0xf111)
            continue;
        if (pi >= cnt) {
            snprintf(msg, sizeof msg, "LwVM: chunk 0x%04llx claims nonexistent partition (0x%04x)",
                     (unsigned long long)c, v);
            return msg;
        }
        const uint8_t *r = h + 0x200 + pi * 0x80;
        if ((uint64_t)(v & 0x3ff) << shift > le64(r + 0x28) - le64(r + 0x20)) {
            snprintf(msg, sizeof msg, "LwVM: chunk 0x%04llx assigned to invalid partition offset (0x%04x)",
                     (unsigned long long)c, v);
            return msg;
        }
    }
    for (unsigned c = n; c < 1024; c++)
        if (MAPV(c) != 0xf3ff) {
            snprintf(msg, sizeof msg, "LwVM: chunk 0x%04x not invalid (0x%04x)", c, MAPV(c));
            return msg;
        }
#undef MAPV
    return NULL;
}

/* lwvm-apply NEW OLD [--dry]: the table NEW into LPN 0 and its backup LPN 4095, if LPN 0 holds OLD now
 * and NEW passes iOS's check; nothing if both already hold NEW */
static int cmd_lwvm_apply(char **argv, int argc)
{
    static uint8_t neu[MAX_PAGE], old[MAX_PAGE], cur[MAX_PAGE], bak[MAX_PAGE];
    struct yw y;
    int dry = argc > 2 && !strcmp(argv[2], "--dry");
    const uint32_t BAK = 4095;                  /* chunk (32 MB) - one 8 KB block */
    if (argc < 2)
        return fprintf(stderr, "usage: k93yaftl lwvm-apply NEW OLD [--dry]\n"), 1;
    FILE *f;
    if (!(f = fopen(argv[0], "rb")) || fread(neu, 1, 8192, f) != 8192 || fclose(f))
        return fprintf(stderr, "k93yaftl: %s: not an 8192-byte table\n", argv[0]), 1;
    if (!(f = fopen(argv[1], "rb")) || fread(old, 1, 8192, f) != 8192 || fclose(f))
        return fprintf(stderr, "k93yaftl: %s: not an 8192-byte table\n", argv[1]), 1;
    if (G.c->page != 8192 || vfl_open(0) || build_map(1))
        return 1;
    if (read_lpn(0, cur) || read_lpn(BAK, bak))
        return fprintf(stderr, "k93yaftl: LPN 0 or %u unreadable\n", BAK), 1;
    uint64_t media = le64(cur + 0x20);
    const char *why = lwvm_check(neu, media);
    if (why)
        return fprintf(stderr, "k93yaftl: %s: iOS would refuse it: %s -- not written\n", argv[0], why), 1;
    printf("%s: passes iOS's check\n", argv[0]);
    int main_new = !memcmp(cur, neu, 8192), bak_new = !memcmp(bak, neu, 0x1000);
    if (main_new && bak_new) {
        printf("LPN 0 and LPN %u already hold it: nothing to do\n", BAK);
        return 0;
    }
    if (!main_new && memcmp(cur, old, 8192))
        return fprintf(stderr, "k93yaftl: LPN 0 is not %s (the table changed since the plan): not written\n",
                       argv[1]), 1;
    printf("LPN 0: %s; backup LPN %u: %s\n", main_new ? "already the new table" : "the expected old table", BAK,
           bak_new ? "already the new table" : !memcmp(bak, old, 0x1000) ? "the old table" :
           "something else (written over)");
    if (yw_init(&y, 1))
        return 1;
    y.dry = dry;
    if (dry) {
        printf("dry run: would write LPN 0 and LPN %u\n", BAK);
        return 0;
    }
    hold_signals();
    if ((!main_new && yw_put(&y, 0, neu)) || (!bak_new && yw_put(&y, BAK, neu)) || yw_close(&y))
        return fprintf(stderr, "k93yaftl: WRITE FAILED: check LPN 0 and %u before booting iOS\n", BAK), 1;
    if (read_lpn(0, cur) || read_lpn(BAK, bak) || memcmp(cur, neu, 8192) || memcmp(bak, neu, 8192))
        return fprintf(stderr, "k93yaftl: the new table does NOT read back from both copies\n"), 1;
    printf("the new table is in LPN 0 and LPN %u (superblock %u), read back from both\n", BAK, y.sb);
    return 0;
}

/* ---- the server: iOS's System read-only, LwVM partitions Linux owns read-write through
 * iOS's FTL, and k93lv's Linux volume; what initramfs/init runs ------------------------------
 *
 *   k93yaftl [-j2] serve System Linux:rw Swap:rw
 *
 * /dev/nbd0.. in that order, then the Linux volume (k93lv's) as the last one.  ":rw" is allowed
 * only on partitions Linux owns (Linux, Swap: tools/k93-part.sh made them); everything else is
 * read-only.  A write to a whole 8 KB page goes to the flash before the reply; a 4 KB half waits
 * in that partition's one-page buffer (read with the rest, so the page is whole) until FLUSH, a
 * quiet second, or a write elsewhere -- the NBD device advertises flushes, so the kernel treats it
 * as a volatile write cache.  The first write puts iOS's marker after its saved context (its next
 * boot runs the full restore that adopts these pages); nothing is written before that. */

#define YP_MAX 4
struct ypart {
    struct part *p;
    uint32_t pend_lpn;          /* NONE: no page waiting */
    uint64_t pend_off;          /* the waiting page's offset in the partition */
    uint8_t pend[MAX_PAGE] __attribute__((aligned(8)));
};
static struct ypart YP[YP_MAX];
static int nyp;
static struct yw YW;
static uint64_t yst_rmw, yst_rd;

static uint32_t yp_lpn(const struct part *p, uint64_t off)
{
    uint64_t c = off / lwvm_chunk;
    return (uint32_t)(((uint64_t)p->phys[c] * lwvm_chunk + off % lwvm_chunk) / G.c->page);
}

static int yp_commit(struct ypart *y)
{
    if (y->pend_lpn == NONE)
        return 0;
    if (yw_put(&YW, y->pend_lpn, y->pend))
        return -1;
    y->pend_lpn = NONE;
    return 0;
}

static int yp_read(struct ypart *y, uint64_t off, uint8_t *buf, uint32_t len)
{
    yst_rd += len;
    if (read_part(y->p, off, buf, len))
        return -1;
    uint64_t a = y->pend_off, b = y->pend_off + G.c->page;
    if (y->pend_lpn != NONE && a < off + len && off < b) {
        uint64_t s = a > off ? a : off, e = b < off + len ? b : off + len;
        memcpy(buf + (s - off), y->pend + (s - a), e - s);
    }
    return 0;
}

static int yp_write(struct ypart *y, uint64_t off, const uint8_t *buf, uint32_t len)
{
    uint32_t pg = G.c->page;
    while (len) {
        uint64_t po = off / pg * pg;
        uint32_t in = (uint32_t)(off - po), n = pg - in < len ? pg - in : len;
        uint32_t lpn = yp_lpn(y->p, po);
        if (!in && n == pg) {
            if (y->pend_lpn == lpn)
                y->pend_lpn = NONE;             /* the whole page replaces what waited */
            if (yw_put(&YW, lpn, buf))
                return -1;
        } else {
            if (y->pend_lpn != lpn) {
                if (yp_commit(y) || read_lpn(lpn, y->pend))
                    return -1;
                y->pend_lpn = lpn;
                y->pend_off = po;
                yst_rmw++;
            }
            memcpy(y->pend + in, buf, n);
        }
        off += n;
        buf += n;
        len -= n;
    }
    return 0;
}

#define YP_OPS(i) \
    static int yp_read##i(uint64_t o, uint8_t *b, uint32_t l) { return yp_read(&YP[i], o, b, l); } \
    static int yp_write##i(uint64_t o, const uint8_t *b, uint32_t l) { return yp_write(&YP[i], o, b, l); } \
    static int yp_flush##i(void) { return yp_commit(&YP[i]); } \
    static void yp_idle##i(void) { yp_commit(&YP[i]); if (i == 0) gc_poke(&YW); } \
    static const struct export_ops YP_OPS##i = { yp_read##i, yp_write##i, yp_flush##i, NULL, yp_idle##i };
YP_OPS(0)
YP_OPS(1)
YP_OPS(2)
YP_OPS(3)
static const struct export_ops *const YP_OPSV[YP_MAX] = { &YP_OPS0, &YP_OPS1, &YP_OPS2, &YP_OPS3 };

/* every map block a partition's LPNs need, made now: no allocation on the write path (swap) */
static void yp_prealloc(const struct part *p)
{
    for (uint32_t c = 0; c < p->nchunks; c++) {
        uint32_t l0 = (uint32_t)((uint64_t)p->phys[c] * lwvm_chunk / G.c->page);
        uint32_t n = (uint32_t)(lwvm_chunk / G.c->page);
        for (uint32_t l = l0; l < l0 + n; l += MB_LPNS)
            if (l < NLPN && !l2v[l >> MB_SHIFT]) {
                void *q = mmap(NULL, MB_LPNS * 4, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
                if (q == MAP_FAILED) {
                    fprintf(stderr, "k93yaftl: out of memory for the map\n");
                    exit(1);
                }
                memset(q, 0xff, MB_LPNS * 4);
                l2v[l >> MB_SHIFT] = q;
                nmapblocks++;
            }
    }
}

static int cmd_yserve(char **names, int n)
{
    static struct export ex[12];
    static char *deflt[] = { "System" };
    char base[40];
    int nex = 0, want_rw = 0;
    if (vfl_open(0) || build_map(1) || lwvm_load())
        return 1;
    if (!n) {
        names = deflt;
        n = 1;
    }
    for (int i = 0; i < n; i++)
        want_rw |= strstr(names[i], ":rw") != NULL;
    int rw_ok = want_rw && yw_init(&YW, 1) == 0;
    if (want_rw && !rw_ok)
        fprintf(stderr, "k93yaftl: iOS's FTL state is not as known: every partition read-only this time\n");
    YW.verify = !getenv("K93YAFTL_NOVERIFY");
    for (int i = 0; i < n && nex < 11; i++) {
        snprintf(base, sizeof(base), "%s", names[i]);
        char *colon = strchr(base, ':');
        int rw = colon && !strcmp(colon, ":rw");
        if (colon)
            *colon = 0;
        struct part *p = part_by_name(base);
        if (!p) {
            fprintf(stderr, "k93yaftl: no LwVM partition %s\n", base);
            continue;
        }
        /* System too (cascadia-system mounts it read-write only when its HFS+ journal is empty); Data never:
         * iOS encrypts it file by file */
        if (rw && strcmp(base, "Linux") && strcmp(base, "Swap") && strcmp(base, "System")) {
            fprintf(stderr, "k93yaftl: %s is iOS's: read-only\n", base);
            rw = 0;
        }
        memset(&ex[nex], 0, sizeof(ex[nex]));
        ex[nex].p = p;
        if (rw && rw_ok && nyp < YP_MAX) {
            YP[nyp].p = p;
            YP[nyp].pend_lpn = NONE;
            yp_prealloc(p);
            ex[nex].ops = YP_OPSV[nyp++];
        }
        nex++;
    }
    if (nyp) {                                  /* the GC: what of the flash is ours to move -- not System's */
        for (int i = 0; i < nyp; i++)
            for (uint32_t c = 0; c < YP[i].p->nchunks && strcmp(YP[i].p->name, "System"); c++)
                if (YP[i].p->phys[c] < 1024)
                    rwchunk[YP[i].p->phys[c]] = 1;
        gc_setup();
        gc_dirfd = open("/run/ios", O_RDONLY | O_DIRECTORY);
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
    /* a block device's server: its own pages stay in RAM, and its allocations never wait on I/O
     * (PR_SET_IO_FLUSHER, Linux 5.6) -- swap may be one of its devices */
    if (mlockall(MCL_CURRENT | MCL_FUTURE))
        perror("k93yaftl: mlockall");
#ifndef PR_SET_IO_FLUSHER
#define PR_SET_IO_FLUSHER 57
#endif
    if (prctl(PR_SET_IO_FLUSHER, 1, 0, 0, 0))
        perror("k93yaftl: PR_SET_IO_FLUSHER");
    if (parallel)
        start_worker();
    int r = serve_exports(ex, nex);
    for (int i = 0; i < nyp; i++)
        yp_commit(&YP[i]);
    fprintf(stderr, "k93yaftl: iOS's FTL: %llu pages written in %llu superblock(s), %llu failure(s), %llu partial "
            "pages merged, %llu MB read; %s\n", (unsigned long long)YW.pages, (unsigned long long)YW.sbs,
            (unsigned long long)YW.failed, (unsigned long long)yst_rmw, (unsigned long long)(yst_rd >> 20),
            YW.marked && !YW.C.marked ? "iOS's next boot restores its map" : "iOS's context untouched");
    fprintf(stderr, "k93yaftl: GC: %llu run(s), %llu superblock(s) given back (%llu pages moved), %llu left alone; "
            "%u free superblocks at the end\n", (unsigned long long)gst.runs, (unsigned long long)gst.victims,
            (unsigned long long)gst.moved, (unsigned long long)gst.failed, YW.nfree);
    fprintf(stderr, "k93lv: %llu MB read, %llu MB written (%llu pages), %llu erases, %llu GC passes\n",
            lst.rd >> 20, lst.wr >> 20, lst.pages, lst.erases, lst.gcs);
    return r;
}

/* every written page of SB from FROM: metadata and the first BYTES bytes decrypted (user pages with the
 * user key and their LPN, the others with the META key and their row) */
static int cmd_pages(char **argv, int argc)
{
    static uint8_t dec[MAX_PAGE];
    uint8_t m[META_BYTES];
    uint32_t row;
    if (argc < 1 || vfl_open(0))
        return 1;
    uint32_t sb = strtoul(argv[0], NULL, 0), from = argc > 1 ? strtoul(argv[1], NULL, 0) : 0;
    uint32_t n = argc > 2 ? strtoul(argv[2], NULL, 0) : PPSB, bytes = argc > 3 ? strtoul(argv[3], NULL, 0) : 64;
    for (uint32_t off = from; off < PPSB && off < from + n; off++) {
        uint32_t vpn = sb * PPSB + off;
        int st = read_vpn(vpn, pdata, m, &row);
        if (st == 2) {
            printf("off %4u  erased\n", off);
            continue;
        }
        if (st < 1) {
            printf("off %4u  unreadable %d\n", off, st);
            continue;
        }
        printf("off %4u  lpn %08x usn %08x b8 %02x type %02x\n", off, le32(m), le32(m + 4), m[8], m[9]);
        if (m[9] == T_USER)
            aesf_cbc(DK_USER, le32(m), pdata, dec, G.c->page);
        else
            aesf_cbc(DK_META, row, pdata, dec, G.c->page);
        for (uint32_t o = 0; o < bytes && o < G.c->page; o += 32) {
            char tag[24];
            snprintf(tag, sizeof tag, "    +%04x  ", o);
            hexline(tag, dec + o, bytes - o < 32 ? bytes - o : 32);
        }
    }
    return 0;
}

#ifndef K93YAFTL_SIM                    /* the GC's host test brings its own */
int main(int argc, char **argv)
{
    if (argc == 3 && !strcmp(argv[1], "lwvm-check")) {     /* no hardware: a table file against iOS's check */
        static uint8_t h[8192];
        FILE *f = fopen(argv[2], "rb");
        if (!f || fread(h, 1, sizeof h, f) != sizeof h)
            return fprintf(stderr, "k93yaftl: %s: not an 8192-byte table\n", argv[2]), 1;
        fclose(f);
        const char *why = lwvm_check(h, le64(h + 0x20));
        printf("%s: %s\n", argv[2], why ? why : "passes iOS's check");
        return why != NULL;
    }
    if (argc > 2 && !strcmp(argv[1], "-j2")) {
        parallel = 1;
        argv++;
        argc--;
    }
    if (argc >= 2 && !strcmp(argv[1], "serve")) {
        setvbuf(stdout, NULL, _IOLBF, 0);
        for (unsigned i = 0; i < PCACHE; i++)
            pcache[i].lpn = NONE;
        if (lv_start())
            return 1;
        enc_init();
        return cmd_yserve(argv + 2, argc - 2);
    }
    if (argc < 2 || (strcmp(argv[1], "inspect") && strcmp(argv[1], "pages") && strcmp(argv[1], "ctl") &&
                     strcmp(argv[1], "write-test") && strcmp(argv[1], "check") && strcmp(argv[1], "write-lpns") &&
                     strcmp(argv[1], "lwvm-apply"))) {
        fprintf(stderr, "usage: k93yaftl [-j2] serve [PARTITION[:rw]...] | inspect [SB...] |\n"
                "                pages SB [FROM [N [BYTES]]] | ctl |\n"
                "                write-test START SEED [--dry] [--sb N] | check START SEED |\n"
                "                write-lpns START FILE [--expect OLD] [--dry] | lwvm-apply NEW OLD [--dry]\n");
        return 1;
    }
    trace = 0;
    setvbuf(stdout, NULL, _IOLBF, 0);
    for (unsigned i = 0; i < PCACHE; i++)       /* zeroed entries would claim to hold LPN 0 */
        pcache[i].lpn = NONE;
    whiten_init();
    aes_init();
    aesf_init();
    if (aesf_check())
        return 1;
    open_hw();
    if (setup(1, 0))
        return 1;
    enc_init();
    if (!strcmp(argv[1], "pages"))
        return cmd_pages(argv + 2, argc - 2);
    if (!strcmp(argv[1], "ctl")) {
        struct ctl C;
        return vfl_open(0) || ctl_find(&C, 1);
    }
    if (!strcmp(argv[1], "write-test"))
        return cmd_write_test(argv + 2, argc - 2);
    if (!strcmp(argv[1], "check"))
        return cmd_ycheck(argv + 2, argc - 2);
    if (!strcmp(argv[1], "write-lpns"))
        return cmd_write_lpns(argv + 2, argc - 2);
    if (!strcmp(argv[1], "lwvm-apply"))
        return cmd_lwvm_apply(argv + 2, argc - 2);
    return cmd_inspect(argv + 2, argc - 2);
}
#endif
