// SPDX-License-Identifier: GPL-2.0
/*
 * cascadia-storage - runs as root on the iPad's own iPadOS (jailbroken by
 * the user: palera1n, Dopamine, ...) and gives Linux a partition of its own
 * on the internal storage. Driven from the Mac by ./cascadia a10 storage,
 * over SSH.
 *
 *   cascadia-storage inspect               read-only: the GPT (both copies,
 *                                          CRCs), the APFS container, the
 *                                          smallest size APFS can shrink to
 *   cascadia-storage plan  <linux-bytes>   read-only: what apply would do
 *   cascadia-storage apply <linux-bytes> --i-have-a-backup
 *                                          shrink APFS, then add a Linux
 *                                          partition to the GPT
 *
 * apply, in this order (as OrchardLinux's resize-apfs does it, MIT):
 *  1. APFSContainerResize (iPadOS's own APFS, private APFS.framework) to the
 *     planned size; the container's superblock is read back.
 *  2. The GPT: the APFS entry ends where the container now ends, a new
 *     "Linux filesystem" entry takes the space after it (1 MiB aligned).
 *     The backup copy is written first, then the primary; both are read
 *     back. Nothing is formatted: the Linux partition is empty space.
 * If step 1 fails nothing was changed; if step 2 fails the container is
 * smaller than its partition, which APFS does not mind.
 *
 * Refused: anything but one APFS partition and free space after it in the
 * layout below, bad CRCs or differing GPT copies, an APFS size under
 * APFS's own minimum + MARGIN.
 *
 * Output is "key=value" lines for the host; "error=..." and a non-zero exit
 * on anything unexpected.
 */
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

/* Darwin's disk ioctls (XNU bsd/sys/disk.h) */
#define DKIOCSYNCHRONIZECACHE _IO('d', 22)
#define DKIOCGETBLOCKSIZE     _IOR('d', 24, uint32_t)
#define DKIOCGETBLOCKCOUNT    _IOR('d', 25, uint64_t)

#define APFS_FRAMEWORK "/System/Library/PrivateFrameworks/APFS.framework/APFS"
#define APFS_TYPE  "7C3457EF-0000-11AA-AA11-00306543ECAC"
/* Linux filesystem data, 0FC63DAF-8483-4772-8E79-3D69D8477DE4, on disk */
static const uint8_t LINUX_TYPE[16] = {
    0xaf, 0x3d, 0xc6, 0x0f, 0x83, 0x84, 0x72, 0x47,
    0x8e, 0x79, 0x3d, 0x69, 0xd8, 0x47, 0x7d, 0xe4,
};
#define ALIGN_BYTES (1ULL << 20)          /* the Linux partition's start */
#define MARGIN      (2ULL << 30)          /* APFS keeps this over its minimum */
#define MIN_LINUX   (1ULL << 30)

static int fail(const char *what)
{
    printf("error=%s: %s\n", what, strerror(errno));
    return 1;
}

static int refuse(const char *why)
{
    printf("error=%s\n", why);
    return 1;
}

static uint32_t crc32(const uint8_t *p, size_t n)
{
    uint32_t c = 0xffffffff;
    while (n--) {
        c ^= *p++;
        for (int k = 0; k < 8; k++)
            c = (c >> 1) ^ (0xedb88320 & -(c & 1));
    }
    return ~c;
}

static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t le64(const uint8_t *p) { return le32(p) | (uint64_t)le32(p + 4) << 32; }
static void put32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = v >> (8 * i); }
static void put64(uint8_t *p, uint64_t v) { for (int i = 0; i < 8; i++) p[i] = v >> (8 * i); }

/* a GPT GUID as text: the first three fields little-endian */
static void guid(const uint8_t *g, char *out)
{
    sprintf(out, "%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X",
            le32(g), g[4] | g[5] << 8, g[6] | g[7] << 8, g[8], g[9],
            g[10], g[11], g[12], g[13], g[14], g[15]);
}

static int read_at(int fd, void *buf, size_t len, uint64_t off)
{
    return pread(fd, buf, len, (off_t)off) == (ssize_t)len ? 0 : -1;
}

