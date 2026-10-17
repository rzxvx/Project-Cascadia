/* Read-only kernel-MTD backend for iosnand.  Header-only so the existing
 * iosnand.c + ppn.c build command remains valid.  No write/erase ioctl exists
 * here.  Explicit opt-in: IOSNAND_BACKEND=mtd; errors never fall back to PIO.
 * Layout is the inverse of mtd-apple-h2fmi.c:h2fmi_split_page.
 */
#ifndef IOSNAND_MTD_H
#define IOSNAND_MTD_H
#include <dirent.h>
#include <ctype.h>
#include <limits.h>
#include <mtd/mtd-abi.h>
#include <stddef.h>
#include <sys/sysmacros.h>

/* Compatibility layout reproduced from Linux v6.12 include/uapi/mtd/mtd-abi.h
 * (adc218676eef25575469234709c2d87185ca223a).
 * UAPI source SPDX: GPL-2.0+ WITH Linux-syscall-note.
 * Ubuntu 22.04's 5.15 cross-sysroot predates MEMREAD.  Keep our own names and
 * the exact 6.12 ABI so building does not require replacing system headers.
 * Runtime kernels without this ioctl fail ENOTTY; there is no PIO fallback.
 */
struct iosnand_mtd_read_req_ecc_stats {
    __u32 uncorrectable_errors;
    __u32 corrected_bitflips;
    __u32 max_bitflips;
};
struct iosnand_mtd_read_req {
    __u64 start;
    __u64 len;
    __u64 ooblen;
    __u64 usr_data;
    __u64 usr_oob;
    __u8 mode;
    __u8 padding[7];
    struct iosnand_mtd_read_req_ecc_stats ecc_stats;
};
#define IOSNAND_MEMREAD _IOWR('M', 26, struct iosnand_mtd_read_req)
_Static_assert(sizeof(struct iosnand_mtd_read_req) == 64, "Linux 6.12 MEMREAD ABI size");
_Static_assert(offsetof(struct iosnand_mtd_read_req, mode) == 40, "MEMREAD mode offset");
_Static_assert(offsetof(struct iosnand_mtd_read_req, ecc_stats) == 48, "MEMREAD ECC offset");
#ifdef MEMREAD
_Static_assert(MEMREAD == IOSNAND_MEMREAD, "native MEMREAD ioctl must match 6.12 ABI");
_Static_assert(sizeof(struct mtd_read_req) == sizeof(struct iosnand_mtd_read_req),
               "native MEMREAD struct size");
_Static_assert(offsetof(struct mtd_read_req, ecc_stats) ==
               offsetof(struct iosnand_mtd_read_req, ecc_stats), "native MEMREAD ECC offset");
#endif

#ifndef IOSNAND_IOCTL
#define IOSNAND_IOCTL ioctl
#endif

#define IOSNAND_MTD_DATA (4 * 4096)
#define IOSNAND_MTD_OOB (4 * 16)
#define IOSNAND_MTD_ERASE (PPN_PAGES * IOSNAND_MTD_DATA)
#define IOSNAND_MTD_SIZE ((uint64_t)PPN_BLOCKS * IOSNAND_MTD_ERASE)

static int mtd_fds[PPN_DIES] = { -1, -1, -1, -1 };

static int mtd_geometry_ok(const struct mtd_info_user *info, uint64_t size)
{
    /* MEMGETINFO.size is only 32 bits!  Read the authoritative 64-bit size
     * from sysfs too; each fixed-geometry die is larger than 4 GiB. */
    return size == IOSNAND_MTD_SIZE && info->size == (uint32_t)size &&
        /* Reject old SLC-mislabelled kernels and any SLC-emulated mapping:
         * this backend expects the driver's exact 256-page MLC geometry. */
        info->type == MTD_MLCNANDFLASH &&
        !(info->flags & (MTD_WRITEABLE | MTD_SLC_ON_MLC_EMULATION)) &&
        info->writesize == IOSNAND_MTD_DATA &&
        info->erasesize == IOSNAND_MTD_ERASE && info->oobsize == IOSNAND_MTD_OOB;
}

