/* iosnand -- the iPad's own iOS partitions, read-only, as Linux block devices.
 *
 * NAND -> PPN pages (ppn.c, PIO) -> the FTL's map -> disk0 -> LwVM -> NBD.
 *
 * The FTL (AppleSwissPPNFTL) keeps 16 bytes of metadata with every 4 KB it
 * writes: type (1 = user data), a 48-bit write sequence, the LBA.  The live
 * copy of an LBA is the one with the highest sequence, so the map comes from
 * the metadata alone; the FTL's own context is never parsed
 * (docs/research/p105-nand.md).  Reading every page's metadata takes a quarter
 * of an hour by PIO, so the page table is cached, and a later start only
 * reads the first page of every block: a block the FTL erased and wrote again
 * has a new sequence there, and a block that was still being filled is
 * checked at its old write pointer.  Only those blocks are read again.
 *
 *   iosnand scan               bring the cache up to date, print what changed
 *   iosnand parts              the LwVM partitions on disk0
 *   iosnand read LBA [N]       N x 4 KB of disk0 to stdout
 *   iosnand serve [NAME...]    attach LwVM partitions (default: System) to
 *                              /dev/nbd0, /dev/nbd1 ... and serve them until
 *                              SIGTERM; read-only, writes are refused
 *
 * The cache is /var/lib/iosnand/pages.v1 (IOSNAND_CACHE overrides).  Both NAND
 * buses stay locked while iosnand runs.  Build:
 *   arm-linux-gnueabihf-gcc -static -O2 -D_FILE_OFFSET_BITS=64 -o iosnand \
 *       tools/nand/iosnand.c tools/nand/ppn.c
 */
#include <arpa/inet.h>
#include <endian.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/nbd.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "ppn.h"

#define NLBA            (16000000000ull / 4096)         /* disk0, in 4 KB */
#define NONE            0xffffffffu
#define META_LEN        (3 * PPN_CHUNK + PPN_META_OFF + 16)     /* through chunk 3's metadata */
#define FIRST_BLOCK     1       /* block 0: boot pages on CAU0, retired on CAU1 */

#define LWVM_CHUNK      (16ull << 20)
#define LWVM_HDR        0xf000
#define LWVM_FREE       0xf3ff

enum { B_UNKNOWN, B_ERASED, B_DATA, B_BAD };

struct block_rec {
    uint64_t head_seq;          /* sequence of page 0, chunk 0 */
    uint16_t written;           /* pages 0..written-1 are programmed */
    uint8_t state;
    uint8_t pad[5];
};

struct page_rec {
    uint64_t seq;               /* chunk 0's; chunk k is seq + k; 0 = none */
    uint32_t lba[4];            /* NONE unless user data */
};

struct cache_hdr {
    char magic[8];              /* "IOSNAND1" */
    uint32_t dies, blocks, pages, rsvd;
};

static struct block_rec blocks[PPN_DIES][PPN_BLOCKS];
static struct page_rec (*pages)[PPN_BLOCKS][PPN_PAGES];
static uint32_t *l2p;           /* LBA -> ((die * blocks + blk) * pages + pg) * 4 + chunk */
static const char *cache_path = "/var/lib/iosnand/pages.v1";

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000ull + ts.tv_nsec / 1000000;
}

static uint64_t meta_seq(const uint8_t *m)
{
    uint64_t s = 0;
    for (int i = 7; i >= 2; i--)
        s = s << 8 | m[i];
    return s;
}

static uint32_t meta_lba(const uint8_t *m)
{
    return m[0] == 1 ? (uint32_t)m[8] | m[9] << 8 | m[10] << 16 | (uint32_t)m[11] << 24 : NONE;
}

/* ---- the page table cache ---------------------------------------------- */

static int cache_load(void)
{
    struct cache_hdr h;
    FILE *f = fopen(cache_path, "rb");
    if (!f)
        return -1;
    if (fread(&h, sizeof(h), 1, f) != 1 || memcmp(h.magic, "IOSNAND1", 8) ||
        h.dies != PPN_DIES || h.blocks != PPN_BLOCKS || h.pages != PPN_PAGES ||
        fread(blocks, sizeof(blocks), 1, f) != 1 ||
        fread(pages, sizeof(*pages) * PPN_DIES, 1, f) != 1) {
        fclose(f);
        fprintf(stderr, "iosnand: %s unusable, starting over\n", cache_path);
        memset(blocks, 0, sizeof(blocks));
        return -1;
    }
    fclose(f);
    return 0;
}