static int write_at(int fd, const void *buf, size_t len, uint64_t off)
{
    return pwrite(fd, buf, len, (off_t)off) == (ssize_t)len ? 0 : -1;
}

/* ---------- the layout ---------------------------------------------------- */

struct gpt {
    uint8_t *header;                /* one sector */
    uint8_t *entries;               /* entries_sectors sectors */
    uint64_t self, alt, first, last, entries_lba;
    uint32_t count, esize, hsize;
    int header_ok, entries_ok;
};

struct layout {
    int fd;
    uint32_t sector;
    uint64_t sectors;
    struct gpt p, b;
    size_t entries_len, entries_sectors;
    int apfs_index;                 /* 1-based GPT entry, also disk0sN */
    uint64_t apfs_first, apfs_last; /* LBAs */
    char bsd[16];
    uint32_t nx_block_size;
    uint64_t nx_blocks;
    int64_t minimal;
};

static int read_gpt(struct layout *l, uint64_t lba, struct gpt *g)
{
    uint32_t s = l->sector;
    memset(g, 0, sizeof(*g));
    g->header = malloc(s);
    if (!g->header || read_at(l->fd, g->header, s, lba * s))
        return -1;
    uint8_t *h = g->header;
    if (memcmp(h, "EFI PART", 8)) { errno = EINVAL; return -1; }
    g->hsize = le32(h + 12);
    if (g->hsize < 92 || g->hsize > s) { errno = EINVAL; return -1; }
    uint8_t tmp[512];
    memcpy(tmp, h, g->hsize > sizeof(tmp) ? sizeof(tmp) : g->hsize);
    memset(tmp + 16, 0, 4);
    g->header_ok = g->hsize <= sizeof(tmp) && crc32(tmp, g->hsize) == le32(h + 16);
    g->self = le64(h + 24);
    g->alt = le64(h + 32);
    g->first = le64(h + 40);
    g->last = le64(h + 48);
    g->entries_lba = le64(h + 72);
    g->count = le32(h + 80);
    g->esize = le32(h + 84);
    if (g->esize != 128 || g->count < 4 || g->count > 1024) { errno = EINVAL; return -1; }
    l->entries_len = (size_t)g->count * g->esize;
    l->entries_sectors = (l->entries_len + s - 1) / s;
    g->entries = malloc(l->entries_sectors * s);
    if (!g->entries || read_at(l->fd, g->entries, l->entries_sectors * s, g->entries_lba * s))
        return -1;
    g->entries_ok = crc32(g->entries, l->entries_len) == le32(h + 88);
    return 0;
}

static int nx_read(struct layout *l)
{
    char dev[32];
    uint8_t nx[4096];
    snprintf(dev, sizeof(dev), "/dev/r%s", l->bsd);
    int fd = open(dev, O_RDONLY | O_CLOEXEC);
    if (fd < 0 || read_at(fd, nx, sizeof(nx), 0)) {
        if (fd >= 0) close(fd);
        return -1;
    }
    close(fd);
    if (memcmp(nx + 32, "NXSB", 4)) { errno = EINVAL; return -1; }
    l->nx_block_size = le32(nx + 36);
    l->nx_blocks = le64(nx + 40);
    return 0;
}

static void *apfs_fn(const char *name)
{
    static void *h;
    if (!h)
        h = dlopen(APFS_FRAMEWORK, RTLD_LOCAL | RTLD_LAZY);
    return h ? dlsym(h, name) : NULL;
}

