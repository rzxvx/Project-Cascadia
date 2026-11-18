/* k93yaftl-sim.c -- tools/nand/k93yaftl.c's write engine and GC on a model of the flash, with iOS's
 * restore (docs/research/k93-nand-write.md section 6) replayed over it at random moments: a power
 * cut there must leave every LPN at the last version written, iOS's own pages where they were.
 * A host test, not shipped.
 *
 * Build and run (from devices/a5):
 *   gcc -O2 -w -pthread -D_FILE_OFFSET_BITS=64 -DK93NAND_SIM -Itools/nand -o k93yaftl-sim \
 *       tools/nand/k93yaftl-sim.c && ./k93yaftl-sim [SEED [WRITES [FAILRATE]]]
 *
 * The model: 48 superblocks of 4 banks x 8 pages (31 data pages + the table of contents), superblock 0
 * never used (the control one); pages programmed once, in order within each bank's block; a program
 * may fail (the page left unreadable), an erase may fail (the bank left as it was).  LwVM chunks of
 * 1024 LPNs: chunk 0 and 3 are iOS's, 1 and 2 the "Linux" and "Swap" written here.  FAILRATE: one
 * program in FAILRATE fails (default 3000; 0: none), one erase in FAILRATE / 4 + 1.  Exit 0 only if
 * no restore at any cut, and no read through the server's map, found a wrong page. */
#define K93YAFTL_SIM
#include "k93yaftl.c"

/* the registers: never touched by this test (K93NAND_SIM leaves these three to the build) */
uint32_t mmio_rd(int bus, int blk, uint32_t off) { fprintf(stderr, "sim: register read\n"); abort(); }
void mmio_wr(int bus, int blk, uint32_t off, uint32_t v) { fprintf(stderr, "sim: register write\n"); abort(); }
uint8_t mmio_rd8(int bus, int blk, uint32_t off) { abort(); }

#define S_NSB   48
#define S_TB    4
#define S_PPSB  32
#define S_NLPN  4096
#define S_PG    8192
#define NPAGES  (S_NSB * S_PPSB)

static uint8_t *sim_data;
static uint8_t sim_meta[NPAGES][12];
static uint8_t sim_st[NPAGES];          /* 0 erased, 1 programmed, 2 torn (a failed program) */
static uint32_t ver[S_NLPN];            /* the last version written of each LPN (0: never by the test) */
static uint32_t amb[S_NLPN];            /* a version whose write failed: it may be there all the same (EIO) */
static uint32_t ios_at[S_NLPN];         /* iOS's pages: where they must stay */
static uint32_t rs = 1, failrate;
static unsigned checks, bad, armed, progs, erases, pfail, efail;

static uint32_t rnd(void)
{
    rs ^= rs << 13;
    rs ^= rs >> 17;
    rs ^= rs << 5;
    return rs;
}

static void sim_check(const char *when);

static void maybe_cut(void)
{
    if (armed && rnd() % 37 == 0)
        sim_check("power cut");
}

static int yv_read(uint32_t vpn, uint8_t *raw, uint8_t m[META_BYTES], uint32_t *rowp)
{
    if (vpn >= NPAGES)
        abort();
    if (rowp)
        *rowp = yv_row(vpn);
    if (sim_st[vpn] == 0) {
        memset(m, 0xff, META_BYTES);
        return 2;
    }
    if (sim_st[vpn] == 2)
        return -0x24;
    memcpy(raw, sim_data + (size_t)vpn * S_PG, S_PG);
    memcpy(m, sim_meta[vpn], META_BYTES);
    return 1;
}

static uint32_t yv_row(uint32_t vpn)
{
    return vpn * 2654435761u >> 7;
}

static int yv_prog(uint32_t vpn, const uint8_t *enc, const uint8_t meta[12])
{
    maybe_cut();
    if (vpn >= NPAGES || sim_st[vpn]) {
        fprintf(stderr, "SIM: program of page %u, %s\n", vpn, vpn >= NPAGES ? "past the flash" : "not erased");
        abort();
    }
    if (vpn % S_PPSB >= S_TB && sim_st[vpn - S_TB] == 0) {
        fprintf(stderr, "SIM: page %u programmed before the one under it in its block\n", vpn);
        abort();
    }
    progs++;
    if (armed && failrate && rnd() % failrate == 0) {
        sim_st[vpn] = 2;
        pfail++;
        return -5;
    }
    sim_st[vpn] = 1;
    memcpy(sim_data + (size_t)vpn * S_PG, enc, S_PG);
    memcpy(sim_meta[vpn], meta, 12);
    return 0;
}

