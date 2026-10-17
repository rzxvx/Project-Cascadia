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
 * The cache is /var/lib/iosnand/pages.v2 (IOSNAND_CACHE overrides).  Both NAND
 * buses stay locked while iosnand runs.  IOSNAND_BACKEND=mtd explicitly opts
 * into the read-only kernel driver instead of /dev/mem; the default is pio.
 * MTD failures never fall back to PIO.  MTD mode scans every page each time,
 * because the MTD API does not expose the physical PPN erased status. Build:
 *   arm-linux-gnueabihf-gcc -static -O2 -D_FILE_OFFSET_BITS=64 -o iosnand \
 *       tools/nand/iosnand.c tools/nand/ppn.c
 */
#include <arpa/inet.h>
#include <dirent.h>
#include <endian.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/nbd.h>
#include <limits.h>
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
#include "iosnand-mtd.h"
#include "iosnand-process.h"
static int use_mtd;
static const char *process_record_path;

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
    uint8_t seq[4][6];          /* exact little-endian u48 for each chunk */
    uint32_t lba[4];            /* NONE unless user data; sequence 0 is valid */
};

struct cache_hdr {
    char magic[8];              /* "IOSNAND2": v1 assumed seq+k and cannot migrate */
    uint32_t dies, blocks, pages, record_size;
    uint64_t payload_size;
    uint32_t crc32, rsvd;
};

_Static_assert(sizeof(struct page_rec) == 40, "cache v2 page ABI");
_Static_assert(sizeof(struct block_rec) == 16, "cache v2 block ABI");
_Static_assert(sizeof(struct cache_hdr) == 40, "cache v2 header ABI");
_Static_assert(__BYTE_ORDER == __LITTLE_ENDIAN, "cache format is little-endian");

static struct block_rec blocks[PPN_DIES][PPN_BLOCKS];
static struct page_rec (*pages)[PPN_BLOCKS][PPN_PAGES];
static uint32_t *l2p;           /* LBA -> ((die * blocks + blk) * pages + pg) * 4 + chunk */
static const char *cache_path = "/var/lib/iosnand/pages.v2";

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

static uint64_t page_seq(const struct page_rec *p, unsigned chunk)
{
    uint64_t s = 0;
    for (int i = 5; i >= 0; i--)
        s = s << 8 | p->seq[chunk][i];
    return s;
}

/* ---- the page table cache ---------------------------------------------- */

static int cache_records_valid(void)
{
    for (int die = 0; die < PPN_DIES; die++)
        for (int blk = 0; blk < PPN_BLOCKS; blk++) {
            const struct block_rec *b = &blocks[die][blk];
            /* written is consumed as an array bound by build_l2p(). */
            if (b->written > PPN_PAGES || b->state > B_BAD || b->head_seq >= (1ull << 48))
                return 0;
        }
    return 1;
}

/* IEEE CRC32 detects accidental payload corruption, not malicious edits.
 * It covers every record, including pages outside a block's write pointer. */
static uint32_t cache_crc32(void)
{
    uint32_t table[256];
    for (unsigned i = 0; i < 256; i++) {
        uint32_t c = i;
        for (unsigned bit = 0; bit < 8; bit++)
            c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1)));
        table[i] = c;
    }
    uint32_t crc = UINT32_MAX;
    const uint8_t *data[] = { (const void *)blocks, (const void *)pages };
    const size_t len[] = { sizeof(blocks), sizeof(*pages) * PPN_DIES };
    for (unsigned part = 0; part < 2; part++)
        for (size_t i = 0; i < len[part]; i++)
            crc = table[(crc ^ data[part][i]) & 255] ^ (crc >> 8);
    return ~crc;
}

/* Only regular cache files are allowed. Reject other paths before touching
 * a device backend; in particular an existing FIFO must not block startup. */
static FILE *cache_open(void)
{
    struct stat before, after;
    if (lstat(cache_path, &before))
        return NULL;
    if (!S_ISREG(before.st_mode)) {
        errno = EINVAL;
        return NULL;
    }
    int fd = open(cache_path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0)
        return NULL;
    if (fstat(fd, &after) || !S_ISREG(after.st_mode) ||
        before.st_dev != after.st_dev || before.st_ino != after.st_ino) {
        close(fd);
        errno = EINVAL;
        return NULL;
    }
    FILE *f = fdopen(fd, "rb");
    if (!f)
        close(fd);
    return f;
}