static void cache_save(void)
{
    struct cache_hdr h = { "IOSNAND1", PPN_DIES, PPN_BLOCKS, PPN_PAGES, 0 };
    char tmp[256], dir[256];
    snprintf(dir, sizeof(dir), "%s", cache_path);
    char *slash = strrchr(dir, '/');
    if (slash) {
        *slash = 0;
        mkdir(dir, 0755);
    }
    snprintf(tmp, sizeof(tmp), "%s.tmp", cache_path);
    FILE *f = fopen(tmp, "wb");
    if (!f || fwrite(&h, sizeof(h), 1, f) != 1 || fwrite(blocks, sizeof(blocks), 1, f) != 1 ||
        fwrite(pages, sizeof(*pages) * PPN_DIES, 1, f) != 1 || fclose(f)) {
        perror(tmp);
        return;
    }
    rename(tmp, cache_path);
}

/* ---- scanning ------------------------------------------------------------ */

static uint8_t pbuf[PPN_PAGE_RAW] __attribute__((aligned(4)));

static int read_raw(int die, int blk, int pg, uint32_t len, uint8_t *st)
{
    if (ppn_read_page(die >> 1, 0, PPN_ROW(die & 1, blk, pg), pbuf, len, st)) {
        fprintf(stderr, "iosnand: die %d block %d page %d: read failed (status 0x%02x)\n",
                die, blk, pg, *st);
        return -1;
    }
    return 0;
}

static unsigned seq_gaps;

/* pages from..255 of a block; returns -1 on a bus failure */
static int scan_block(int die, int blk, int from)
{
    struct block_rec *b = &blocks[die][blk];
    for (int pg = from; pg < PPN_PAGES; pg++) {
        struct page_rec *p = &pages[die][blk][pg];
        uint8_t st;
        if (read_raw(die, blk, pg, META_LEN, &st))
            return -1;
        if (st == PPN_ST_ERASED) {
            b->written = pg;
            for (; pg < PPN_PAGES; pg++)
                memset(&pages[die][blk][pg], 0, sizeof(struct page_rec));
            return 0;
        }
        p->seq = 0;
        for (int k = 0; k < 4; k++)
            p->lba[k] = NONE;
        if (!PPN_ST_OK(st))
            continue;           /* unreadable page: nothing from it */
        const uint8_t *m0 = pbuf + PPN_META_OFF;
        p->seq = meta_seq(m0);
        for (int k = 0; k < 4; k++) {
            const uint8_t *m = pbuf + k * PPN_CHUNK + PPN_META_OFF;
            p->lba[k] = meta_lba(m);
            if ((m[0] == 1 || m[0] == 2) && meta_seq(m) != p->seq + k && !seq_gaps++)
                fprintf(stderr, "iosnand: die %d block %d page %d: chunk %d out of sequence\n",
                        die, blk, pg, k);
        }
    }
    b->written = PPN_PAGES;
    return 0;
}

/* Bring blocks[] / pages[] in line with the NAND.  Returns the number of
 * blocks that had to be read again, or -1. */
