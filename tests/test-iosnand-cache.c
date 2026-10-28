/* Exercise the production v2 cache and exact sequence map without hardware.
 * Fail hard if any PPN backend path is reached by the offline fixtures. */
#define main iosnand_device_main
#include "../tools/nand/iosnand.c"
#undef main
#include <assert.h>

int ppn_read_page(int bus, int ce, uint32_t row, uint8_t *buf, uint32_t len, uint8_t *st)
{ abort(); }
int ppn_open(void) { abort(); }
int ppn_claim(int bus) { abort(); }
int ppn_lock(int bus) { abort(); }

static int fail_fsync;
int __real_fsync(int fd);
int __wrap_fsync(int fd)
{
    if (fail_fsync) {
        errno = EIO;
        return -1;
    }
    return __real_fsync(fd);
}

static void set_seq(struct page_rec *p, unsigned k, uint64_t seq)
{
    for (unsigned i = 0; i < 6; i++)
        p->seq[k][i] = seq >> (8 * i);
}

static void set_meta(uint8_t *m, uint32_t lba, uint64_t seq)
{
    memset(m, 0, 16);
    m[0] = 1;
    for (unsigned i = 0; i < 6; i++)
        m[2 + i] = seq >> (8 * i);
    for (unsigned i = 0; i < 4; i++)
        m[8 + i] = lba >> (8 * i);
}

/* Small independent oracle uses the originally generated sequence value,
 * never page_seq/loc_sequence, and tracks deterministic first-wins ties. */
static uint64_t oracle_seq[68];
static uint32_t oracle_loc[68];
static uint32_t random_state = 0x19465a5u;
static uint32_t random_word(void)
{
    random_state ^= random_state << 13;
    random_state ^= random_state >> 17;
    random_state ^= random_state << 5;
    return random_state;
}

static void candidate(unsigned die, unsigned blk, unsigned pg, unsigned k,
                      unsigned lba, uint64_t seq)
{
    struct page_rec *p = &pages[die][blk][pg];
    p->lba[k] = lba;
    set_seq(p, k, seq);
    blocks[die][blk].state = B_DATA;
    if (blocks[die][blk].written < pg + 1)
        blocks[die][blk].written = pg + 1;
    if (oracle_loc[lba] == NONE || seq > oracle_seq[lba]) {
        oracle_seq[lba] = seq;
        oracle_loc[lba] = ((die * PPN_BLOCKS + blk) * PPN_PAGES + pg) * 4 + k;
    }
}

static unsigned random_map_test(void)
{
    memset(blocks, 0, sizeof(blocks));
    memset(pages, 0, PPN_DIES * sizeof(*pages));
    memset(oracle_loc, 0xff, sizeof(oracle_loc));
    memset(oracle_seq, 0, sizeof(oracle_seq));
    for (unsigned die = 0; die < PPN_DIES; die++)
        for (unsigned b = 0; b < 2; b++)
            for (unsigned pg = 0; pg < 17; pg++)
                for (unsigned k = 0; k < 4; k++) {
                    unsigned lba = random_word() % 64;
                    uint64_t seq = (uint64_t)(random_word() & 0xffff) << 32;
                    seq |= random_word();
                    candidate(die, b * 2 + 1, pg, k, lba, seq);
                }
    /* Cross-die/cross-chunk duplicates include valid zero, maximal u48,
     * equal-sequence first-wins and a later strictly newer candidate. */
    candidate(0, 7, 0, 0, 64, 0);
    candidate(0, 7, 0, 1, 65, (1ull << 48) - 1);
    candidate(0, 7, 0, 2, 66, 900);
    candidate(0, 7, 0, 3, 67, 900);
    candidate(3, 7, 0, 3, 64, 0);
    candidate(3, 7, 0, 2, 65, (1ull << 48) - 1);
    candidate(3, 7, 0, 1, 66, 900);
    candidate(3, 7, 0, 0, 67, 901);
    build_l2p();
    for (unsigned lba = 0; lba < 68; lba++) {
        assert(l2p[lba] == oracle_loc[lba]);
        assert(loc_sequence(l2p[lba]) == oracle_seq[lba]);
    }
    assert(l2p[68] == NONE && l2p[NLBA - 1] == NONE);
    free(l2p);
    l2p = NULL;
    return 69;
}

