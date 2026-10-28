/* Private iosnand process records and pidfd-only control for Linux 6.12.
 * The record is Linux-side runtime state, never an iOS/NAND write.
 * No signal-by-numeric-PID fallback exists. */
#ifndef IOSNAND_PROCESS_H
#define IOSNAND_PROCESS_H
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <sys/syscall.h>
#include <unistd.h>

#if !defined(SYS_pidfd_open) || !defined(SYS_pidfd_send_signal)
#error "Linux pidfd syscall definitions required; there is no PID-kill fallback"
#endif

struct process_record { uint32_t pid; uint64_t start, dev, ino, sectors; };

static int process_number(const char **cursor, uint64_t *value)
{
    const char *s = *cursor;
    if (!isdigit((unsigned char)*s)) { errno = EINVAL; return -1; }
    errno = 0;
    char *end;
    unsigned long long n = strtoull(s, &end, 10);
    if (errno || (*end && *end != ' ' && *end != '\n')) {
        if (!errno) errno = EINVAL;
        return -1;
    }
    *value = n; *cursor = end;
    return 0;
}

static int process_parse_record(const char *text, struct process_record *r)
{
    const char *p = text;
    uint64_t pid;
    int ready = !strncmp(p, "IOSNANDREADY1 ", 14);
    if (ready) p += 14;
    else if (!strncmp(p, "IOSNANDPID1 ", 12)) p += 12;
    else goto invalid;
    r->sectors = 0;
    if (process_number(&p, &pid) || *p++ != ' ' || pid <= 1 || pid > INT_MAX ||
        process_number(&p, &r->start) || *p++ != ' ' ||
        process_number(&p, &r->dev) || *p++ != ' ' ||
        process_number(&p, &r->ino)) goto invalid;
    if (ready && (*p++ != ' ' || process_number(&p, &r->sectors) || !r->sectors)) goto invalid;
    if (strcmp(p, "\n")) goto invalid;
    r->pid = pid;
    return 0;
invalid: errno = EINVAL; return -1;
}

/* Field 2 may contain spaces and parentheses; never use awk's $22. */
static int process_parse_stat(const char *text, uint32_t pid, uint64_t *start)
{
    const char *p = text;
    uint64_t found;
    if (process_number(&p, &found) || found != pid || p[0] != ' ' || p[1] != '(')
        goto invalid;
    p = strrchr(p + 2, ')');
    if (!p || p[1] != ' ') goto invalid;
    p += 2;
    for (unsigned field = 3; field < 22; field++) {
        if (!*p || *p == ' ' || *p == '\n') goto invalid;
        while (*p && *p != ' ') p++;
        if (*p++ != ' ') goto invalid;
    }
    if (process_number(&p, start)) goto invalid;
    return 0;
invalid: errno = EINVAL; return -1;
}

static int process_command(const char *buf, size_t len)
{
    const char *end = memchr(buf, 0, len);
    if (!end) return -1;
    size_t used = end - buf + 1;
    if (used >= len) return -1;
    const char *arg = buf + used;
    end = memchr(arg, 0, len - used);
    if (!end) return -1;
    if (!strcmp(arg, "--pid-record")) {
        used = end - buf + 1;
        if (used >= len) return -1;
        end = memchr(buf + used, 0, len - used); /* record pathname */
        if (!end || end == buf + used) return -1;
        used = end - buf + 1;
        if (used >= len) return -1;
        arg = buf + used;
        end = memchr(arg, 0, len - used);
        if (!end) return -1;
    }
    return strcmp(arg, "serve") ? -1 : 0;
}

static int process_read_at(int directory, const char *name, char *buf, size_t cap,
                           int record)
{
    int fd = openat(directory, name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) return -1;
    struct stat st;
    int ret = -1;
    if (fstat(fd, &st)) goto done;
    if (!S_ISREG(st.st_mode) ||
        (record && (st.st_uid || (st.st_mode & 07777) != 0600 || st.st_nlink != 1))) {
        errno = EINVAL;
        goto done;
    }
    size_t got = 0;
    while (got < cap) {
        ssize_t n = read(fd, buf + got, cap - got);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) goto done;
        if (!n) { ret = got; goto done; }
        got += n;
    }
    errno = EOVERFLOW;
done: {
        int saved = errno;
        if (close(fd) && ret >= 0) return -1;
        errno = saved;
        return ret;
    }
}