/* 0 = loaded, -1 = missing/corrupt v2 (rescan), -2 = foreign/unreadable path
 * (stop). An old v1 has lost individual chunk sequences: never infer them or
 * overwrite it. Explicit overrides must point at a new file or a v2 cache. */
static int cache_load(void)
{
    struct cache_hdr h;
    FILE *f = cache_open();
    if (!f) {
        if (errno == ENOENT)
            return -1;
        fprintf(stderr, "iosnand: refusing cache path %s: %s\n", cache_path, strerror(errno));
        return -2;
    }
    if (fread(h.magic, sizeof(h.magic), 1, f) != 1 || memcmp(h.magic, "IOSNAND2", 8)) {
        fclose(f);
        fprintf(stderr, "iosnand: %s is v1 or an unknown cache; choose a new IOSNAND_CACHE path\n", cache_path);
        return -2;
    }
    const size_t page_bytes = sizeof(*pages) * PPN_DIES;
    if (fread((char *)&h + 8, sizeof(h) - 8, 1, f) != 1 ||
        h.dies != PPN_DIES || h.blocks != PPN_BLOCKS || h.pages != PPN_PAGES ||
        h.record_size != sizeof(struct page_rec) || h.rsvd ||
        h.payload_size != sizeof(blocks) + page_bytes ||
        fread(blocks, sizeof(blocks), 1, f) != 1 ||
        fread(pages, page_bytes, 1, f) != 1 || fgetc(f) != EOF || ferror(f) ||
        !cache_records_valid() || h.crc32 != cache_crc32()) {
        fclose(f);
        fprintf(stderr, "iosnand: %s unusable, starting over\n", cache_path);
        memset(blocks, 0, sizeof(blocks));
        memset(pages, 0, page_bytes);
        return -1;
    }
    fclose(f);
    return 0;
}

