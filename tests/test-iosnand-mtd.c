/* Exercise the actual MTD backend through a fake ioctl; no files/devices
 * opened, no PPN implementation linked, no real kernel ioctl performed. */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <assert.h>
#include "../tools/nand/ppn.h"
static int fake_ioctl(int fd, unsigned long request, void *arg);
#define IOSNAND_IOCTL fake_ioctl
#include "../tools/nand/iosnand-mtd.h"

enum { GOOD, REFRESH, CORRECTED_STATS, BLANK, RETIRED, BAD, TIMEOUT, SHORT_DATA,
       SHORT_OOB, UNCORRECTABLE, BAD_BLOCK_QUERY_FAILED, UNSUPPORTED };
static int behavior, calls, seen_fd;
static uint64_t seen_offset;
static int64_t seen_block;

static int fake_ioctl(int fd, unsigned long code, void *arg)
{
    calls++;
    seen_fd = fd;
    if (code == MEMGETBADBLOCK) {
        seen_block = *(int64_t *)arg;
        if (behavior == BAD_BLOCK_QUERY_FAILED) {
            errno = ENODEV;
            return -1;
        }
        return behavior == RETIRED ? 1 : 0;
    }
    assert(code == IOSNAND_MEMREAD); /* No program/erase or real ioctl possible. */
    struct iosnand_mtd_read_req *req = arg;
    assert(req->mode == MTD_OPS_PLACE_OOB);
    assert(req->len == 16384 && req->ooblen == 64);
    seen_offset = req->start;
    if (behavior == RETIRED || behavior == BAD || behavior == BAD_BLOCK_QUERY_FAILED) {
        errno = EBADMSG;
        return -1;
    }
    if (behavior == TIMEOUT || behavior == UNSUPPORTED) {
        errno = behavior == UNSUPPORTED ? ENOTTY : ETIMEDOUT;
        return -1;
    }
    uint8_t *data = (void *)(uintptr_t)req->usr_data;
    uint8_t *oob = (void *)(uintptr_t)req->usr_oob;
    for (unsigned c = 0; c < 4; c++) {
        memset(data + c * 4096, behavior == BLANK ? 0xff : 0x10 + c, 4096);
        memset(oob + c * 16, behavior == BLANK ? 0xff : 0xa0 + c, 16);
    }
    req->start += req->len;     /* mtdchar reports bytes consumed. */
    if (behavior == SHORT_DATA)
        req->len--;
    if (behavior == SHORT_OOB)
        req->ooblen--;
    if (behavior == UNCORRECTABLE)
        req->ecc_stats.uncorrectable_errors = 1;
    if (behavior == CORRECTED_STATS)
        req->ecc_stats.max_bitflips = 1;
    if (behavior == REFRESH) {
        req->ecc_stats.max_bitflips = 1;
        errno = EUCLEAN;
        return -1;
    }
    return 0;
}

static void check_layout(const uint8_t *raw, unsigned len)
{
    for (unsigned off = 0; off < len; off++) {
        unsigned c = off / 4112, pos = off % 4112;
        unsigned value = pos >= 1024 && pos < 1040 ? 0xa0 + c : 0x10 + c;
        assert(raw[off] == value);
    }
}