static int process_identity(uint32_t pid, struct process_record *r)
{
    char path[64], data[4096];
    snprintf(path, sizeof(path), "/proc/%u", pid);
    int fd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return -1;
    struct stat dir, exe;
    int ret = -1, n;
    if (fstat(fd, &dir)) goto done;
    if (dir.st_uid) { errno = EINVAL; goto done; }
    if (fstatat(fd, "exe", &exe, 0)) goto done;
    if (!S_ISREG(exe.st_mode)) { errno = EINVAL; goto done; }
    r->pid = pid; r->dev = exe.st_dev; r->ino = exe.st_ino; r->sectors = 0;
    n = process_read_at(fd, "stat", data, sizeof(data) - 1, 0);
    if (n < 0) goto done;
    if (memchr(data, 0, n)) { errno = EINVAL; goto done; }
    data[n] = 0;
    if (process_parse_stat(data, pid, &r->start)) goto done;
    n = process_read_at(fd, "cmdline", data, sizeof(data), 0);
    if (n < 0) goto done;
    if (process_command(data, n)) { errno = EINVAL; goto done; }
    ret = 0;
done: {
        int saved = errno;
        if (close(fd) && !ret) return -1;
        errno = saved;
        return ret;
    }
}

static int process_live(int pidfd)
{
    struct pollfd p = { .fd = pidfd, .events = POLLIN };
    int r;
    do { r = poll(&p, 1, 0); } while (r < 0 && errno == EINTR);
    if (r < 0) return -1;
    if (p.revents & POLLNVAL) { errno = EBADF; return -1; }
    if (p.revents & POLLERR) { errno = EIO; return -1; }
    if (p.revents & (POLLIN | POLLHUP)) { errno = ESRCH; return -1; }
    if (r || p.revents) { errno = EIO; return -1; }
    return 0;
}

static int process_bind(const struct process_record *r)
{
    if (r->pid <= 1 || r->pid > INT_MAX) { errno = EINVAL; return -1; }
    int fd = syscall(SYS_pidfd_open, (pid_t)r->pid, 0u);
    if (fd < 0) return -1;
    struct process_record actual;
    if (!process_live(fd) && !process_identity(r->pid, &actual)) {
        if (actual.start != r->start || actual.dev != r->dev || actual.ino != r->ino)
            errno = ESTALE;
        else if (!process_live(fd))
            return fd;
    }
    int saved = errno;
    close(fd);
    errno = saved;
    return -1;
}

/* Pin a root-private directory; all later file operations are relative to
 * this fd. No replacement, symlink traversal at the final component or FIFO. */