static FILE *edit_cache(void)
{
    FILE *f = fopen(cache_path, "r+b");
    assert(f);
    return f;
}

static void magic_file(const char *magic)
{
    FILE *f = fopen(cache_path, "wb");
    assert(f);
    assert(fwrite(magic, 8, 1, f) == 1);
    assert(!fclose(f));
}

static void check_magic(const char *magic)
{
    char got[8];
    FILE *f = fopen(cache_path, "rb");
    assert(f && fread(got, 8, 1, f) == 1);
    assert(!memcmp(got, magic, 8));
    assert(fgetc(f) == EOF);
    assert(!fclose(f));
}

int main(void)
{
    unsigned checks = 0;
    char dir[] = "/tmp/cascadia-iosnand-cache-XXXXXX";
    assert(mkdtemp(dir));
    char path[256], other[256];
    snprintf(path, sizeof(path), "%s/cache.v2", dir);
    snprintf(other, sizeof(other), "%s/symlink", dir);
    cache_path = path;
    pages = calloc(PPN_DIES, sizeof(*pages));
    assert(pages && cache_load() == -1);
    checks++;
    /* Independently calculated with Python zlib over 43,649,536 zero bytes. */
    assert(cache_crc32() == 0xfb824c71u);
    checks++;

    /* Competing chunk-3 copies: seq=base+k would incorrectly select pg1's
     * 503 over pg0's 103. Actual metadata says pg0 seq900 wins over pg1 700. */
    struct page_rec *a = &pages[0][1][0], *b = &pages[0][1][1];
    for (unsigned k = 0; k < 4; k++) {
        a->lba[k] = b->lba[k] = NONE;
        set_seq(a, k, 100 + k);
        set_seq(b, k, 500 + k);
    }
    a->lba[3] = b->lba[3] = 7;
    set_seq(a, 3, 900);
    set_seq(b, 3, 700);
    b->lba[2] = 8;
    set_seq(b, 2, 0);          /* valid seq0, not an unmapped sentinel */
    blocks[0][1].state = B_DATA;
    blocks[0][1].written = 2;
    blocks[0][1].head_seq = 100;
    build_l2p();
    uint32_t key = PPN_PAGES;
    assert(l2p[7] == key * 4 + 3);
    assert(l2p[8] == (key + 1) * 4 + 2);
    assert(l2p[9] == NONE);
    checks += 3;

    /* Use production page cache to exercise read_lba without any ioctl. */
    pcache[0].key = key + 1;
    uint8_t *c = pcache[0].data + 3 * PPN_CHUNK;
    memset(c, 0x5a, PPN_CHUNK);
    set_meta(c + PPN_META_OFF, 7, 900);
    uint8_t out[4096];
    assert(!read_lba(7, out) && out[0] == 0x5a && out[4095] == 0x5a);
    set_meta(c + PPN_META_OFF, 7, 901);
    memset(out, 0xab, sizeof(out));
    assert(read_lba(7, out) == -1 && out[0] == 0xab && out[4095] == 0xab);
    set_meta(c + PPN_META_OFF, 6, 900);
    assert(read_lba(7, out) == -1);
    checks += 3;
    free(l2p);
    l2p = NULL;

    assert(!cache_save());
    struct stat sb;
    assert(!stat(path, &sb));
    assert((sb.st_mode & 0777) == 0600);
    assert(sb.st_size == sizeof(struct cache_hdr) + sizeof(blocks) + PPN_DIES * sizeof(*pages));
    memset(blocks, 0, sizeof(blocks));
    memset(pages, 0, sizeof(*pages) * PPN_DIES);
    assert(!cache_load());
    assert(page_seq(&pages[0][1][0], 3) == 900 && page_seq(&pages[0][1][1], 2) == 0);
    assert(blocks[0][1].written == 2);
    checks += 3;

    /* Failed persistence cannot publish partial data or destroy an old v2.
     * Linker wrapping injects EIO into the actual production fsync call. */
    set_seq(&pages[0][1][0], 3, 1000);
    fail_fsync = 1;
    assert(cache_save() == -1);
    fail_fsync = 0;
    assert(!cache_load() && page_seq(&pages[0][1][0], 3) == 900);
    checks++;

    /* A valid-looking u48 bit flip must fail CRC, even though all bounds
     * and metadata types still make sense. This uses a 44MB cache, not NAND. */
    FILE *f = edit_cache();
    size_t off = sizeof(struct cache_hdr) + sizeof(blocks) +
                 PPN_PAGES * sizeof(struct page_rec) + 3 * 6;
    assert(!fseek(f, off, SEEK_SET));
    int byte = fgetc(f);
    assert(byte >= 0 && !fseek(f, off, SEEK_SET));
    assert(fputc(byte ^ 1, f) != EOF && !fclose(f));
    assert(cache_load() == -1);
    assert(blocks[0][1].written == 0 && page_seq(&pages[0][1][0], 3) == 0);
    checks++;

    assert(!cache_save());
    assert(!truncate(path, sb.st_size - 1));
    assert(cache_load() == -1);
    checks++;
    assert(!cache_save());
    f = fopen(path, "ab");
    assert(f && fputc(0, f) != EOF && !fclose(f));
    assert(cache_load() == -1); /* trailing payload is not part of v2 */
    checks++;
    /* Wrong length/record ABI is rejected before attempting full reads. */
    f = edit_cache();
    assert(!fseek(f, offsetof(struct cache_hdr, record_size), SEEK_SET));
    uint32_t wrong = 24;
    assert(fwrite(&wrong, 4, 1, f) == 1 && !fclose(f));
    assert(cache_load() == -1);
    checks++;
    f = edit_cache();
    struct cache_hdr bad_header;
    assert(fread(&bad_header, sizeof(bad_header), 1, f) == 1);
    bad_header.record_size = sizeof(struct page_rec);
    bad_header.payload_size = 1;
    rewind(f);
    assert(fwrite(&bad_header, sizeof(bad_header), 1, f) == 1 && !fclose(f));
    assert(cache_load() == -1);
    checks++;
    f = edit_cache();
    bad_header.payload_size = sizeof(blocks) + PPN_DIES * sizeof(*pages);
    bad_header.dies++;
    assert(fwrite(&bad_header, sizeof(bad_header), 1, f) == 1 && !fclose(f));
    assert(cache_load() == -1);
    checks++;
    assert(!truncate(path, sizeof(struct cache_hdr) - 1));
    assert(cache_load() == -1);
    checks++;
    assert(!unlink(path));

    /* v1 detection is by bytes, even under a .v2 name. Never modify a v1. */
    magic_file("IOSNAND1");
    assert(cache_load() == -2);
    assert(cache_save() == -1);
    check_magic("IOSNAND1");
    checks++;
    magic_file("FOREIGN!");
    assert(cache_load() == -2 && cache_save() == -1);
    check_magic("FOREIGN!");
    checks++;
    assert(!symlink(path, other));
    cache_path = other;
    assert(cache_load() == -2 && cache_save() == -1);
    check_magic("FOREIGN!"); /* fopen test intentionally follows local fixture */
    assert(!unlink(other));
    assert(!mkfifo(other, 0600));
    assert(cache_load() == -2); /* must not hang */
    assert(!unlink(other));
    cache_path = dir;
    assert(cache_load() == -2);
    checks += 3;

    /* The actual command exits on an old/foreign override before ppn_open:
     * the backend stubs would abort this process if regression reached it. */
    cache_path = path;
    free(pages);
    pages = NULL;
    pages = calloc(PPN_DIES, sizeof(*pages));
    assert(pages);
    checks += random_map_test();
    free(pages);
    pages = NULL;
    setenv("IOSNAND_CACHE", path, 1);
    setenv("IOSNAND_BACKEND", "pio", 1);
    char *argv[] = { "iosnand", "scan", NULL };
    assert(iosnand_device_main(2, argv) == 1);
    assert(!pages);
    check_magic("FOREIGN!");
    magic_file("IOSNAND1");
    assert(iosnand_device_main(2, argv) == 1);
    assert(!pages);
    check_magic("IOSNAND1");
    checks += 2;
    unsetenv("IOSNAND_CACHE");
    unsetenv("IOSNAND_BACKEND");
    assert(!unlink(path) && !rmdir(dir)); /* no abandoned .tmp files */
    printf("iosnand: %u offline v2-cache/exact-sequence cases passed\n", checks);
    return 0;
}
