/* hfs-state DEVICE -- may Linux write this HFS+ volume?  (iOS's System, on the iPad 2)
 *
 * Linux's hfsplus neither replays nor writes an HFS+ journal: it mounts a
 * journaled volume read-write only with -o force, and then any transaction iOS
 * left in the journal would be lost, or replayed by iOS over what Linux wrote.
 * So the answer is yes only when the volume is cleanly unmounted (iOS's
 * `mount -u -r /` before kloader does that: it flushes and closes the journal)
 * and its journal holds nothing: start == end in the journal header.
 *
 * Prints one line (signature, attributes, last mounted by, journal state) and
 * exits 0 for "may be written", 1 for "read-only", 2 when it cannot tell.
 * Reads only, past the page cache (O_DIRECT): hfsplus writes its volume
 * header with bios of its own, so the block device's cache may still hold
 * what an earlier read saw -- it once showed 'HFSJ' after Linux's own mount
 * had written 'H+Lx'.  Fields as Apple's TN1150 and xnu's hfs_journal have them.
 *
 * Build: arm-linux-gnueabihf-gcc -static -O2 -Wall -o hfs-state tools/hfs-state.c
 */
#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64
#include <fcntl.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <linux/fs.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define VOL_UNMOUNTED   (1u << 8)       /* kHFSVolumeUnmountedBit */
#define VOL_INCONSIST   (1u << 11)      /* kHFSBootVolumeInconsistentBit (Linux: INCNSTNT) */
#define VOL_JOURNALED   (1u << 13)      /* kHFSVolumeJournaledBit */
#define VOL_SOFTLOCK    (1u << 15)
#define JI_IN_FS        1u              /* kJIJournalInFSMask */
#define JI_OTHER_DEV    2u
#define JI_NEED_INIT    4u
#define JNL_MAGIC       0x4a4e4c78u     /* "JNLx" */
#define JNL_ENDIAN      0x12345678u

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static uint64_t be64(const uint8_t *p) { return (uint64_t)be32(p) << 32 | be32(p + 4); }
static uint32_t le32(const uint8_t *p) { return (uint32_t)p[3] << 24 | p[2] << 16 | p[1] << 8 | p[0]; }
static uint64_t le64(const uint8_t *p) { return (uint64_t)le32(p + 4) << 32 | le32(p); }

/* N bytes at OFF: whole aligned 4 KB blocks read with O_DIRECT, the part wanted copied out */
static int dfd = -1;
static int rd(int fd, uint64_t off, void *buf, size_t n)
{
    static uint8_t *blk;
    if (!blk && posix_memalign((void **)&blk, 4096, 16384))
        return -1;
    uint64_t a = off & ~4095ull, e = (off + n + 4095) & ~4095ull;
    if (dfd >= 0 && e - a <= 16384 && pread(dfd, blk, e - a, (off_t)a) == (ssize_t)(e - a)) {
        memcpy(buf, blk + (off - a), n);
        return 0;
    }
    return pread(fd, buf, n, (off_t)off) == (ssize_t)n ? 0 : -1;    /* no O_DIRECT here: the cache, flushed */
}

int main(int argc, char **argv)
{
    uint8_t vh[512], ji[512], jh[64];
    if (argc != 2) {
        fprintf(stderr, "usage: hfs-state DEVICE\n");
        return 2;
    }
    int fd = open(argv[1], O_RDONLY);
    if (fd < 0) {
        perror(argv[1]);
        return 2;
    }
    dfd = open(argv[1], O_RDONLY | O_DIRECT);
    if (dfd < 0)
        ioctl(fd, BLKFLSBUF, 0);
    if (rd(fd, 1024, vh, sizeof vh)) {
        printf("%s: the volume header does not read\n", argv[1]);
        return 2;
    }
    char sig[3] = { (char)vh[0], (char)vh[1], 0 };
    if (strcmp(sig, "H+") && strcmp(sig, "HX")) {
        printf("%s: no HFS+ volume header (signature %02x%02x)\n", argv[1], vh[0], vh[1]);
        return 2;
    }
    uint32_t attr = be32(vh + 4), jib = be32(vh + 12), bsz = be32(vh + 40);
    char lmv[5] = { (char)vh[8], (char)vh[9], (char)vh[10], (char)vh[11], 0 };
    for (int i = 0; i < 4; i++)
        if (lmv[i] < 32 || lmv[i] > 126)
            lmv[i] = '?';
    printf("%s: %s, last mounted by '%s', %s%s%s", argv[1], sig, lmv,
           attr & VOL_UNMOUNTED ? "cleanly unmounted" : "NOT cleanly unmounted",
           attr & VOL_INCONSIST ? ", marked inconsistent" : "", attr & VOL_SOFTLOCK ? ", locked" : "");
    int ok = (attr & VOL_UNMOUNTED) && !(attr & (VOL_INCONSIST | VOL_SOFTLOCK));
    if (!(attr & VOL_JOURNALED)) {
        printf(", not journaled\n");
        return ok ? 0 : 1;
    }
    if (!bsz || !jib || rd(fd, (uint64_t)jib * bsz, ji, sizeof ji)) {
        printf(", journaled, but its journal info block (%u) does not read\n", jib);
        return 2;
    }
    uint32_t flags = be32(ji);
    uint64_t joff = be64(ji + 36), jsize = be64(ji + 44);
    if (!(flags & JI_IN_FS) || (flags & JI_OTHER_DEV)) {
        printf(", journaled on another device (flags %x)\n", flags);
        return 2;
    }
    if (flags & JI_NEED_INIT) {
        printf(", journal not initialized yet (empty)\n");
        return ok ? 0 : 1;
    }
    if (rd(fd, joff, jh, sizeof jh)) {
        printf(", journal at %llu does not read\n", (unsigned long long)joff);
        return 2;
    }
    uint64_t start, end;
    if (le32(jh) == JNL_MAGIC && le32(jh + 4) == JNL_ENDIAN) {
        start = le64(jh + 8);
        end = le64(jh + 16);
    } else if (be32(jh) == JNL_MAGIC && be32(jh + 4) == JNL_ENDIAN) {
        start = be64(jh + 8);
        end = be64(jh + 16);
    } else {
        printf(", journal header without its magic (%02x%02x%02x%02x)\n", jh[0], jh[1], jh[2], jh[3]);
        return 2;
    }
    printf(", journal %llu KB at %llu: %s\n", (unsigned long long)(jsize >> 10), (unsigned long long)joff,
           start == end ? "empty" : "HOLDS TRANSACTIONS");
    return ok && start == end ? 0 : 1;
}