static int scan(int verbose)
{
    int rescanned = 0, erased = 0, bad = 0, kept = 0;
    uint64_t t0 = now_ms();
    for (int die = 0; die < PPN_DIES; die++) {
        for (int blk = FIRST_BLOCK; blk < PPN_BLOCKS; blk++) {
            struct block_rec *b = &blocks[die][blk];
            uint8_t st;
            if (read_raw(die, blk, 0, PPN_META_OFF + 16, &st))
                return -1;
            if (st == PPN_ST_ERASED) {
                if (b->state != B_ERASED)
                    memset(pages[die][blk], 0, sizeof(pages[die][blk]));
                b->state = B_ERASED;
                b->head_seq = 0;
                b->written = 0;
                erased++;
                continue;
            }
            if (!PPN_ST_OK(st)) {
                b->state = B_BAD;
                bad++;
                continue;
            }
            uint64_t head = meta_seq(pbuf + PPN_META_OFF);
            int from = 0;
            if (b->state == B_DATA && b->head_seq == head) {
                if (b->written == PPN_PAGES) {
                    kept++;
                    continue;
                }
                /* still being filled last time: anything past the old end? */
                if (read_raw(die, blk, b->written, PPN_META_OFF + 16, &st))
                    return -1;
                if (st == PPN_ST_ERASED) {
                    kept++;
                    continue;
                }
                from = b->written;
            }
            b->state = B_DATA;
            b->head_seq = head;
            if (scan_block(die, blk, from))
                return -1;
            rescanned++;
            if (verbose && rescanned % 16 == 0)
                fprintf(stderr, "iosnand: die %d block %4d, %d blocks read, %llu s\n",
                        die, blk, rescanned, (unsigned long long)(now_ms() - t0) / 1000);
        }
        if (rescanned)
            cache_save();       /* a first scan that is cut short resumes here */
    }
    fprintf(stderr, "iosnand: %d blocks read again, %d unchanged, %d erased, %d retired, %.1f s%s\n",
            rescanned, kept, erased, bad, (now_ms() - t0) / 1000.0,
            seq_gaps ? " (chunks out of sequence inside a page: see above)" : "");
    return rescanned;
}

static void build_l2p(void)
{
    uint64_t *best = calloc(NLBA, sizeof(uint64_t));
    l2p = malloc(NLBA * sizeof(uint32_t));
    if (!best || !l2p) {
        fprintf(stderr, "iosnand: out of memory\n");
        exit(1);
    }
    memset(l2p, 0xff, NLBA * sizeof(uint32_t));
    uint32_t mapped = 0;
    for (int die = 0; die < PPN_DIES; die++)
        for (int blk = 0; blk < PPN_BLOCKS; blk++) {
            if (blocks[die][blk].state != B_DATA)
                continue;
            for (int pg = 0; pg < blocks[die][blk].written; pg++) {
                struct page_rec *p = &pages[die][blk][pg];
                for (int k = 0; k < 4; k++) {
                    uint32_t lba = p->lba[k];
                    if (lba >= NLBA || p->seq + k <= best[lba])
                        continue;
                    if (!best[lba])
                        mapped++;
                    best[lba] = p->seq + k;
                    l2p[lba] = (((uint32_t)die * PPN_BLOCKS + blk) * PPN_PAGES + pg) * 4 + k;
                }
            }
        }
    free(best);
    fprintf(stderr, "iosnand: %u of %llu LBAs mapped\n", mapped, (unsigned long long)NLBA);
}

/* ---- reading disk0 ------------------------------------------------------- */

#define PCACHE 8
static struct { uint32_t key; uint8_t data[PPN_PAGE_RAW]; } pcache[PCACHE] __attribute__((aligned(4)));
static unsigned pcache_next;
static unsigned verify_fail;

/* 4 KB of disk0; unmapped LBAs read as zeros.  -1 on a NAND failure. */
static int read_lba(uint32_t lba, uint8_t *out)
{
    uint32_t loc = lba < NLBA ? l2p[lba] : NONE;
    if (loc == NONE) {
        memset(out, 0, 4096);
        return 0;
    }
    uint32_t key = loc >> 2, k = loc & 3;
    uint8_t *d = NULL;
    for (int i = 0; i < PCACHE; i++)
        if (pcache[i].key == key + 1)
            d = pcache[i].data;
    if (!d) {
        int die = key / (PPN_BLOCKS * PPN_PAGES), blk = key / PPN_PAGES % PPN_BLOCKS, pg = key % PPN_PAGES;
        unsigned slot = pcache_next++ % PCACHE;
        uint8_t st;
        pcache[slot].key = 0;
        if (ppn_read_page(die >> 1, 0, PPN_ROW(die & 1, blk, pg), pcache[slot].data, PPN_PAGE_RAW, &st) ||
            !PPN_ST_OK(st)) {
            fprintf(stderr, "iosnand: LBA %u: die %d block %d page %d read failed (0x%02x)\n",
                    lba, die, blk, pg, st);
            return -1;
        }
        pcache[slot].key = key + 1;
        d = pcache[slot].data;
    }
    const uint8_t *c = d + k * PPN_CHUNK;
    if (meta_lba(c + PPN_META_OFF) != lba) {
        /* the FTL moved it since the scan: s_verify_meta's "lba mismatch" */
        if (verify_fail++ < 10)
            fprintf(stderr, "iosnand: LBA %u moved since the scan (rescan with iosnand scan)\n", lba);
        return -1;
    }
    memcpy(out, c, PPN_META_OFF);
    memcpy(out + PPN_META_OFF, c + PPN_META_OFF + 16, 4096 - PPN_META_OFF);
    return 0;
}

