/* Offline tests of production LwVM parsing.  No MMIO, NBD or device access.
 * Compile with -ffunction-sections -fdata-sections -Wl,--gc-sections: the
 * hardware paths are discarded, so no PPN implementation is even linked. */
static int fake_mtd_ioctl(int fd, unsigned long request, void *arg);
#define IOSNAND_IOCTL fake_mtd_ioctl
#define main iosnand_device_main
#include "../tools/nand/iosnand.c"
#undef main
#include <assert.h>

static unsigned fake_mtd_reads;
static int fake_mtd_unsupported;
static int fake_mtd_ioctl(int fd, unsigned long request, void *arg)
{
    assert(request == IOSNAND_MEMREAD && fd == 1000);
    if (fake_mtd_unsupported) {
        errno = ENOTTY;
        return -1;
    }
    struct iosnand_mtd_read_req *req = arg;
    unsigned pg = (req->start / IOSNAND_MTD_DATA) % PPN_PAGES;
    fake_mtd_reads++;
    uint8_t *data = (void *)(uintptr_t)req->usr_data;
    uint8_t *oob = (void *)(uintptr_t)req->usr_oob;
    memset(data, 0xff, 16384);
    memset(oob, 0xff, 64);
    /* A valid page after an all-FF page proves no erased-page shortcut
     * can stop the MTD scan at page zero. */
    if (pg == 1)
        for (unsigned c = 0; c < 4; c++) {
            uint8_t *m = oob + c * 16;
            memset(m, 0, 16);
            m[0] = 1;
            const uint64_t seq[] = { 100, 107, 0, 900 };
            for (unsigned i = 0; i < 6; i++)
                m[2 + i] = seq[c] >> (8 * i);
            m[8] = 3 + c;
        }
    req->start += req->len;
    return 0;
}

/* Even a regression in the bounds guard can never reach MMIO: fail hard if
 * a test accidentally requests an actual PPN page. */
int ppn_read_page(int bus, int ce, uint32_t row, uint8_t *buf, uint32_t len, uint8_t *st)
{
    abort();
}

int ppn_open(void) { abort(); }
int ppn_claim(int bus) { abort(); }
int ppn_lock(int bus) { abort(); }

static void put32(uint8_t *h, size_t off, uint32_t v)
{
    v = htole32(v);
    memcpy(h + off, &v, 4);
}

static void put64(uint8_t *h, size_t off, uint64_t v)
{
    v = htole64(v);
    memcpy(h + off, &v, 8);
}

static void chunk(uint8_t *h, unsigned physical, uint16_t value)
{
    h[0x800 + physical * 2] = value & 255;
    h[0x801 + physical * 2] = value >> 8;
}

static void header(uint8_t h[4096])
{
    const uint8_t magic[] = { 0x6a, 0x90, 0x88, 0xcf, 0x8a, 0xfd, 0x63, 0x0a,
                             0xe3, 0x51, 0xe2, 0x48, 0x87, 0xe0, 0xb9, 0x8b };
    memset(h, 0, 4096);
    memcpy(h, magic, 16);
    put64(h, 0x20, NLBA * 4096);
    put32(h, 0x28, 1);
    put64(h, 0x228, LWVM_CHUNK + 1234);
    const char *name = "System";
    for (unsigned i = 0; name[i]; i++)
        h[0x238 + 2 * i] = name[i];
    for (unsigned p = 0; p < 1024; p++)
        chunk(h, p, LWVM_FREE);
    chunk(h, 0, LWVM_HDR);
    chunk(h, 1, 0);
    chunk(h, 2, 1);
}

