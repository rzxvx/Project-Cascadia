// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/magic.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <unistd.h>
#include "ppn.h"

struct fake {
    int fail_root, fail_child, fail_fs, fail_fdopendir, fail_read, fail_stat, fail_dirfd;
    int fail_close, fail_closedir;
    bool child_absent, wrong_root_fs, wrong_child_fs;
    unsigned int checks, mode, second_mode, index, opens, maps, reads, locks;
    unsigned int close_count[32], live[32], expected_errno;
    int exited;
} f;
static jmp_buf escaped;
static struct dirent ent;
static uint32_t regs[7][1024], gates[1024];
static char fake_dir;

int fake_open(const char *name, int flags, ...)
{
    if (!strcmp(name, "/sys/bus/platform/drivers")) {
        f.checks++;
        assert(flags == (O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
        if (f.fail_root) { errno = f.fail_root; return -1; }
        f.live[10] = 1;
        return 10;
    }
    if (!strcmp(name, "/dev/mem")) {
        assert(flags == (O_RDWR | O_SYNC));
        f.opens++;
        return 20;
    }
    assert(!strcmp(name, "/tmp/nandctl-bus0.lock"));
    assert(flags == (O_RDWR | O_CREAT));
    f.locks++;
    return 21;
}
int fake_openat(int fd, const char *name, int flags, ...)
{
    assert(fd == 10 && f.live[10] && !strcmp(name, "apple-h2fmi"));
    assert(flags == (O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
    if (f.fail_child || f.child_absent) { errno = f.fail_child ? f.fail_child : ENOENT; return -1; }
    f.live[11] = 1;
    return 11;
}
int fake_fstatfs(int fd, struct statfs *s)
{
    assert((fd == 10 || fd == 11) && f.live[fd]);
    if (f.fail_fs) { errno = f.fail_fs; return -1; }
    memset(s, 0, sizeof(*s));
    s->f_type = ((fd == 10 && f.wrong_root_fs) || (fd == 11 && f.wrong_child_fs)) ? 0x1234 : SYSFS_MAGIC;
    return 0;
}
DIR *fake_fdopendir(int fd)
{
    assert(fd == 11 && f.live[11]);
    if (f.fail_fdopendir) { errno = f.fail_fdopendir; return NULL; }
    f.index = 0;
    return (DIR *)&fake_dir;
}
struct dirent *fake_readdir(DIR *d)
{
    static const char *names[] = {".", "..", "bind", "unbind", "uevent", "module", "31200000.flash-controller0"};
    assert(d == (DIR *)&fake_dir && f.live[11]);
    if (f.fail_read) { errno = f.fail_read; return NULL; }
    unsigned int mode = f.checks > 1 && f.second_mode ? f.second_mode : f.mode;
    unsigned int count = mode ? 7 : 6;
    if (f.index >= count) return NULL;
    memset(&ent, 0, sizeof(ent));
    strcpy(ent.d_name, names[f.index++]);
    if (mode == 2 && f.index == 7) strcpy(ent.d_name, "unexpected");
    return &ent;
}
int fake_dirfd(DIR *d)
{
    assert(d == (DIR *)&fake_dir && f.live[11]);
    if (f.fail_dirfd) { errno = f.fail_dirfd; return -1; }
    return 11;
}
int fake_fstatat(int fd, const char *name, struct stat *s, int flags)
{
    if (fd < 0) { errno = EBADF; return -1; }
    assert(fd == 11 && f.live[11] && flags == AT_SYMLINK_NOFOLLOW);
    if (f.fail_stat) { errno = f.fail_stat; return -1; }
    memset(s, 0, sizeof(*s));
    s->st_mode = (!strcmp(name, "module") || !strncmp(name, "31200000.", 9)) ? S_IFLNK : S_IFREG;
    if (f.mode == 3 && !strcmp(name, "module")) s->st_mode = S_IFREG;
    if (f.mode == 4 && !strcmp(name, "bind")) s->st_mode = S_IFLNK;
    return 0;
}
int fake_close(int fd)
{
    assert((fd == 10 || fd == 11) && f.live[fd]);
    f.live[fd] = 0;
    f.close_count[fd]++;
    if (f.fail_close) { errno = f.fail_close; return -1; }
    return 0;
}
int fake_closedir(DIR *d)
{
    assert(d == (DIR *)&fake_dir && f.live[11]);
    f.live[11] = 0;
    f.close_count[11]++;
    if (f.fail_closedir) { errno = f.fail_closedir; return -1; }
    return 0;
}
void *fake_mmap(void *p, size_t n, int prot, int flags, int fd, off_t off)
{
    (void)off;
    assert(!p && n == 4096 && prot == (PROT_READ | PROT_WRITE) && flags == MAP_SHARED && fd == 20);
    assert(f.maps < 7);
    return regs[f.maps++];
}
int fake_flock(int fd, int op)
{
    assert(fd == 21 && op == (LOCK_EX | LOCK_NB));
    return 0;
}
_Noreturn void fake_exit(int status)
{
    f.exited = status;
    longjmp(escaped, 1);
}
uint32_t fake_read(volatile uint32_t *base, uint32_t offset)
{
    assert(base == gates && (offset == 0xc4 || offset == 0xc8));
    f.reads++;
    return 0xf0;
}
#undef PPN_R
#define PPN_R fake_read
#define open fake_open
#define openat fake_openat
#define fstatfs fake_fstatfs
#define fdopendir fake_fdopendir
#define readdir fake_readdir
#define dirfd fake_dirfd
#define fstatat fake_fstatat
#define close fake_close
#define closedir fake_closedir
#define mmap fake_mmap
#define flock fake_flock
#define exit fake_exit
#include "ppn.c"
#undef exit

static void fresh(void)
{
    memset(&f, 0, sizeof(f));
    ppn_pmgr = gates;
}
int main(void)
{
    volatile unsigned int cases = 0;
#ifndef HISTORICAL_CONTROL
    for (unsigned int mode = 0; mode < 5; mode++) {
        fresh(); f.mode = mode;
        int ret = ppn_kernel_owner_guard();
        assert(ret == (mode == 0 ? 0 : mode == 1 ? -EBUSY : -EPROTO));
        assert(!f.live[10] && !f.live[11] && f.close_count[10] == 1 && f.close_count[11] == 1);
        assert(!f.opens && !f.maps && !f.reads && !f.locks);
        cases++;
    }
    for (unsigned int fail = 0; fail < 12; fail++) {
        fresh();
        switch (fail) {
        case 0: f.fail_root = ENOENT; break;
        case 1: f.fail_root = EACCES; break;
        case 2: f.fail_child = EACCES; break;
        case 3: f.fail_child = ELOOP; break;
        case 4: f.fail_fs = EIO; break;
        case 5: f.wrong_root_fs = true; break;
        case 6: f.wrong_child_fs = true; break;
        case 7: f.fail_fdopendir = ENOMEM; break;
        case 8: f.fail_read = EIO; break;
        case 9: f.fail_stat = ENOENT; break;
        case 10: f.fail_closedir = EIO; break;
        case 11: f.fail_close = EIO; break;
        }
        assert(ppn_kernel_owner_guard() < 0);
        assert(!f.live[10] && !f.live[11] && f.close_count[10] <= 1 && f.close_count[11] <= 1);
        assert(!f.opens && !f.maps && !f.reads && !f.locks);
        cases++;
    }
    fresh(); f.child_absent = true;
    assert(!ppn_kernel_owner_guard() && !f.live[10] && !f.live[11]);
    cases++;
    fresh(); f.fail_dirfd = EBADF;
    assert(ppn_kernel_owner_guard() == -EBADF && !f.live[10] && !f.live[11]);
    cases++;
#endif
    fresh(); f.mode = 1;
    if (!setjmp(escaped)) {
        assert(!ppn_open());
#ifdef HISTORICAL_CONTROL
        assert(f.opens == 1 && f.maps == 7 && !f.exited);
#else
        assert(!"bound driver raw open should exit before /dev/mem");
#endif
    } else {
        assert(f.exited == 1 && !f.opens && !f.maps);
    }
    cases++;
    fresh(); f.mode = 1;
    int claimed = ppn_claim(0);
#ifdef HISTORICAL_CONTROL
    assert(claimed && f.reads == 2 && f.locks == 1);
#else
    assert(!claimed && !f.reads && !f.locks);
#endif
    cases++;
#ifndef HISTORICAL_CONTROL
    fresh(); f.second_mode = 1;
    assert(!ppn_claim(0) && f.reads == 2 && f.locks == 1 && f.checks == 2);
    cases++;
    fresh(); f.second_mode = 1;
    if (!setjmp(escaped)) {
        ppn_open();
        assert(!"bind during mapping must be rejected before caller register use");
    } else {
        assert(f.exited == 1 && f.opens == 1 && f.maps == 7 && f.checks == 2);
    }
    cases++;
    fresh();
    assert(ppn_claim(0) && f.reads == 2 && f.locks == 1 && f.checks == 2);
    cases++;
    fresh();
    assert(!ppn_open() && f.opens == 1 && f.maps == 7 && f.checks == 2);
    cases++;
#endif
    printf("PASS %u actual raw-owner caller/guard cases, hardware_calls=0\n", cases);
    return 0;
}