static int mtd_sysfs_text(const char *entry, const char *attribute, char *buf, size_t len)
{
    char path[PATH_MAX];
    if (!len || len > INT_MAX) { errno = EINVAL; return -1; }
    if (snprintf(path, sizeof(path), "/sys/class/mtd/%s/%s", entry, attribute) >= (int)sizeof(path)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    FILE *f = fopen(path, "r");
    if (!f)
        return -1;
    errno = 0;
    int ok = fgets(buf, len, f) && (feof(f) || strchr(buf, '\n'));
    int saved = errno, read_error = ferror(f);
    int closed = fclose(f);
    if (!ok) { errno = read_error && saved ? saved : EINVAL; return -1; }
    if (closed) return -1;
    buf[strcspn(buf, "\r\n")] = 0;
    return 0;
}

/* The mtdchar minor selects index>>1, not the sysfs name. Geometry alone
 * cannot distinguish two identical dies when a device node is misbound. */
static int mtd_sysfs_dev(const char *entry, dev_t *device)
{
    char text[64], *end;
    if (mtd_sysfs_text(entry, "dev", text, sizeof(text))) return -1;
    if (!isdigit((unsigned char)text[0])) { errno = EINVAL; return -1; }
    errno = 0;
    unsigned long maj = strtoul(text, &end, 10);
    if (errno || *end != ':' || !isdigit((unsigned char)end[1])) {
        if (!errno) errno = EINVAL;
        return -1;
    }
    unsigned long min = strtoul(end + 1, &end, 10);
    if (errno || *end || maj > UINT_MAX || min > UINT_MAX) {
        if (!errno) errno = EINVAL;
        return -1;
    }
    dev_t expected = makedev(maj, min);
    if (major(expected) != maj || minor(expected) != min || maj != 90) {
        errno = EINVAL;
        return -1;
    }
    *device = expected;
    return 0;
}

static void mtd_backend_close(void)
{
    for (int die = 0; die < PPN_DIES; die++) {
        if (mtd_fds[die] >= 0)
            close(mtd_fds[die]);
        mtd_fds[die] = -1;
    }
}

static int mtd_backend_open(void)
{
    DIR *directory = opendir("/sys/class/mtd");
    if (!directory)
        return -1;
    struct dirent *entry;
    int result = -1;
    while ((entry = readdir(directory))) {
        char name[64], size_text[64], path[PATH_MAX], *end;
        if (strncmp(entry->d_name, "mtd", 3) || !entry->d_name[3] ||
            strspn(entry->d_name + 3, "0123456789") != strlen(entry->d_name + 3))
            continue;
        if (mtd_sysfs_text(entry->d_name, "name", name, sizeof(name)))
            continue;
        int die = -1;
        for (int i = 0; i < PPN_DIES; i++) {
            char expected[32];
            snprintf(expected, sizeof(expected), "apple-nand%d", i);
            if (!strcmp(expected, name))
                die = i;
        }
        if (die < 0)
            continue;
        if (mtd_fds[die] >= 0) { errno = EINVAL; goto out; }
        if (mtd_sysfs_text(entry->d_name, "size", size_text, sizeof(size_text))) goto out;
        errno = 0;
        uint64_t size = strtoull(size_text, &end, 10);
        if (errno || !*size_text || *size_text == '-' || *end) {
            if (!errno) errno = EINVAL;
            goto out;
        }
        if (snprintf(path, sizeof(path), "/dev/%s", entry->d_name) >= (int)sizeof(path)) {
            errno = ENAMETOOLONG;
            goto out;
        }
        dev_t expected_device;
        if (mtd_sysfs_dev(entry->d_name, &expected_device)) goto out;
        int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        if (fd < 0)
            goto out;
        mtd_fds[die] = fd;
        struct stat st;
        struct mtd_info_user info;
        if (fstat(fd, &st)) goto out;
        if (!S_ISCHR(st.st_mode) || st.st_rdev != expected_device) {
            errno = ENODEV;
            goto out;
        }
        int status = IOSNAND_IOCTL(fd, MEMGETINFO, &info);
        if (status < 0) goto out;
        if (status > 0) { errno = EIO; goto out; }
        if (!mtd_geometry_ok(&info, size)) { errno = EINVAL; goto out; }
    }
    for (int die = 0; die < PPN_DIES; die++)
        if (mtd_fds[die] < 0) { errno = ENODEV; goto out; }
    result = 0;
out: {
        int saved = errno;
        closedir(directory);
        if (result) mtd_backend_close();
        errno = saved;
        return result;
    }
}

static int mtd_backend_read(int die, int block, int page, uint8_t *raw, uint32_t len, uint8_t *status)
{
    uint8_t data[IOSNAND_MTD_DATA], oob[IOSNAND_MTD_OOB];
    *status = 0;
    if (die < 0 || die >= PPN_DIES || block < 0 || block >= PPN_BLOCKS ||
        page < 0 || page >= PPN_PAGES || len > PPN_PAGE_RAW || mtd_fds[die] < 0) {
        errno = EINVAL;
        return -1;
    }
    uint64_t offset = ((uint64_t)block * PPN_PAGES + page) * IOSNAND_MTD_DATA;
    struct iosnand_mtd_read_req request = {
        .start = offset, .len = sizeof(data), .ooblen = sizeof(oob),
        .usr_data = (uintptr_t)data, .usr_oob = (uintptr_t)oob,
        .mode = MTD_OPS_PLACE_OOB,
    };
    int ret = IOSNAND_IOCTL(mtd_fds[die], IOSNAND_MEMREAD, &request);
    int error = ret < 0 ? errno : 0;
    if (ret > 0) {
        errno = EIO;
        return -1;
    }
    if (error == EBADMSG) {
        /* The H2FMI driver reports retired and other unreadable pages as
         * EBADMSG.  Only _block_isbad's explicit 0x45 result is retirement. */
        int64_t block_offset = (int64_t)block * IOSNAND_MTD_ERASE;
        int bad = IOSNAND_IOCTL(mtd_fds[die], MEMGETBADBLOCK, &block_offset);
        if (bad == 1) {
            memset(raw, 0xff, len);
            *status = 0x45;
            return 0;
        }
        if (bad < 0) {
            int query_error = errno;
            fprintf(stderr, "iosnand: MTD die %d block %d: read failed (%s), bad-block query failed (%s)\n",
                    die, block, strerror(error), strerror(query_error));
            errno = query_error;
            return -1;
        }
        errno = EBADMSG;
        return -1;
    }
    if (error && error != EUCLEAN)
        return -1;
    if (request.len != sizeof(data) || request.ooblen != sizeof(oob) ||
        request.ecc_stats.uncorrectable_errors) {
        errno = EIO;
        return -1;
    }
    /* MTD does not expose physical PPN 0x49.  All-FF bytes are not proof of
     * erase completion: return readable data and let the FTL ignore FF
     * metadata.  iosnand disables its erased-page fast scan in this mode. */
    *status = error == EUCLEAN || request.ecc_stats.max_bitflips ||
        request.ecc_stats.corrected_bitflips ? 0x42 : 0x40;
    /* Reassemble only the requested prefix of the PPN raw layout. */
    for (unsigned c = 0; c < 4; c++) {
        uint8_t chunk[PPN_CHUNK];
        memcpy(chunk, data + c * 4096, 1024);
        memcpy(chunk + 1024, oob + c * 16, 16);
        memcpy(chunk + 1040, data + c * 4096 + 1024, 3072);
        unsigned off = c * PPN_CHUNK;
        if (off < len)
            memcpy(raw + off, chunk, len - off < PPN_CHUNK ? len - off : PPN_CHUNK);
    }
    return 0;
}
#endif