/* read everything; print it when verbose; refuse a layout we do not know */
static int load(struct layout *l, int verbose, int writable)
{
    memset(l, 0, sizeof(*l));
    /* writes: the same shared-lock open as GPT fdisk / resize-apfs on Darwin */
    l->fd = open("/dev/rdisk0", writable ? (O_RDWR | O_SHLOCK | O_CLOEXEC) : (O_RDONLY | O_CLOEXEC));
    if (l->fd < 0)
        return fail("open /dev/rdisk0");
    if (ioctl(l->fd, DKIOCGETBLOCKSIZE, &l->sector) || ioctl(l->fd, DKIOCGETBLOCKCOUNT, &l->sectors))
        return fail("disk geometry");
    if (verbose)
        printf("disk=disk0 sector=%u sectors=%llu bytes=%llu\n", l->sector,
               (unsigned long long)l->sectors, (unsigned long long)l->sectors * l->sector);
    if (read_gpt(l, 1, &l->p))
        return fail("primary GPT");
    if (read_gpt(l, l->sectors - 1, &l->b))
        return fail("backup GPT");
    int same = !memcmp(l->p.entries, l->b.entries, l->entries_len);
    if (verbose) {
        char dg[40];
        guid(l->p.header + 56, dg);
        printf("gpt.primary header_crc=%s entries_crc=%s self=%llu alt=%llu entries_lba=%llu\n",
               l->p.header_ok ? "ok" : "BAD", l->p.entries_ok ? "ok" : "BAD",
               (unsigned long long)l->p.self, (unsigned long long)l->p.alt,
               (unsigned long long)l->p.entries_lba);
        printf("gpt.backup header_crc=%s entries_crc=%s self=%llu alt=%llu entries_lba=%llu\n",
               l->b.header_ok ? "ok" : "BAD", l->b.entries_ok ? "ok" : "BAD",
               (unsigned long long)l->b.self, (unsigned long long)l->b.alt,
               (unsigned long long)l->b.entries_lba);
        printf("gpt disk_guid=%s first_usable=%llu last_usable=%llu entries=%u entry_size=%u copies_match=%s\n",
               dg, (unsigned long long)l->p.first, (unsigned long long)l->p.last,
               l->p.count, l->p.esize, same ? "yes" : "NO");
    }
    if (!l->p.header_ok || !l->p.entries_ok || !l->b.header_ok || !l->b.entries_ok || !same)
        return refuse("the GPT is not intact (CRC or copies differ)");
    if (l->p.self != 1 || l->p.alt != l->sectors - 1 || l->b.self != l->sectors - 1 ||
        l->b.alt != 1 || l->p.entries_lba != 2 || l->b.entries_lba + l->entries_sectors != l->sectors - 1)
        return refuse("an unexpected GPT layout");

    uint64_t used_end = l->p.first;
    int parts = 0;
    for (uint32_t i = 0; i < l->p.count; i++) {
        const uint8_t *e = l->p.entries + (size_t)i * l->p.esize;
        static const uint8_t zero[16];
        if (!memcmp(e, zero, 16))
            continue;
        parts++;
        char type[40], uniq[40], name[37] = "";
        guid(e, type);
        guid(e + 16, uniq);
        for (int k = 0; k < 36; k++) {
            uint16_t ch = e[56 + 2 * k] | e[57 + 2 * k] << 8;
            if (!ch) break;
            name[k] = ch < 128 ? (char)ch : '?';
        }
        uint64_t first = le64(e + 32), last = le64(e + 40);
        if (verbose)
            printf("part index=%u bsd=disk0s%u type=%s guid=%s first=%llu last=%llu bytes=%llu name=%s\n",
                   i + 1, i + 1, type, uniq, (unsigned long long)first, (unsigned long long)last,
                   (unsigned long long)(last - first + 1) * l->sector, name);
        if (!strcmp(type, APFS_TYPE) && !l->apfs_index) {
            l->apfs_index = i + 1;
            l->apfs_first = first;
            l->apfs_last = last;
        }
        if (last + 1 > used_end)
            used_end = last + 1;
    }
    if (verbose)
        printf("free_after_last_partition sectors=%llu bytes=%llu\n",
               (unsigned long long)(l->p.last + 1 - used_end),
               (unsigned long long)(l->p.last + 1 - used_end) * l->sector);
    if (!l->apfs_index)
        return refuse("no APFS container partition in the GPT");
    snprintf(l->bsd, sizeof(l->bsd), "disk0s%d", l->apfs_index);
    if (nx_read(l))
        return fail("APFS container superblock");
    if (verbose)
        printf("apfs bsd=%s block_size=%u blocks=%llu bytes=%llu\n", l->bsd, l->nx_block_size,
               (unsigned long long)l->nx_blocks,
               (unsigned long long)l->nx_blocks * l->nx_block_size);

    int (*minimal)(const char *, intptr_t *) =
        (int (*)(const char *, intptr_t *))apfs_fn("APFSContainerGetMinimalSize");
    if (!minimal)
        return refuse("APFSContainerGetMinimalSize not found in APFS.framework");
    intptr_t min = 0;
    int st = minimal(l->bsd, &min);
    if (verbose)
        printf("apfs.minimal status=%d bytes=%lld\n", st, (long long)min);
    if (st || min <= 0)
        return refuse("APFS gave no minimal size");
    l->minimal = min;
    (void)parts;
    return 0;
}