static int cache_replaceable(void)
{
    FILE *old = cache_open();
    if (!old)
        return errno == ENOENT ? 0 : -1;
    char magic[8];
    int is_v2 = fread(magic, sizeof(magic), 1, old) == 1 && !memcmp(magic, "IOSNAND2", 8);
    fclose(old);
    if (!is_v2) {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

static int cache_save(void)
{
    if (cache_replaceable()) {
        fprintf(stderr, "iosnand: refusing cache replacement %s: %s\n", cache_path, strerror(errno));
        return -1;
    }
    struct cache_hdr h = { "IOSNAND2", PPN_DIES, PPN_BLOCKS, PPN_PAGES,
                          sizeof(struct page_rec), sizeof(blocks) + sizeof(*pages) * PPN_DIES,
                          cache_crc32(), 0 };
    char *dir = strdup(cache_path), *tmp = malloc(strlen(cache_path) + 12);
    if (!dir || !tmp) {
        free(dir);
        free(tmp);
        fprintf(stderr, "iosnand: cannot allocate cache path\n");
        return -1;
    }
    char *slash = strrchr(dir, '/');
    if (slash && slash != dir) {
        *slash = 0;
        mkdir(dir, 0700);
    }
    free(dir);
    sprintf(tmp, "%s.tmp.XXXXXX", cache_path);
    int fd = mkstemp(tmp);      /* private 0600; never follows a fixed .tmp symlink */
    FILE *f = fd < 0 ? NULL : fdopen(fd, "wb");
    int ret = -1;
    if (!f) {
        if (fd >= 0)
            close(fd);
        goto done;
    }
    int ok = fwrite(&h, sizeof(h), 1, f) == 1 &&
             fwrite(blocks, sizeof(blocks), 1, f) == 1 &&
             fwrite(pages, sizeof(*pages) * PPN_DIES, 1, f) == 1 &&
             !fflush(f) && !fsync(fileno(f));
    if (fclose(f))
        ok = 0;
    if (!ok)
        goto done;
    /* Recheck identity before publication as well: a foreign/old file that
     * appeared after startup must remain intact. Cooperative users hold the
     * PPN locks; this does not claim protection against hostile path races. */
    if (cache_replaceable())
        goto done;
    ret = rename(tmp, cache_path);
done:
    if (ret)
        fprintf(stderr, "iosnand: cache save %s failed: %s\n", cache_path, strerror(errno));
    if (fd >= 0)
        unlink(tmp);           /* only if we created it; rename removed it on success */
    free(tmp);
    return ret;
}

/* ---- scanning ------------------------------------------------------------ */

static uint8_t pbuf[PPN_PAGE_RAW] __attribute__((aligned(4)));

static int read_raw(int die, int blk, int pg, uint32_t len, uint8_t *st)
{
    int ret = use_mtd ? mtd_backend_read(die, blk, pg, pbuf, len, st) :
        ppn_read_page(die >> 1, 0, PPN_ROW(die & 1, blk, pg), pbuf, len, st);
    if (ret) {
        fprintf(stderr, "iosnand: die %d block %d page %d: read failed (status 0x%02x)\n",
                die, blk, pg, *st);
        return -1;
    }
    return 0;
}

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
        memset(p->seq, 0, sizeof(p->seq));
        for (int k = 0; k < 4; k++)
            p->lba[k] = NONE;
        if (!PPN_ST_OK(st))
            continue;           /* unreadable page: nothing from it */
        for (int k = 0; k < 4; k++) {
            const uint8_t *m = pbuf + k * PPN_CHUNK + PPN_META_OFF;
            p->lba[k] = meta_lba(m);
            memcpy(p->seq[k], m + 2, 6);
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
            /* PIO exposes physical 0x49 and its write pointer.  MTD only
             * exposes readable FF data, so do not reuse the early-stop
             * cache assumption there: full scans are slower but cannot
             * hide pages appended after a previous all-FF tail. */
            if (!use_mtd && b->state == B_DATA && b->head_seq == head) {
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
    fprintf(stderr, "iosnand: %d blocks read again, %d unchanged, %d erased, %d retired, %.1f s\n",
            rescanned, kept, erased, bad, (now_ms() - t0) / 1000.0);
    return rescanned;
}

/* The page table already owns the exact sequence for every physical chunk.
 * Compare against the current winner there instead of a second NLBA-sized
 * uint64_t table (~29.8 MiB). It is immutable during map construction. */
static uint64_t loc_sequence(uint32_t loc)
{
    uint32_t key = loc >> 2;
    uint32_t die = key / (PPN_BLOCKS * PPN_PAGES);
    uint32_t blk = key / PPN_PAGES % PPN_BLOCKS;
    uint32_t pg = key % PPN_PAGES;

    return page_seq(&pages[die][blk][pg], loc & 3);
}

static void build_l2p(void)
{
    l2p = malloc(NLBA * sizeof(uint32_t));
    if (!l2p) {
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
                    uint64_t seq = page_seq(p, k);
                    if (lba >= NLBA || (l2p[lba] != NONE && seq <= loc_sequence(l2p[lba])))
                        continue;
                    if (l2p[lba] == NONE)
                        mapped++;
                    l2p[lba] = (((uint32_t)die * PPN_BLOCKS + blk) * PPN_PAGES + pg) * 4 + k;
                }
            }
        }
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
        int ret = use_mtd ? mtd_backend_read(die, blk, pg, pcache[slot].data, PPN_PAGE_RAW, &st) :
            ppn_read_page(die >> 1, 0, PPN_ROW(die & 1, blk, pg), pcache[slot].data, PPN_PAGE_RAW, &st);
        if (ret ||
            !PPN_ST_OK(st)) {
            fprintf(stderr, "iosnand: LBA %u: die %d block %d page %d read failed (0x%02x)\n",
                    lba, die, blk, pg, st);
            return -1;
        }
        pcache[slot].key = key + 1;
        d = pcache[slot].data;
    }
    const uint8_t *c = d + k * PPN_CHUNK;
    int die = key / (PPN_BLOCKS * PPN_PAGES), blk = key / PPN_PAGES % PPN_BLOCKS, pg = key % PPN_PAGES;
    if (meta_lba(c + PPN_META_OFF) != lba ||
        meta_seq(c + PPN_META_OFF) != page_seq(&pages[die][blk][pg], k)) {
        /* Same LBA with a different sequence also means this map is stale. */
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

/* Decode only: kept separate from MMIO reads so malformed headers can be
 * regression-tested offline.  A hole or duplicate is not a zero-filled
 * allocation: accepting it would read another partition's NAND blocks. */
static int lwvm_decode(const uint8_t h[4096])
{
    static const uint8_t magic[16] = { 0x6a, 0x90, 0x88, 0xcf, 0x8a, 0xfd, 0x63, 0x0a,
                                       0xe3, 0x51, 0xe2, 0x48, 0x87, 0xe0, 0xb9, 0x8b };
    nparts = 0;
    memset(parts, 0, sizeof(parts));
    if (memcmp(h, magic, 16)) {
        fprintf(stderr, "iosnand: no LwVM header at LBA 0\n");
        return -1;
    }
    uint64_t media_size;
    memcpy(&media_size, h + 0x20, 8);
    if (media_size != NLBA * 4096) {
        fprintf(stderr, "iosnand: unsupported LwVM media size %llu (expected %llu)\n",
                (unsigned long long)media_size, (unsigned long long)(NLBA * 4096));
        return -1;
    }
    uint32_t n;
    memcpy(&n, h + 0x28, 4);
    if (!n || n > 12) {
        fprintf(stderr, "iosnand: invalid LwVM partition count\n");
        return -1;
    }
    const uint8_t *map = h + 0x800;
    for (uint32_t i = 0; i < n; i++) {
        struct part *p = &parts[i];
        const uint8_t *r = h + 0x200 + i * 0x80;
        uint64_t begin, end;
        memcpy(&begin, r + 32, 8);
        memcpy(&end, r + 40, 8);
        for (int c = 0; c < 36; c++)        /* UTF-16LE, ASCII in practice */
            p->name[c] = r[56 + 2 * c];
        p->name[36] = 0;
        p->nchunks = 0;
        for (int c = 0; c < 1024; c++)
            p->phys[c] = UINT16_MAX;
        for (int c = 0; c < 1024; c++) {
            uint16_t v = map[2 * c] | map[2 * c + 1] << 8;
            if (v == LWVM_HDR || v == LWVM_FREE || v >> 12 != i)
                continue;
            if ((v & 0xfff) >= 1024 || p->phys[v & 0xfff] != UINT16_MAX ||
                (uint64_t)c * LWVM_CHUNK >= NLBA * 4096) {
                fprintf(stderr, "iosnand: invalid or duplicate LwVM chunk\n");
                return -1;
            }
            p->phys[v & 0xfff] = c;
            if ((v & 0xfff) + 1u > p->nchunks)
                p->nchunks = (v & 0xfff) + 1;
        }
        if (end <= begin || end > media_size || end - begin > p->nchunks * LWVM_CHUNK) {
            fprintf(stderr, "iosnand: invalid LwVM partition size\n");
            return -1;
        }
        p->size = end - begin;
        for (uint32_t c = 0; c < p->nchunks; c++) {
            uint64_t used = p->size > c * LWVM_CHUNK ?
                p->size - c * LWVM_CHUNK : 0;
            if (used > LWVM_CHUNK)
                used = LWVM_CHUNK;
            if (p->phys[c] == UINT16_MAX ||
                (uint64_t)p->phys[c] * LWVM_CHUNK + used > NLBA * 4096) {
                fprintf(stderr, "iosnand: missing or out-of-range LwVM chunk\n");
                return -1;
            }
        }
    }
    nparts = n;
    return 0;
}

static int lwvm_load(void)
{
    uint8_t h[4096];
    return read_lba(0, h) ? -1 : lwvm_decode(h);
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
    if (off > p->size || len > p->size - off)
        return -1;
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
    int owned;                 /* our SET_SOCK succeeded; not a lasting lease */
    int started;               /* a DO_IT child may have run: no reset ioctl */
    char dev[24];
};

/* A DO_IT worker needs only its control fd and the parent's log. Do not
 * retain other exports' socket peers or PPN/MTD descriptors and bus locks. */
static int child_close_fds(int keep)
{
    DIR *d = opendir("/proc/self/fd");
    if (!d)
        return -1;
    int enumeration_fd = dirfd(d), ret = 0;
    if (enumeration_fd < 0) {
        int saved = errno;
        closedir(d);
        errno = saved;
        return -1;
    }
    for (;;) {
        errno = 0;
        struct dirent *entry = readdir(d);
        if (!entry) {
            if (errno)
                ret = -1;
            break;
        }
        if (!entry->d_name[0] ||
            strspn(entry->d_name, "0123456789") != strlen(entry->d_name))
            continue;
        char *end;
        long fd = strtol(entry->d_name, &end, 10);
        if (errno || *end || fd < 0 || fd > INT_MAX) {
            ret = -1;
            break;
        }
        if (fd != keep && fd != STDERR_FILENO && fd != enumeration_fd && close(fd)) {
            ret = -1;
            break;
        }
    }
    int saved = errno;
    if (closedir(d))
        ret = -1;
    else
        errno = saved;
    return ret;
}

static int cleanup_exports(struct export *ex, int n)
{
    int failed = 0;
    /* Drop all peers first: kernel receive workers can then exit even if
     * several exports started. Never wait while retaining another peer. */
    for (int i = 0; i < n; i++)
        if (ex[i].sock >= 0) {
            if (close(ex[i].sock)) {
                perror("close NBD peer");
                failed = 1;
            }
            ex[i].sock = -1;
        }
    for (int i = 0; i < n; i++) {
        if (ex[i].child > 0) {
            int status;
            pid_t r;
            do { r = waitpid(ex[i].child, &status, 0); } while (r < 0 && errno == EINTR);
            if (r < 0) {
                perror("wait NBD worker");
                failed = 1;
            } else if (!WIFEXITED(status) || WEXITSTATUS(status)) {
                fprintf(stderr, "iosnand: %s worker failed\n", ex[i].dev);
                failed = 1;
            }
            ex[i].child = 0;
        }
        /* Before any DO_IT child exists, unwind our own SET_SOCK. Once a
         * child could have run, release only our descriptors: a global
         * CLEAR/DISCONNECT is not tied to this socket generation and could
         * affect another client. Final kernel config refs clean up normally. */
        if (ex[i].owned && !ex[i].started) {
            if (ioctl(ex[i].nbd, NBD_CLEAR_SOCK)) {
                perror("clear owned NBD socket");
                failed = 1;
            }
        }
        ex[i].owned = 0;
        if (ex[i].nbd >= 0) {
            if (close(ex[i].nbd)) {
                perror("close NBD control");
                failed = 1;
            }
            ex[i].nbd = -1;
        }
    }
    return failed;
}

/* A failed DO_IT/descriptor setup need not close the kernel's socket ref.
 * Detect our worker's exit explicitly instead of waiting forever for HUP. */
static int check_workers(struct export *ex, int n)
{
    int failed = 0;
    for (int i = 0; i < n; i++) {
        if (ex[i].child <= 0)
            continue;
        int status;
        pid_t r = waitpid(ex[i].child, &status, WNOHANG);
        if (!r || (r < 0 && errno == EINTR))
            continue;
        ex[i].child = 0;       /* reaped, or no longer waitable: never wait any */
        if (r < 0) {
            perror("check NBD worker");
            failed = 1;
        } else if (!WIFEXITED(status) || WEXITSTATUS(status) || ex[i].sock >= 0) {
            fprintf(stderr, "iosnand: %s worker stopped before server cleanup\n", ex[i].dev);
            failed = 1;
        }
    }
    return failed;
}

static int attach(struct export *e, int idx)
{
    int sv[2] = { -1, -1 };
    e->nbd = e->sock = -1;
    e->child = 0;
    e->owned = 0;
    e->started = 0;
    snprintf(e->dev, sizeof(e->dev), "/dev/nbd%d", idx);
    e->nbd = open(e->dev, O_RDWR | O_CLOEXEC);
    if (e->nbd < 0) {
        perror(e->dev);
        return -1;
    }
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv)) {
        perror("socketpair");
        goto fail;
    }
    e->sock = sv[1];
    /* SET_SOCK atomically checks the v6.12 kernel's setup-task/Netlink owner.
     * Do not reset or change geometry/flags on an export owned by others. */
    if (ioctl(e->nbd, NBD_SET_SOCK, (unsigned long)sv[0])) {
        perror("claim NBD socket");
        goto fail;
    }
    e->owned = 1;
    if (ioctl(e->nbd, NBD_SET_BLKSIZE, 4096UL) ||
        ioctl(e->nbd, NBD_SET_SIZE_BLOCKS, (unsigned long)(e->p->size / 4096)) ||
        ioctl(e->nbd, NBD_SET_FLAGS, (unsigned long)(NBD_FLAG_HAS_FLAGS | NBD_FLAG_READ_ONLY))) {
        perror("nbd ioctl");
        goto fail;
    }
    e->child = fork();
    if (e->child < 0) {
        perror("fork NBD worker");
        goto fail;
    }
    if (e->child == 0) {
        /* NBD_DO_IT runs the device until it is disconnected */
        if (child_close_fds(e->nbd)) {
            perror("NBD worker descriptors");
            _exit(1);
        }
        int ret = ioctl(e->nbd, NBD_DO_IT);
        if (ret)
            perror("NBD_DO_IT");
        if (close(e->nbd)) {
            perror("close NBD worker control");
            ret = -1;
        }
        _exit(ret ? 1 : 0);
    }
    e->started = 1;
    int closed = close(sv[0]);
    sv[0] = -1;               /* Linux close releases fd even on error; no retry */
    if (closed) {
        perror("close NBD setup socket");
        goto fail;
    }
    e->sock = sv[1];
    fprintf(stderr, "iosnand: %s = %s, %llu MB, read-only\n", e->dev, e->p->name,
            (unsigned long long)(e->p->size >> 20));
    return 0;
fail: {
        int saved = errno;
        if (sv[0] >= 0 && close(sv[0]))
            perror("close failed NBD setup socket");
        cleanup_exports(e, 1);
        errno = saved;
        return -1;
    }
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
        /* Reject impossible ranges before reserving their requested memory.
         * Subtraction avoids overflow of an untrusted from + len. */
        if (from > e->p->size || len > e->p->size - from)
            rp.error = htonl(EIO);
        else if (len > sizeof(sbuf) && !(buf = malloc(len)))
            rp.error = htonl(ENOMEM);
        else if (read_part(e->p, from, buf, len))
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

/* 0: every own export ready; 1: pending; -1: foreign/error/timeout.
 * Each pending result is checked again after the serve loop's bounded poll. */
static int exports_ready(struct export *ex, int n, unsigned *attempts)
{
    for (int i = 0; i < n; i++) {
        if (!process_kernel_state(i, ex[i].child, (ex[i].p->size / 4096) * 8)) continue;
        if (errno != ENOENT && errno != EAGAIN) return -1;
        if (++*attempts >= 10) { errno = ETIMEDOUT; return -1; }
        return 1;
    }
    return 0;
}

static int serve(char **names, int n)
{
    static char *deflt[] = { "System" };
    struct export ex[12];
    int nex = 0, failed = 0, published_ready = 0;
    unsigned readiness_attempts = 0;
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
        if (attach(&ex[nex], nex)) {
            cleanup_exports(ex, nex);
            return 1;
        }
        nex++;
    }
    if (!nex)
        return 1;
    /* The wrapper's generation-ready API authorizes System on export zero.
     * Manual unregistered serve retains its existing arbitrary-part API. */
    if (process_record_path && strcmp(ex[0].p->name, "System")) {
        fprintf(stderr, "iosnand: registered readiness requires System as export zero\n");
        cleanup_exports(ex, nex);
        return 1;
    }
    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);
    signal(SIGPIPE, SIG_IGN);
    struct pollfd pf[12];
    while (!stop) {
        for (int i = 0; i < nex; i++)
            pf[i] = (struct pollfd){ ex[i].sock, POLLIN, 0 };
        int ready = poll(pf, nex, 1000);
        if (ready < 0 && errno != EINTR) {
            perror("poll NBD peers");
            failed = 1;
            break;
        }
        if (check_workers(ex, nex)) {
            failed = 1;
            break;
        }
        if (process_record_path && !published_ready) {
            int pending = exports_ready(ex, nex, &readiness_attempts);
            if (pending < 0) {
                perror("iosnand: own export readiness failed (no reset)");
                failed = 1; break;
            }
            if (!pending) {
                if (process_publish_ready(process_record_path, ex[0].child, (ex[0].p->size / 4096) * 8)) {
                    perror("iosnand: cannot publish export readiness");
                    failed = 1; break;
                }
                published_ready = 1;
            }
        }
        if (ready < 0)
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
    failed |= cleanup_exports(ex, nex);
    fprintf(stderr, "iosnand: stopped\n");
    return failed;
}