static int yv_erase(uint32_t sb)
{
    for (uint32_t b = 0; b < S_TB; b++) {
        maybe_cut();
        if (armed && failrate && rnd() % (failrate / 4 + 1) == 0) {
            efail++;
            return -5;                          /* this bank (and the rest) as they were */
        }
        for (uint32_t p = b; p < S_PPSB; p += S_TB)
            sim_st[sb * S_PPSB + p] = 0;
    }
    erases++;
    return 0;
}

static int yv_free_ok(uint32_t sb)
{
    if (sb < 1 || sb >= S_NSB)
        return 0;
    for (uint32_t b = 0; b < S_TB; b++)
        if (sim_st[sb * S_PPSB + b])
            return 0;
    return 1;
}

/* iOS's restore over the model: superblocks by the usn of their first readable page, newest first;
 * the table of contents if it reads, else the newest up to its first hole, the others page by page;
 * the first sighting of an LPN wins */
static void sim_restore(uint32_t *map)
{
    static uint32_t toc[S_PG / 4];
    struct sbrec list[S_NSB];
    unsigned n = 0;
    for (uint32_t i = 0; i < S_NLPN; i++)
        map[i] = NONE;
    for (uint32_t sb = 1; sb < S_NSB; sb++) {
        int st = -1;
        uint32_t p;
        for (p = 0; p + 1 < S_PPSB; p++) {
            st = sim_st[sb * S_PPSB + p];
            if (st != 2)
                break;                          /* torn: unreadable, look further */
        }
        if (st != 1)
            continue;                           /* erased (free) or nothing readable */
        const uint8_t *m = sim_meta[sb * S_PPSB + p];
        if (m[9] != T_USER)
            continue;
        list[n++] = (struct sbrec){ sb, le32(m + 4), m[9] };
    }
    qsort(list, n, sizeof(list[0]), by_usn_desc);
    for (unsigned i = 1; i < n; i++)
        if (list[i].usn == list[i - 1].usn) {
            printf("SIM: two superblocks (%u, %u) with weaveSeq %08x\n", list[i - 1].sb, list[i].sb, list[i].usn);
            bad++;
        }
    for (unsigned i = 0; i < n; i++) {
        uint32_t base = list[i].sb * S_PPSB, last = base + S_PPSB - 1;
        if (sim_st[last] == 1 && sim_meta[last][9] == T_BTOC) {
            aesf_cbc(DK_META, yv_row(last), sim_data + (size_t)last * S_PG, (uint8_t *)toc, S_PG);
            for (int off = S_PPSB - 2; off >= 0; off--)
                if (toc[off] < S_NLPN && map[toc[off]] == NONE)
                    map[toc[off]] = base + off;
        } else if (i == 0) {
            uint32_t wp = 0;
            while (wp + 1 < S_PPSB && sim_st[base + wp] == 1 && sim_meta[base + wp][9] == T_USER)
                wp++;
            for (int off = (int)wp - 1; off >= 0; off--) {
                uint32_t l = le32(sim_meta[base + off]);
                if (l < S_NLPN && map[l] == NONE)
                    map[l] = base + off;
            }
        } else {
            for (int off = S_PPSB - 2; off >= 0; off--)
                if (sim_st[base + off] == 1 && sim_meta[base + off][9] == T_USER) {
                    uint32_t l = le32(sim_meta[base + off]);
                    if (l < S_NLPN && map[l] == NONE)
                        map[l] = base + off;
                }
        }
    }
}

/* the version a page holds: its first 16 bytes decrypted */
static int page_version(uint32_t vpn, uint32_t lpn, uint32_t *v)
{
    uint8_t m[META_BYTES], raw[S_PG], first[16];
    if (yv_read(vpn, raw, m, NULL) != 1 || m[9] != T_USER || le32(m) != lpn)
        return -1;
    aesf_cbc(DK_USER, lpn, raw, first, 16);
    if (memcmp(first, "GCSM", 4) || le32(first + 4) != lpn)
        return -1;
    *v = le32(first + 8);
    return 0;
}