/* ---- LwVM ---------------------------------------------------------------- */

struct part {
    char name[40];
    uint64_t size;              /* bytes */
    uint32_t nchunks;
    uint16_t phys[1024];        /* partition chunk -> disk0 chunk */
};
static struct part parts[12];
static int nparts;

static int lwvm_load(void)
{
    uint8_t h[4096];
    static const uint8_t magic[16] = { 0x6a, 0x90, 0x88, 0xcf, 0x8a, 0xfd, 0x63, 0x0a,
                                       0xe3, 0x51, 0xe2, 0x48, 0x87, 0xe0, 0xb9, 0x8b };
    if (read_lba(0, h) || memcmp(h, magic, 16)) {
        fprintf(stderr, "iosnand: no LwVM header at LBA 0\n");
        return -1;
    }
    uint32_t n;
    memcpy(&n, h + 0x28, 4);
    const uint8_t *map = h + 0x800;
    for (uint32_t i = 0; i < n && i < 12; i++) {
        struct part *p = &parts[i];
        const uint8_t *r = h + 0x200 + i * 0x80;
        uint64_t begin, end;
        memcpy(&begin, r + 32, 8);
        memcpy(&end, r + 40, 8);
        for (int c = 0; c < 36; c++)        /* UTF-16LE, ASCII in practice */
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
        if (p->size > p->nchunks * LWVM_CHUNK)
            p->size = p->nchunks * LWVM_CHUNK;
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

/* len bytes of a partition from off, any alignment */
static int read_part(struct part *p, uint64_t off, uint8_t *out, uint32_t len)
{
    uint8_t b[4096];
    while (len) {
        uint64_t c = off / LWVM_CHUNK;
        if (c >= p->nchunks)
            return -1;
        uint64_t d0 = p->phys[c] * LWVM_CHUNK + off % LWVM_CHUNK;
        uint32_t in = d0 % 4096, n = 4096 - in < len ? 4096 - in : len;
        if (read_lba(d0 / 4096, b))
            return -1;
        memcpy(out, b + in, n);
        out += n;
        off += n;
        len -= n;
    }
    return 0;
}

/* ---- NBD ----------------------------------------------------------------- */

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

struct export {
    struct part *p;
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
    if (ioctl(e->nbd, NBD_SET_BLKSIZE, 4096UL) ||
        ioctl(e->nbd, NBD_SET_SIZE_BLOCKS, (unsigned long)(e->p->size / 4096)) ||
        ioctl(e->nbd, NBD_SET_FLAGS, (unsigned long)(NBD_FLAG_HAS_FLAGS | NBD_FLAG_READ_ONLY)) ||
        ioctl(e->nbd, NBD_SET_SOCK, sv[0])) {
        perror("nbd ioctl");
        return -1;
    }
    e->child = fork();
    if (e->child == 0) {
        /* NBD_DO_IT runs the device until it is disconnected */
        close(sv[1]);
        ioctl(e->nbd, NBD_DO_IT);
        ioctl(e->nbd, NBD_CLEAR_QUE);
        ioctl(e->nbd, NBD_CLEAR_SOCK);
        _exit(0);
    }
    close(sv[0]);
    e->sock = sv[1];
    fprintf(stderr, "iosnand: %s = %s, %llu MB, read-only\n", e->dev, e->p->name,
            (unsigned long long)(e->p->size >> 20));
    return 0;
}

/* one request; -1 when the device went away */
static int serve_one(struct export *e)
{
    static uint8_t sbuf[1 << 20];
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
        else if (from + len > e->p->size || read_part(e->p, from, buf, len))
            rp.error = htonl(EIO);
        if (write_full(e->sock, &rp, sizeof(rp)) || (!rp.error && write_full(e->sock, buf, len)))
            r = -1;
        if (buf != sbuf)
            free(buf);
        return r;
    }
    case NBD_CMD_DISC:
        return -1;
    case NBD_CMD_WRITE:
        for (uint32_t left = len; left; ) {     /* swallow the data, refuse */
            uint32_t n = left < sizeof(sbuf) ? left : sizeof(sbuf);
            if (read_full(e->sock, buf, n))
                return -1;
            left -= n;
        }
        rp.error = htonl(EPERM);
        break;
    case NBD_CMD_FLUSH:
        break;
    default:
        rp.error = htonl(EPERM);
    }
    return write_full(e->sock, &rp, sizeof(rp));
}