int main(void)
{
    uint8_t h[4096], unused[1];
    unsigned checks = 0;
    header(h);
    assert(lwvm_decode(h) == 0);
    assert(nparts == 1 && parts[0].size == LWVM_CHUNK + 1234);
    assert(parts[0].nchunks == 2 && parts[0].phys[0] == 1 && parts[0].phys[1] == 2);
    assert(!strcmp(parts[0].name, "System"));
    checks++;

    /* Request bounds must be checked before read_lba can touch hardware. */
    assert(read_part(&parts[0], UINT64_MAX, unused, 1) == -1);
    assert(read_part(&parts[0], parts[0].size, unused, 1) == -1);
    assert(read_part(&parts[0], parts[0].size, unused, 0) == 0);
    checks += 3;

    h[0] ^= 1;
    assert(lwvm_decode(h) == -1 && nparts == 0);
    checks++;
    const uint32_t counts[] = { 0, 13, UINT32_MAX };
    for (unsigned i = 0; i < sizeof(counts) / sizeof(counts[0]); i++) {
        header(h);
        put32(h, 0x28, counts[i]);
        assert(lwvm_decode(h) == -1 && nparts == 0);
        checks++;
    }
    header(h);
    put64(h, 0x20, 32000000000ull);
    assert(lwvm_decode(h) == -1 && nparts == 0);
    checks++;

    header(h);
    chunk(h, 3, 0);             /* duplicate logical chunk */
    assert(lwvm_decode(h) == -1 && nparts == 0);
    checks++;
    header(h);
    chunk(h, 2, 2);             /* missing logical chunk 1 */
    assert(lwvm_decode(h) == -1 && nparts == 0);
    checks++;
    header(h);
    chunk(h, 2, 1024);          /* phys[] would otherwise be out of bounds */
    assert(lwvm_decode(h) == -1 && nparts == 0);
    checks++;
    header(h);
    chunk(h, 2, LWVM_FREE);
    chunk(h, 1023, 1);          /* physical chunk beyond 16 GB disk0 */
    assert(lwvm_decode(h) == -1 && nparts == 0);
    checks++;

    const uint64_t ends[] = { 0, 2 * LWVM_CHUNK + 1, UINT64_MAX };
    for (unsigned i = 0; i < sizeof(ends) / sizeof(ends[0]); i++) {
        header(h);
        put64(h, 0x228, ends[i]);
        assert(lwvm_decode(h) == -1 && nparts == 0);
        checks++;
    }
    header(h);
    put64(h, 0x220, 100);
    put64(h, 0x228, 99);         /* end-before-begin, unsigned underflow */
    assert(lwvm_decode(h) == -1 && nparts == 0);
    checks++;

    /* Corrupt cache records must not become build_l2p array bounds. */
    pages = calloc(PPN_DIES, sizeof(*pages));
    assert(pages);
    assert(cache_records_valid());
    checks++;
    blocks[0][1].written = PPN_PAGES + 1;
    assert(!cache_records_valid());
    checks++;
    blocks[0][1].written = PPN_PAGES;
    blocks[0][1].state = B_BAD + 1;
    assert(!cache_records_valid());
    checks++;
    blocks[0][1].state = B_DATA;
    blocks[0][1].head_seq = UINT64_MAX;
    assert(!cache_records_valid());
    checks++;
    blocks[0][1].head_seq = 1;
    memset(pages[0][1][0].seq, 0xff, sizeof(pages[0][1][0].seq));
    assert(page_seq(&pages[0][1][0], 3) == (1ull << 48) - 1);
    checks++;
    assert(page_seq(&pages[0][1][0], 0) == (1ull << 48) - 1);
    assert(cache_records_valid());
    checks++;
    memset(blocks, 0, sizeof(blocks));
    memset(pages, 0, PPN_DIES * sizeof(*pages));
    use_mtd = 1;
    mtd_fds[0] = 1000;
    assert(scan_block(0, 1, 0) == 0);
    assert(fake_mtd_reads == PPN_PAGES);
    assert(blocks[0][1].written == PPN_PAGES);
    assert(pages[0][1][1].lba[0] == 3 && page_seq(&pages[0][1][1], 0) == 100);
    assert(page_seq(&pages[0][1][1], 1) == 107);
    assert(page_seq(&pages[0][1][1], 2) == 0);
    assert(page_seq(&pages[0][1][1], 3) == 900);
    checks += 3;
    assert(pages[0][1][0].lba[0] == NONE && pages[0][1][255].lba[0] == NONE);
    checks++;
    fake_mtd_unsupported = 1;
    uint8_t st;
    assert(read_raw(0, 1, 0, META_LEN, &st) == -1 && errno == ENOTTY);
    checks++;
    fake_mtd_unsupported = 0;
    use_mtd = 0;
    free(pages);
    pages = NULL;

    char *help[] = { "iosnand", "--help", NULL };
    char *bad[] = { "iosnand", "unknown", NULL };
    char *short_read[] = { "iosnand", "read", NULL };
    char *negative[] = { "iosnand", "read", "-1", NULL };
    char *beyond[] = { "iosnand", "read", "3906250", "1", NULL };
    char *garbage[] = { "iosnand", "read", "123garbage", NULL };
    char *overflow[] = { "iosnand", "read", "4294967296", NULL };
    assert(iosnand_device_main(2, help) == 0);
    assert(iosnand_device_main(2, bad) == 1);
    assert(iosnand_device_main(2, short_read) == 1);
    assert(iosnand_device_main(3, negative) == 1);
    assert(iosnand_device_main(4, beyond) == 1);
    assert(iosnand_device_main(3, garbage) == 1);
    assert(iosnand_device_main(3, overflow) == 1);
    checks += 7;
    uint32_t start, count;
    char *valid[] = { "iosnand", "read", "0x1", "2", NULL };
    assert(check_args(4, valid, &start, &count) == 0 && start == 1 && count == 2);
    checks++;
    printf("iosnand: %u offline parser/bounds cases passed\n", checks);
    return 0;
}