static void sim_check(const char *when)
{
    static uint32_t map[S_NLPN];
    int was = armed;
    armed = 0;                                  /* no cuts inside the check */
    checks++;
    sim_restore(map);
    for (uint32_t l = 0; l < S_NLPN; l++) {
        uint32_t v;
        if (ios_at[l] != NONE && map[l] != ios_at[l]) {
            if (bad++ < 10)
                printf("SIM (%s, check %u): iOS's LPN %u restored at %u, not %u\n", when, checks, l, map[l], ios_at[l]);
        } else if (ver[l] && (map[l] == NONE || page_version(map[l], l, &v) || (v != ver[l] && v != amb[l]))) {
            if (bad++ < 10)
                printf("SIM (%s, check %u): LPN %u restored as version %d (vpn %d), written %u\n", when, checks, l,
                       map[l] == NONE ? -1 : page_version(map[l], l, &v) ? -2 : (int)v, (int)map[l], ver[l]);
        }
        /* and the server's own map: what Linux reads */
        if (ver[l] && (l2v_get(l) == NONE || page_version(l2v_get(l), l, &v) || (v != ver[l] && v != amb[l]))) {
            if (bad++ < 10)
                printf("SIM (%s, check %u): LPN %u reads as the wrong version through the server's map\n", when,
                       checks, l);
        }
    }
    armed = was;
}

static void fill_page(uint8_t *p, uint32_t lpn, uint32_t v)
{
    memset(p, 0, S_PG);
    memcpy(p, "GCSM", 4);
    memcpy(p + 4, &lpn, 4);
    memcpy(p + 8, &v, 4);
    for (unsigned i = 16; i < S_PG; i += 4) {
        uint32_t x = lpn * 2654435761u ^ v * 40503u ^ i;
        memcpy(p + i, &x, 4);
    }
}

/* a superblock as someone else left it: LPNS[i] at page i, its table of contents, weaveSeq USN */
static void prefill(uint32_t sb, const uint32_t *lpns, unsigned n, uint32_t usn, int ios)
{
    static uint8_t plain[S_PG], enc[S_PG];
    uint32_t toc[S_PG / 4];
    uint8_t meta[12];
    memset(toc, 0xff, sizeof(toc));
    for (unsigned i = 0; i < n; i++) {
        uint32_t vpn = sb * S_PPSB + i, l = lpns[i];
        if (!ios)
            ver[l]++;
        fill_page(plain, l, ios ? 0 : ver[l]);
        aese_cbc(EK_USER, l, plain, enc, S_PG);
        put_meta(meta, l, usn, T_USER);
        yv_prog(vpn, enc, meta);
        toc[i] = l;
        if (ios)
            ios_at[l] = vpn;
    }
    uint32_t last = sb * S_PPSB + S_PPSB - 1;
    for (unsigned i = n; i + 1 < S_PPSB; i++) {      /* (a short one: pad with iOS-style FF pages? no --
                                                        the table is written only when full; fill the rest) */
        uint32_t vpn = sb * S_PPSB + i, l = lpns[i % n];
        if (!ios)
            ver[l]++;
        fill_page(plain, l, ios ? 0 : ver[l]);
        aese_cbc(EK_USER, l, plain, enc, S_PG);
        put_meta(meta, l, usn, T_USER);
        yv_prog(vpn, enc, meta);
        toc[i] = l;
        if (ios)
            ios_at[l] = vpn;
    }
    aese_cbc(EK_META, yv_row(last), (uint8_t *)toc, enc, S_PG);
    put_meta(meta, 0, 0, T_BTOC);
    yv_prog(last, enc, meta);
}