/* ---------- the plan ------------------------------------------------------ */

struct plan {
    uint64_t apfs_last, apfs_bytes, linux_first, linux_last, linux_bytes;
    int linux_index;
};

static int make_plan(const struct layout *l, uint64_t want, struct plan *pl)
{
    uint64_t s = l->sector, align = ALIGN_BYTES / s;
    memset(pl, 0, sizeof(*pl));
    if (l->apfs_last != l->p.last)
        return refuse("the APFS partition does not end at the disk's last usable sector "
                      "(a Linux partition may already exist)");
    if (l->nx_blocks * l->nx_block_size != (l->apfs_last - l->apfs_first + 1) * s)
        return refuse("the APFS container does not fill its partition");
    for (uint32_t i = 0; i < l->p.count && !pl->linux_index; i++) {
        static const uint8_t zero[16];
        if (!memcmp(l->p.entries + (size_t)i * l->p.esize, zero, 16) && (int)i + 1 > l->apfs_index)
            pl->linux_index = i + 1;
    }
    if (!pl->linux_index)
        return refuse("no free GPT entry after the APFS one");
    if (want < MIN_LINUX)
        return refuse("a Linux partition under 1 GiB");
    uint64_t end = l->p.last + 1;                       /* exclusive */
    uint64_t start = (end - want / s) / align * align;  /* Linux start, aligned down */
    pl->linux_first = start;
    pl->linux_last = l->p.last;
    pl->linux_bytes = (pl->linux_last - pl->linux_first + 1) * s;
    pl->apfs_last = start - 1;
    pl->apfs_bytes = (pl->apfs_last - l->apfs_first + 1) * s;
    if (pl->apfs_bytes % l->nx_block_size)
        return refuse("the new APFS size is not a whole number of APFS blocks");
    printf("plan apfs.bytes=%llu apfs.last=%llu linux.index=%d linux.first=%llu linux.last=%llu linux.bytes=%llu\n",
           (unsigned long long)pl->apfs_bytes, (unsigned long long)pl->apfs_last, pl->linux_index,
           (unsigned long long)pl->linux_first, (unsigned long long)pl->linux_last,
           (unsigned long long)pl->linux_bytes);
    printf("plan apfs.minimal=%lld apfs.margin_over_minimal=%lld\n", (long long)l->minimal,
           (long long)pl->apfs_bytes - (long long)l->minimal);
    if (pl->apfs_bytes < (uint64_t)l->minimal + MARGIN)
        return refuse("APFS would keep less than 2 GiB over its own minimum");
    return 0;
}

/* ---------- apply --------------------------------------------------------- */

static void rewrite_header(struct gpt *g, uint32_t entries_crc)
{
    put32(g->header + 88, entries_crc);
    put32(g->header + 16, 0);
    put32(g->header + 16, crc32(g->header, g->hsize));
}