static int serve(char **names, int n)
{
    static char *deflt[] = { "System" };
    struct export ex[12];
    int nex = 0;
    if (!n) {
        names = deflt;
        n = 1;
    }
    for (int i = 0; i < n && nex < 12; i++) {
        struct part *p = part_by_name(names[i]);
        if (!p) {
            fprintf(stderr, "iosnand: no LwVM partition %s\n", names[i]);
            continue;
        }
        ex[nex].p = p;
        if (attach(&ex[nex], nex))
            return 1;
        nex++;
    }
    if (!nex)
        return 1;
    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);
    signal(SIGPIPE, SIG_IGN);
    struct pollfd pf[12];
    while (!stop) {
        for (int i = 0; i < nex; i++)
            pf[i] = (struct pollfd){ ex[i].sock, POLLIN, 0 };
        if (poll(pf, nex, 1000) < 0)
            continue;
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
    for (int i = 0; i < nex; i++) {
        ioctl(ex[i].nbd, NBD_DISCONNECT);
        if (ex[i].sock >= 0)
            close(ex[i].sock);
        waitpid(ex[i].child, NULL, 0);
    }
    fprintf(stderr, "iosnand: stopped\n");
    return 0;
}

/* ---- main ---------------------------------------------------------------- */

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: iosnand scan | parts | read LBA [N] | serve [PARTITION...]\n");
        return 1;
    }
    if (getenv("IOSNAND_CACHE"))
        cache_path = getenv("IOSNAND_CACHE");
    pages = calloc(PPN_DIES, sizeof(*pages));
    if (!pages) {
        fprintf(stderr, "iosnand: out of memory\n");
        return 1;
    }
    ppn_open();
    for (int bus = 0; bus < PPN_BUSES; bus++)
        if (!ppn_claim(bus))
            return 1;
    if (cache_load())
        fprintf(stderr, "iosnand: no page cache yet: reading every page once, ~15 minutes\n");
    int changed = scan(1);
    if (changed < 0)
        return 1;
    if (changed)
        cache_save();
    if (!strcmp(argv[1], "scan"))
        return 0;
    build_l2p();
    if (lwvm_load())
        return 1;
    if (!strcmp(argv[1], "parts")) {
        for (int i = 0; i < nparts; i++)
            printf("%-10s %6llu MB  %u chunks\n", parts[i].name,
                   (unsigned long long)(parts[i].size >> 20), parts[i].nchunks);
        return 0;
    }
    if (!strcmp(argv[1], "read") && argc >= 3) {
        uint32_t lba = strtoul(argv[2], NULL, 0), n = argc > 3 ? strtoul(argv[3], NULL, 0) : 1;
        uint8_t b[4096];
        for (uint32_t i = 0; i < n; i++)
            if (read_lba(lba + i, b) || write_full(1, b, 4096))
                return 1;
        return 0;
    }
    if (!strcmp(argv[1], "serve"))
        return serve(argv + 2, argc - 2);
    fprintf(stderr, "iosnand: unknown command %s\n", argv[1]);
    return 1;
}