/* ---- main ---------------------------------------------------------------- */

static int parse_number(const char *s, uint32_t *value)
{
    char *end;
    unsigned long long v;
    if (!*s || *s == '-')
        return -1;
    errno = 0;
    v = strtoull(s, &end, 0);
    if (errno || *end || v > UINT32_MAX)
        return -1;
    *value = v;
    return 0;
}

/* Check syntax and requested read bounds before ppn_open maps /dev/mem or
 * scan reads a page.  A typo should never start a fifteen-minute scan. */
static int check_args(int argc, char **argv, uint32_t *lba, uint32_t *n)
{
    if (argc < 2)
        return -1;
    if (!strcmp(argv[1], "scan") || !strcmp(argv[1], "parts"))
        return argc == 2 ? 0 : -1;
    if (!strcmp(argv[1], "serve"))
        return argc <= 14 ? 0 : -1;
    if (strcmp(argv[1], "read") || argc < 3 || argc > 4)
        return -1;
    *n = 1;
    if (parse_number(argv[2], lba) || (argc == 4 && parse_number(argv[3], n)) ||
        *lba > NLBA || *n > NLBA - *lba)
        return -1;
    return 0;
}

int main(int argc, char **argv)
{
    const char *pid_record = NULL;
    if (argc > 1 && !strcmp(argv[1], "--pid-record")) {
        if (argc < 4 || !argv[2][0] || strcmp(argv[3], "serve")) {
            fprintf(stderr, "iosnand: --pid-record PATH is only valid before serve\n");
            return 1;
        }
        pid_record = argv[2];
        argc -= 2; argv += 2;
    }
    uint32_t read_start = 0, read_count = 0;
    int help = argc == 2 && (!strcmp(argv[1], "--help") || !strcmp(argv[1], "-h"));
    if (help || check_args(argc, argv, &read_start, &read_count)) {
        fprintf(stderr, "usage: iosnand scan | parts | read LBA [N] | [--pid-record PATH] serve [PARTITION...]\n");
        return help ? 0 : 1;
    }
    const char *backend = getenv("IOSNAND_BACKEND");
    if (backend && strcmp(backend, "pio") && strcmp(backend, "mtd")) {
        fprintf(stderr, "iosnand: IOSNAND_BACKEND must be pio or mtd\n");
        return 1;
    }
    use_mtd = backend && !strcmp(backend, "mtd");
    if (getenv("IOSNAND_CACHE"))
        cache_path = getenv("IOSNAND_CACHE");
    if (pid_record && process_record_self(pid_record)) {
        perror("iosnand: cannot publish private process record; no backend opened");
        return 1;
    }
    process_record_path = pid_record;
    pages = calloc(PPN_DIES, sizeof(*pages));
    if (!pages) {
        fprintf(stderr, "iosnand: out of memory\n");
        return 1;
    }
    int loaded = cache_load();
    if (loaded == -2) {
        free(pages);
        pages = NULL;
        return 1;              /* no device open for a legacy/foreign path */
    }
    if (loaded)
        fprintf(stderr, "iosnand: no v2 page cache yet: full scan (PIO previously ~15 minutes)\n");
    if (use_mtd) {
        if (mtd_backend_open()) {
            fprintf(stderr, "iosnand: read-only MTD backend unavailable or wrong geometry; no PIO fallback\n");
            return 1;
        }
        atexit(mtd_backend_close);
        for (int bus = 0; bus < PPN_BUSES; bus++)
            if (!ppn_lock(bus))
                return 1;
    } else {
        ppn_open();
        for (int bus = 0; bus < PPN_BUSES; bus++)
            if (!ppn_claim(bus))
                return 1;
    }
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
    if (!strcmp(argv[1], "read")) {
        uint8_t b[4096];
        for (uint32_t i = 0; i < read_count; i++)
            if (read_lba(read_start + i, b) || write_full(1, b, 4096))
                return 1;
        return 0;
    }
    if (!strcmp(argv[1], "serve"))
        return serve(argv + 2, argc - 2);
    fprintf(stderr, "iosnand: unknown command %s\n", argv[1]);
    return 1;
}