static int process_directory(const char *path, char name[NAME_MAX + 1])
{
    if (!path || path[0] != '/' || strlen(path) >= PATH_MAX) { errno = EINVAL; return -1; }
    const char *slash = strrchr(path, '/');
    size_t len = strlen(slash + 1), parent = slash - path;
    if (!len || len > NAME_MAX || !strcmp(slash + 1, ".") || !strcmp(slash + 1, "..")) {
        errno = EINVAL; return -1;
    }
    char directory[PATH_MAX];
    if (!parent) { directory[0] = '/'; parent = 1; }
    else memcpy(directory, path, parent);
    directory[parent] = 0; memcpy(name, slash + 1, len + 1);
    int fd = open(directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return -1;
    struct stat st;
    if (!fstat(fd, &st)) {
        if (S_ISDIR(st.st_mode) && !st.st_uid && (st.st_mode & 07777) == 0700)
            return fd;
        errno = EINVAL;
    }
    int saved = errno;
    close(fd); errno = saved; return -1;
}

static int process_load(const char *path, struct process_record *r)
{
    char name[NAME_MAX + 1], text[256];
    int fd = process_directory(path, name);
    if (fd < 0) return -1;
    int n = process_read_at(fd, name, text, sizeof(text) - 1, 1);
    int saved = errno;
    if (close(fd) && n >= 0) return -1;
    errno = saved;
    if (n < 0) return -1;
    if (memchr(text, 0, n)) { errno = EINVAL; return -1; }
    text[n] = 0;
    return process_parse_record(text, r);
}

static inline int process_control(const char *path, int stop_process)
{
    struct process_record r;
    if (geteuid() || process_load(path, &r)) { if (geteuid()) errno = EPERM; return -1; }
    if (r.sectors) { errno = EINVAL; return -1; }
    int fd = process_bind(&r);
    if (fd < 0) return -1;
    int ret = stop_process ? syscall(SYS_pidfd_send_signal, fd, SIGTERM, NULL, 0u) : 0;
    int saved = errno;
    if (close(fd) && !ret) return -1;
    errno = saved;
    return ret;
}

/* Only cooperative publishers/cleaners serialize on the pinned private
 * directory. Other root processes can replace state; this is not a root lease. */
static inline int process_check_directory(const char *path)
{
    if (geteuid()) { errno = EPERM; return -1; }
    char name[NAME_MAX + 1];
    int fd = process_directory(path, name);
    return fd < 0 ? -1 : close(fd);
}

static inline int process_clean(const char *path)
{
    if (geteuid()) { errno = EPERM; return -1; }
    char name[NAME_MAX + 1], text[256];
    int directory = process_directory(path, name);
    if (directory < 0) return -1;
    int ret = -1, bound = -1;
    struct process_record r;
    if (flock(directory, LOCK_EX)) goto done;
    int n = process_read_at(directory, name, text, sizeof(text) - 1, 1);
    if (n < 0) goto done;
    if (memchr(text, 0, n)) { errno = EINVAL; goto done; }
    text[n] = 0;
    if (process_parse_record(text, &r)) goto done;
    if (r.sectors) { errno = EINVAL; goto done; }
    bound = process_bind(&r);
    if (bound >= 0) { errno = EBUSY; goto done; }
    if (errno != ESRCH && errno != ESTALE) goto done;
    if (unlinkat(directory, name, 0) || fsync(directory)) goto done;
    ret = 0;
done: {
        int saved = errno;
        if (bound >= 0 && close(bound) && !ret) { ret = -1; saved = errno; }
        if (close(directory) && !ret) { ret = -1; saved = errno; }
        errno = saved;
        return ret;
    }
}

/* Publish a new record without overwriting an existing (even legacy) file.
 * Partial failure can retain a published record; it never starts the backend
 * and never deletes/replaces an unrelated existing destination. */
static int process_publish(const char *path, const struct process_record *r)
{
    char name[NAME_MAX + 1], temporary[128], text[256];
    int directory = process_directory(path, name);
    if (directory < 0) return -1;
    int fd = -1, created = 0, ret = -1;
    if (flock(directory, LOCK_EX)) goto done;
    for (unsigned attempt = 0; attempt < 16; attempt++) {
        snprintf(temporary, sizeof(temporary), ".iosnand.%u.%llu.%u.tmp", r->pid,
                 (unsigned long long)r->start, attempt);
        fd = openat(directory, temporary, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (fd >= 0) { created = 1; break; }
        if (errno != EEXIST) goto done;
    }
    if (fd < 0) goto done;
    if (fchmod(fd, 0600)) goto done;
    int len;
    if (r->sectors)
        len = snprintf(text, sizeof(text), "IOSNANDREADY1 %u %llu %llu %llu %llu\n", r->pid,
                       (unsigned long long)r->start, (unsigned long long)r->dev,
                       (unsigned long long)r->ino, (unsigned long long)r->sectors);
    else
        len = snprintf(text, sizeof(text), "IOSNANDPID1 %u %llu %llu %llu\n", r->pid,
                       (unsigned long long)r->start, (unsigned long long)r->dev,
                       (unsigned long long)r->ino);
    if (len < 0 || len >= (int)sizeof(text)) { errno = EOVERFLOW; goto done; }
    for (int put = 0; put < len;) {
        ssize_t n = write(fd, text + put, len - put);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { if (!n) errno = EIO; goto done; }
        put += n;
    }
    if (fsync(fd) || linkat(directory, temporary, directory, name, 0)) goto done;
    if (unlinkat(directory, temporary, 0)) goto done;
    created = 0;
    if (fsync(directory)) goto done;
    ret = 0;
done: {
        int saved = errno;
        if (created && unlinkat(directory, temporary, 0) && !ret) {
            ret = -1; saved = errno;
        }
        if (fd >= 0 && close(fd) && !ret) { ret = -1; saved = errno; }
        if (close(directory) && !ret) { ret = -1; saved = errno; }
        errno = saved;
        return ret;
    }
}

/* Generation-bound readiness is separate from daemon registration. The
 * kernel's pid attribute names the task executing this export's DO_IT. */
static int process_ready_path(const char *path, const struct process_record *r,
                              char ready[PATH_MAX])
{
    int n = snprintf(ready, PATH_MAX, "%s.ready.%u.%llu", path, r->pid,
                     (unsigned long long)r->start);
    if (n < 0 || n >= PATH_MAX) { errno = ENAMETOOLONG; return -1; }
    return 0;
}

static int process_kernel_value(int directory, const char *name, uint64_t *value)
{
    char text[64];
    int n = process_read_at(directory, name, text, sizeof(text) - 1, 0);
    if (n < 0) return -1;
    if (memchr(text, 0, n)) { errno = EINVAL; return -1; }
    text[n] = 0;
    const char *p = text;
    if (process_number(&p, value) || strcmp(p, "\n")) { errno = EINVAL; return -1; }
    return 0;
}

static int process_kernel_state(unsigned index, uint32_t pid, uint64_t sectors)
{
    char name[64];
    snprintf(name, sizeof(name), "/sys/devices/virtual/block/nbd%u", index);
    int directory = open(name, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (directory < 0) return -1;
    uint64_t actual_pid, actual_size, ro;
    int ret = -1;
    if (process_kernel_value(directory, "pid", &actual_pid) ||
        process_kernel_value(directory, "size", &actual_size) ||
        process_kernel_value(directory, "ro", &ro)) goto done;
    if (actual_pid != pid || !sectors) { errno = ESTALE; goto done; }
    /* Own DO_IT task is visible before startup finishes. Size/RO are not
     * allowed to authorize mounting until they match the configured export. */
    if (actual_size != sectors || ro != 1) { errno = EAGAIN; goto done; }
    ret = 0;
done: {
        int saved = errno;
        if (close(directory) && !ret) return -1;
        errno = saved; return ret;
    }
}

static inline int process_ready_control(const char *path)
{
    if (geteuid()) { errno = EPERM; return -1; }
    struct process_record daemon, worker;
    char ready[PATH_MAX];
    if (process_load(path, &daemon)) return -1;
    if (daemon.sectors) { errno = EINVAL; return -1; }
    int dfd = process_bind(&daemon), wfd = -1, ret = -1;
    if (dfd < 0) return -1;
    if (process_ready_path(path, &daemon, ready) || process_load(ready, &worker)) goto done;
    /* The worker inherits iosnand's executable, but has its own start time. */
    if (worker.dev != daemon.dev || worker.ino != daemon.ino || worker.pid == daemon.pid || !worker.sectors) {
        errno = ESTALE; goto done;
    }
    wfd = process_bind(&worker);
    if (wfd < 0 || process_kernel_state(0, worker.pid, worker.sectors) ||
        process_live(dfd) || process_live(wfd)) goto done;
    ret = 0;
done: {
        int saved = errno;
        if (wfd >= 0 && close(wfd) && !ret) { ret = -1; saved = errno; }
        if (close(dfd) && !ret) { ret = -1; saved = errno; }
        errno = saved; return ret;
    }
}

static inline int process_publish_ready(const char *path, uint32_t first_worker, uint64_t sectors)
{
    struct process_record daemon, worker;
    char ready[PATH_MAX];
    if (process_load(path, &daemon)) return -1;
    if (daemon.sectors) { errno = EINVAL; return -1; }
    if (daemon.pid != (uint32_t)getpid()) { errno = ESTALE; return -1; }
    int dfd = process_bind(&daemon), wfd = -1, ret = -1;
    if (dfd < 0) return -1;
    if (process_identity(first_worker, &worker)) goto done;
    if (worker.dev != daemon.dev || worker.ino != daemon.ino || worker.pid == daemon.pid) {
        errno = ESTALE; goto done;
    }
    worker.sectors = sectors;
    wfd = process_bind(&worker);
    if (wfd < 0 || process_kernel_state(0, first_worker, sectors) ||
        process_ready_path(path, &daemon, ready)) goto done;
    ret = process_publish(ready, &worker);
done: {
        int saved = errno;
        if (wfd >= 0 && close(wfd) && !ret) { ret = -1; saved = errno; }
        if (close(dfd) && !ret) { ret = -1; saved = errno; }
        errno = saved; return ret;
    }
}

/* Called by iosnand itself after exec and CLI validation, before NAND opens.
 * This avoids a shell-background exec race and an unregistered live daemon. */
static inline int process_record_self(const char *path)
{
    if (geteuid()) { errno = EPERM; return -1; }
    pid_t pid = getpid();
    if (pid <= 1) { errno = EINVAL; return -1; }
    int pidfd = syscall(SYS_pidfd_open, pid, 0u);
    if (pidfd < 0) return -1;
    struct process_record r;
    struct stat expected;
    int binary = -1, ret = -1;
    if (process_live(pidfd) || process_identity(pid, &r)) goto done;
    binary = open("/sbin/iosnand", O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (binary < 0 || fstat(binary, &expected)) goto done;
    if (!S_ISREG(expected.st_mode) || expected.st_uid ||
        (uint64_t)expected.st_dev != r.dev || (uint64_t)expected.st_ino != r.ino) {
        errno = ESTALE; goto done;
    }
    ret = process_publish(path, &r);
done: {
        int saved = errno;
        if (binary >= 0 && close(binary) && !ret) { ret = -1; saved = errno; }
        if (close(pidfd) && !ret) { ret = -1; saved = errno; }
        errno = saved;
        return ret;
    }
}

#endif