int main(int argc, char **argv)
{
    static struct chip simchip;
    static uint8_t plain[S_PG];
    static uint32_t map[S_NLPN];
    static uint32_t lpns[S_PPSB];
    struct yw y;
    uint32_t seed = argc > 1 ? strtoul(argv[1], NULL, 0) : 1;
    uint32_t writes = argc > 2 ? strtoul(argv[2], NULL, 0) : 20000;
    failrate = argc > 3 ? strtoul(argv[3], NULL, 0) : 3000;
    rs = seed * 2654435761u | 1;
    setvbuf(stdout, NULL, _IOLBF, 0);
    whiten_init();
    aes_init();
    aesf_init();
    if (aesf_check())
        return 1;
    enc_init();
    simchip.page = S_PG;
    simchip.ppb = S_PPSB / S_TB;
    simchip.blocks = S_NSB;
    G.c = &simchip;
    NCE = 0;                                    /* is_ctrl(): no control superblocks in the model */
    TB = S_TB;
    PPSB = S_PPSB;
    NSB = S_NSB;
    NLPN = S_NLPN;
    NMB = (NLPN + MB_LPNS - 1) >> MB_SHIFT;
    l2v = calloc(NMB, sizeof(*l2v));
    for (unsigned i = 0; i < PCACHE; i++)
        pcache[i].lpn = NONE;
    lwvm_chunk = (uint64_t)1024 * S_PG;
    rwchunk[1] = rwchunk[2] = 1;
    sim_data = calloc(NPAGES, S_PG);
    for (uint32_t i = 0; i < S_NLPN; i++)
        ios_at[i] = NONE;

    /* before the session: iOS's superblocks 40..43 (chunk 0 and 3), an older Linux session's 30..35
     * (its later ones overwriting part of its earlier ones), one superblock mixing both (36) */
    for (uint32_t k = 0; k < 4; k++) {
        for (unsigned i = 0; i < S_PPSB - 1; i++)
            lpns[i] = (k < 2 ? 0 : 3072) + (k % 2) * 31 + i;
        prefill(40 + k, lpns, S_PPSB - 1, 100 + 16 * k, 1);
    }
    for (uint32_t k = 0; k < 6; k++) {
        for (unsigned i = 0; i < S_PPSB - 1; i++)
            lpns[i] = 1024 + (k % 3) * 31 + i * (k < 3 ? 1 : 2) % 93;
        prefill(30 + k, lpns, S_PPSB - 1, 200 + 16 * k, 0);
    }
    for (unsigned i = 0; i < S_PPSB - 1; i++)
        lpns[i] = i % 2 ? 62 + i : 2048 + 100 + i;
    prefill(36, lpns, S_PPSB - 1, 300, 0);
    for (unsigned i = 0; i < S_PPSB - 1; i++)       /* the mixed one's iOS pages are iOS's */
        if (i % 2) {
            ios_at[62 + i] = 36 * S_PPSB + i;
            ver[62 + i] = 0;
        }

    /* the server's start: the map as iOS's restore makes it, then the GC's view */
    sim_restore(map);
    for (uint32_t l = 0; l < S_NLPN; l++)
        if (map[l] != NONE)
            l2v_set(l, map[l]);
    memset(&y, 0, sizeof(y));
    y.sb = NONE;
    y.last_sb = NSB;
    y.verify = 1;
    y.marked = 1;
    y.base = 400;
    sbkind = calloc(NSB, 1);
    for (uint32_t sb = 1; sb < S_NSB; sb++) {
        if (yv_free_ok(sb))
            y.nfree++, sbkind[sb] = 1;
        else if (sim_st[sb * S_PPSB] == 1 && sim_meta[sb * S_PPSB][9] == T_USER)
            sbkind[sb] = 2;
    }
    yw_keep = 3;
    gc_low = 8;
    setenv("K93YAFTL_GC_LOW", "0", 1);
    gc_setup();
    unsigned cand = 0;
    for (uint32_t sb = 0; sb < S_NSB; sb++)
        cand += sbstate[sb] == SB_CLOSED;
    printf("start: %u free superblocks, %u GC candidates (36, mixed, must not be one: %s)\n", y.nfree, cand,
           sbstate[36] ? "IS ONE" : "ok");
    if (sbstate[36] || sbstate[40] || sbstate[41] || sbstate[42] || sbstate[43])
        bad++;
    sim_check("start");

    /* the session: writes over 2 x 300 LPNs, a hot 2 x 120 of them taking most */
    armed = 1;
    unsigned ok = 0, eio = 0;
    for (uint32_t w = 0; w < writes; w++) {
        uint32_t l = (rnd() % 5 ? 1024 + rnd() % 120 : 1024 + rnd() % 300) + (rnd() & 1) * 1024;
        fill_page(plain, l, ver[l] + 1);
        amb[l] = ver[l] + 1;                    /* in flight: a cut now may find either */
        int r = yw_put(&y, l, plain);
        if (r) {                                /* the kernel would see EIO: the write did not happen... */
            eio++;
            r = yw_put(&y, l, plain);           /* ...and the next one tries again */
        }
        if (r == 0) {
            ver[l]++;
            amb[l] = 0;
            ok++;
        } else {
            amb[l] = ver[l] + 1;
            eio++;
        }
    }
    armed = 0;
    sim_check("end");
    printf("seed %u: %u writes ok, %u failed (EIO); %llu superblocks filled; GC %llu runs, %llu given back "
           "(%llu pages moved), %llu left alone; %u programs (%u failed), %u erases (%u failed); %u free at the end; "
           "%u restore checks, %u problems\n", seed, ok, eio, (unsigned long long)y.sbs, (unsigned long long)gst.runs,
           (unsigned long long)gst.victims, (unsigned long long)gst.moved, (unsigned long long)gst.failed, progs,
           pfail, erases, efail, y.nfree, checks, bad);
    return bad != 0;
}