static int write_gpt(struct layout *l, const struct plan *pl)
{
    uint32_t s = l->sector;
    /* the new entries, the same for both copies */
    uint8_t *e = l->p.entries;
    uint8_t *apfs = e + (size_t)(l->apfs_index - 1) * 128;
    put64(apfs + 40, pl->apfs_last);
    uint8_t *lx = e + (size_t)(pl->linux_index - 1) * 128;
    memset(lx, 0, 128);
    memcpy(lx, LINUX_TYPE, 16);
    arc4random_buf(lx + 16, 16);
    lx[16 + 7] = (lx[16 + 7] & 0x0f) | 0x40;    /* a version 4 GUID */
    lx[16 + 8] = (lx[16 + 8] & 0x3f) | 0x80;
    put64(lx + 32, pl->linux_first);
    put64(lx + 40, pl->linux_last);
    static const char name[] = "Linux";
    for (size_t k = 0; k < sizeof(name) - 1; k++)
        lx[56 + 2 * k] = name[k];
    memcpy(l->b.entries, e, l->entries_sectors * s);
    uint32_t crc = crc32(e, l->entries_len);
    rewrite_header(&l->p, crc);
    rewrite_header(&l->b, crc);

    /* backup first (entries, then header), then the primary */
    if (write_at(l->fd, l->b.entries, l->entries_sectors * s, l->b.entries_lba * s) ||
        write_at(l->fd, l->b.header, s, l->b.self * s))
        return fail("writing the backup GPT");
    fsync(l->fd);
    ioctl(l->fd, DKIOCSYNCHRONIZECACHE);
    if (write_at(l->fd, l->p.entries, l->entries_sectors * s, l->p.entries_lba * s) ||
        write_at(l->fd, l->p.header, s, l->p.self * s))
        return fail("writing the primary GPT");
    fsync(l->fd);
    ioctl(l->fd, DKIOCSYNCHRONIZECACHE);
    return 0;
}

static int cmd_apply(uint64_t want)
{
    struct layout l;
    struct plan pl;
    if (load(&l, 0, 1) || make_plan(&l, want, &pl))
        return 1;

    /* 1. APFS */
    int (*resize)(const char *, uint64_t) =
        (int (*)(const char *, uint64_t))apfs_fn("APFSContainerResize");
    if (!resize)
        return refuse("APFSContainerResize not found in APFS.framework");
    printf("step=apfs-resize from=%llu to=%llu\n",
           (unsigned long long)l.nx_blocks * l.nx_block_size, (unsigned long long)pl.apfs_bytes);
    fflush(stdout);
    int st = resize(l.bsd, pl.apfs_bytes);
    printf("apfs.resize status=%d\n", st);
    if (st)
        return refuse("APFSContainerResize failed: nothing was changed");
    if (nx_read(&l))
        return fail("APFS container superblock after the resize");
    printf("apfs.after blocks=%llu bytes=%llu\n", (unsigned long long)l.nx_blocks,
           (unsigned long long)l.nx_blocks * l.nx_block_size);
    if (l.nx_blocks * l.nx_block_size != pl.apfs_bytes)
        return refuse("the container is not the planned size: the GPT was left as it was");

    /* 2. GPT */
    printf("step=gpt\n");
    fflush(stdout);
    if (write_gpt(&l, &pl))
        return 1;

    /* 3. read it all back */
    struct layout r;
    if (load(&r, 1, 0))
        return refuse("the layout did not read back intact");
    const uint8_t *lx = r.p.entries + (size_t)(pl.linux_index - 1) * 128;
    const uint8_t *ap = r.p.entries + (size_t)(r.apfs_index - 1) * 128;
    if (memcmp(lx, LINUX_TYPE, 16) || le64(lx + 32) != pl.linux_first || le64(lx + 40) != pl.linux_last ||
        le64(ap + 40) != pl.apfs_last || r.nx_blocks * r.nx_block_size != pl.apfs_bytes)
        return refuse("the read-back layout is not the planned one");
    printf("done=verified linux=disk0s%d first=%llu last=%llu bytes=%llu\n", pl.linux_index,
           (unsigned long long)pl.linux_first, (unsigned long long)pl.linux_last,
           (unsigned long long)pl.linux_bytes);
    return 0;
}

int main(int argc, char **argv)
{
    struct layout l;
    struct plan pl;
    if (argc >= 2 && geteuid()) {
        printf("error=run as root\n");
        return 1;
    }
    if (argc == 2 && !strcmp(argv[1], "inspect"))
        return load(&l, 1, 0);
    if (argc == 3 && !strcmp(argv[1], "plan"))
        return load(&l, 1, 0) || make_plan(&l, strtoull(argv[2], NULL, 10), &pl);
    if (argc == 4 && !strcmp(argv[1], "apply") && !strcmp(argv[3], "--i-have-a-backup"))
        return cmd_apply(strtoull(argv[2], NULL, 10));
    fprintf(stderr, "usage: cascadia-storage inspect\n"
                    "       cascadia-storage plan <linux-bytes>\n"
                    "       cascadia-storage apply <linux-bytes> --i-have-a-backup\n");
    return 2;
}