int main(void)
{
    unsigned checks = 0;
    uint8_t raw[PPN_PAGE_RAW + 1], status;
    for (int die = 0; die < PPN_DIES; die++)
        mtd_fds[die] = 100 + die;
    for (int die = 0; die < PPN_DIES; die++) {
        behavior = GOOD;
        calls = 0;
        assert(mtd_backend_read(die, 1024, 255, raw, PPN_PAGE_RAW, &status) == 0);
        assert(status == 0x40 && seen_fd == 100 + die && calls == 1);
        assert(seen_offset == ((uint64_t)1024 * 256 + 255) * 16384);
        assert(seen_offset > UINT32_MAX);
        check_layout(raw, PPN_PAGE_RAW);
        checks++;
    }
    memset(raw, 0x55, sizeof(raw));
    const unsigned prefix = 3 * PPN_CHUNK + 1040;
    assert(mtd_backend_read(0, 1, 0, raw, prefix, &status) == 0);
    check_layout(raw, prefix);
    assert(raw[prefix] == 0x55);
    checks++;
    for (int b = REFRESH; b <= CORRECTED_STATS; b++) {
        behavior = b;
        assert(mtd_backend_read(0, 1, 1, raw, PPN_PAGE_RAW, &status) == 0);
        assert(status == 0x42);
        check_layout(raw, PPN_PAGE_RAW);
        checks++;
    }
    behavior = BLANK;
    assert(mtd_backend_read(0, 1, 1, raw, PPN_PAGE_RAW, &status) == 0 && status == 0x40);
    for (unsigned i = 0; i < PPN_PAGE_RAW; i++)
        assert(raw[i] == 0xff);
    checks++;
    behavior = RETIRED;
    calls = 0;
    assert(mtd_backend_read(0, 1024, 7, raw, PPN_PAGE_RAW, &status) == 0 && status == 0x45);
    assert(calls == 2 && seen_block == (int64_t)1024 * 4194304);
    checks++;
    const int bad_behaviors[] = { BAD, TIMEOUT, SHORT_DATA, SHORT_OOB, UNCORRECTABLE, BAD_BLOCK_QUERY_FAILED, UNSUPPORTED };
    for (unsigned i = 0; i < sizeof(bad_behaviors) / sizeof(bad_behaviors[0]); i++) {
        behavior = bad_behaviors[i];
        calls = 0;
        memset(raw, 0x55, sizeof(raw));
        assert(mtd_backend_read(0, 1, 1, raw, PPN_PAGE_RAW, &status) == -1 && status == 0);
        assert(errno == (behavior == UNSUPPORTED ? ENOTTY : behavior == TIMEOUT ? ETIMEDOUT :
                        behavior == BAD ? EBADMSG :
                        behavior == BAD_BLOCK_QUERY_FAILED ? ENODEV : EIO));
        assert(raw[0] == 0x55);  /* Never publish partial/error data. */
        if (behavior == UNSUPPORTED)
            assert(calls == 1); /* No legacy read or PIO fallback. */
        checks++;
    }
    calls = 0;
    assert(mtd_backend_read(-1, 1, 1, raw, 16, &status) == -1);
    assert(mtd_backend_read(4, 1, 1, raw, 16, &status) == -1);
    assert(mtd_backend_read(0, 1064, 1, raw, 16, &status) == -1);
    assert(mtd_backend_read(0, 1, 256, raw, 16, &status) == -1);
    assert(mtd_backend_read(0, 1, 1, raw, PPN_PAGE_RAW + 1, &status) == -1);
    assert(calls == 0);
    checks += 5;

    struct mtd_info_user info = {
        .type = MTD_MLCNANDFLASH, .flags = MTD_NO_ERASE,
        .size = (uint32_t)IOSNAND_MTD_SIZE, .writesize = 16384,
        .erasesize = 4194304, .oobsize = 64,
    };
    assert(mtd_geometry_ok(&info, IOSNAND_MTD_SIZE));
    assert(!mtd_geometry_ok(&info, (uint32_t)IOSNAND_MTD_SIZE));
    info.flags |= MTD_WRITEABLE;
    assert(!mtd_geometry_ok(&info, IOSNAND_MTD_SIZE));
    info.flags &= ~MTD_WRITEABLE;
    info.type = MTD_NANDFLASH;
    assert(!mtd_geometry_ok(&info, IOSNAND_MTD_SIZE)); /* Legacy SLC label. */
    info.type = MTD_NORFLASH;
    assert(!mtd_geometry_ok(&info, IOSNAND_MTD_SIZE));
    info.type = MTD_MLCNANDFLASH;
    info.flags |= MTD_SLC_ON_MLC_EMULATION;
    assert(!mtd_geometry_ok(&info, IOSNAND_MTD_SIZE));
    info.flags &= ~MTD_SLC_ON_MLC_EMULATION;
    assert(mtd_geometry_ok(&info, IOSNAND_MTD_SIZE));
    info.oobsize = 128;
    assert(!mtd_geometry_ok(&info, IOSNAND_MTD_SIZE));
    checks += 8;
    printf("iosnand-mtd: %u offline ioctl/layout/geometry cases passed\n", checks);
    return 0;
}
