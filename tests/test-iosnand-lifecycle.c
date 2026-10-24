/* Offline attach/serve/worker tests: all process, device and directory APIs
 * are fake. No OS fork, ioctl, wait, signal, device or socket is performed. */
#define main iosnand_device_main
#include "../tools/nand/iosnand.c"
#undef main
#include <assert.h>
#include <setjmp.h>

int ppn_open(void) { abort(); }
int ppn_claim(int b) { (void)b; abort(); }
int ppn_lock(int b) { (void)b; abort(); }
int ppn_read_page(int b, int c, uint32_t r, uint8_t *p, uint32_t n, uint8_t *s)
{ (void)b; (void)c; (void)r; (void)p; (void)n; (void)s; abort(); }

enum { STDIO = 1, CONTROL, PEER, SOCKET, NAND, ENUMERATION };
enum { IDLE, OURS, FOREIGN };
static int nextfd, live[256], types[256], nbdidx[256], owner[12];
static int opens, socketfail, openfail, configfail, forkfail, forkchild;
static int clears, doit, waits, eintr, waitfail, childstatus, rebind, closefail;
static int enumfail, enumfd, cursor, childmode, exitstatus;
static int reap_now, pollfail;
static int close_worker, closedirfail, dirfdfail;
static int close_kernel;
static unsigned checks;
static pid_t waited;
static jmp_buf exit_jump;
static int ioctl_order[128], nioctls;
static struct dirent dent;
static char directory;

static int newfd(int type, int idx)
{
    int fd = nextfd++;
    assert(fd < 256);
    live[fd] = 1; types[fd] = type; nbdidx[fd] = idx;
    return fd;
}
static int resources(void)
{ int n = 0; for (int f = 100; f < nextfd; f++) n += live[f]; return n; }
static void reset(void)
{
    memset(live, 0, sizeof(live)); memset(types, 0, sizeof(types));
    memset(nbdidx, -1, sizeof(nbdidx)); memset(owner, 0, sizeof(owner));
    for (int f = 0; f < 3; f++) { live[f] = 1; types[f] = STDIO; }
    nextfd = 100; opens = socketfail = configfail = forkfail = forkchild = 0;
    clears = doit = waits = eintr = waitfail = childstatus = rebind = closefail = 0;
    enumfail = enumfd = cursor = childmode = nioctls = reap_now = pollfail = 0;
    close_worker = closedirfail = dirfdfail = 0;
    close_kernel = 0;
    openfail = -1; waited = 0; exitstatus = -1; stop = 0;
}
int __wrap_open(const char *path, int flags, ...)
{
    assert(!strncmp(path, "/dev/nbd", 8) && (flags & O_RDWR) && (flags & O_CLOEXEC));
    if (opens++ == openfail) { errno = ENOENT; return -1; }
    return newfd(CONTROL, atoi(path + 8));
}
int __wrap_socketpair(int domain, int type, int protocol, int sv[2])
{
    assert(domain == AF_UNIX && type == SOCK_STREAM && protocol == 0);
    if (socketfail) { errno = EMFILE; return -1; }
    sv[0] = newfd(SOCKET, -1); sv[1] = newfd(PEER, -1); return 0;
}
int __wrap_close(int fd)
{
    assert(fd >= 0 && fd < nextfd && live[fd]);
    live[fd] = 0;
    if (close_kernel && types[fd] == SOCKET && !childmode) {
        close_kernel = 0; errno = EIO; return -1;
    }
    if (close_worker && types[fd] == CONTROL && nbdidx[fd] == 1) {
        close_worker = 0; errno = EIO; return -1;
    }
    if (closefail) { closefail = 0; errno = EIO; return -1; }
    return 0;
}
int __wrap_ioctl(int fd, unsigned long req, ...)
{
    assert(fd >= 100 && fd < nextfd && live[fd] && types[fd] == CONTROL);
    assert(nioctls < 128); ioctl_order[nioctls++] = req;
    int idx = nbdidx[fd];
    if (req == NBD_SET_SOCK) {
        if (owner[idx] == FOREIGN || configfail == 1) { errno = EBUSY; return -1; }
        owner[idx] = OURS;
        return 0;
    }
    /* Reset/geometry/flag operations can never reach a foreign generation. */
    assert(owner[idx] == OURS);
    if (req == NBD_CLEAR_SOCK) { clears++; owner[idx] = IDLE; return 0; }
    if (req == NBD_DO_IT) {
        assert(childmode && live[2]);
        for (int f = 0; f < nextfd; f++)
            assert(!live[f] || f == fd || f == 2);
        doit++;
        if (childstatus) { errno = EIO; return -1; }
        return 0;
    }
    assert(req == NBD_SET_BLKSIZE || req == NBD_SET_SIZE_BLOCKS || req == NBD_SET_FLAGS);
    if ((req == NBD_SET_BLKSIZE && configfail == 2) ||
        (req == NBD_SET_SIZE_BLOCKS && configfail == 3) ||
        (req == NBD_SET_FLAGS && configfail == 4)) { errno = EINVAL; return -1; }
    return 0;
}
pid_t __wrap_fork(void)
{
    if (forkfail) { errno = EAGAIN; return -1; }
    if (forkchild) { childmode = 1; return 0; }
    return 40000 + opens;
}
_Noreturn void __wrap__exit(int status)
{ assert(childmode); exitstatus = status; longjmp(exit_jump, 1); }
pid_t __wrap_waitpid(pid_t pid, int *status, int options)
{
    assert(pid > 0 && (options == 0 || options == WNOHANG));
    if (!options)
        for (int f = 100; f < nextfd; f++) assert(!live[f] || types[f] != PEER);
    waits++; waited = pid;
    if (eintr) { eintr--; errno = EINTR; return -1; }
    if (waitfail) { errno = ECHILD; return -1; }
    if (options == WNOHANG && !reap_now) return 0;
    if (rebind) owner[0] = FOREIGN;
    *status = childstatus << 8;
    return pid;
}
__sighandler_t __wrap_signal(int sig, __sighandler_t fn)
{ (void)sig; (void)fn; return SIG_DFL; }
int __wrap_poll(struct pollfd *fds, nfds_t n, int timeout)
{
    assert(n > 0 && timeout == 1000);
    for (unsigned i = 0; i < n; i++) fds[i].revents = 0;
    if (pollfail) { errno = ENOMEM; return -1; }
    assert(reap_now); /* never let a fake test spin indefinitely */
    return 0;
}
/* Fortified libc headers can redirect poll() to this bounds-check entry. */
int __wrap___poll_chk(struct pollfd *fds, nfds_t n, int timeout, size_t bytes)
{
    assert(n <= bytes / sizeof(*fds));
    return __wrap_poll(fds, n, timeout);
}
/* Unreachable request transport or unexpected future signal is never real. */
ssize_t __wrap_read(int fd, void *buf, size_t n)
{ (void)fd; (void)buf; (void)n; abort(); }
ssize_t __wrap_write(int fd, const void *buf, size_t n)
{ (void)fd; (void)buf; (void)n; abort(); }
int __wrap_kill(pid_t pid, int sig) { (void)pid; (void)sig; abort(); }
DIR *__wrap_opendir(const char *path)
{
    assert(childmode && !strcmp(path, "/proc/self/fd"));
    if (enumfail == 1) { errno = ENOENT; return NULL; }
    enumfd = newfd(ENUMERATION, -1); cursor = 0;
    return (DIR *)&directory;
}
int __wrap_dirfd(DIR *d)
{
    assert(d == (DIR *)&directory);
    if (dirfdfail) { errno = EBADF; return -1; }
    return enumfd;
}
struct dirent *__wrap_readdir(DIR *d)
{
    assert(d == (DIR *)&directory && live[enumfd]);
    if (enumfail == 2) { errno = EIO; return NULL; }
    while (cursor < nextfd && !live[cursor]) cursor++;
    if (cursor == nextfd) return NULL;
    snprintf(dent.d_name, sizeof(dent.d_name), "%d", cursor++);
    return &dent;
}
int __wrap_closedir(DIR *d)
{
    assert(d == (DIR *)&directory);
    int r = __wrap_close(enumfd);
    if (closedirfail) { errno = EIO; return -1; }
    return r;
}

static struct part p = { .name = "System", .size = LWVM_CHUNK };
static void child_case(int failure)
{
    reset();
    struct export e = { .p = &p };
    assert(attach(&e, 0) == 0); /* prior export plus simulated NAND fd */
    newfd(NAND, -1);
    forkchild = 1;
    if (failure == 1 || failure == 2) enumfail = failure;
    if (failure == 3) closefail = 1;
    if (failure == 4) childstatus = 1;
    if (failure == 5) close_worker = 1;
    if (failure == 6) closedirfail = 1;
    if (failure == 7) dirfdfail = 1;
    if (!setjmp(exit_jump)) attach(&e, 1);
    assert(exitstatus == !!failure && clears == 0);
    assert(doit == (failure == 0 || failure == 4 || failure == 5));
    if (!failure) assert(resources() == 0 && live[2] && !live[0] && !live[1]);
    checks++;
}

int main(void)
{
    struct export e = { .p = &p };
    reset(); openfail = 0;
    assert(attach(&e, 0) == -1 && resources() == 0 && nioctls == 0); checks++;
    reset(); socketfail = 1;
    assert(attach(&e, 0) == -1 && resources() == 0 && nioctls == 0); checks++;
    for (int i = 1; i <= 4; i++) {
        reset(); configfail = i;
        assert(attach(&e, 0) == -1 && resources() == 0 && waits == 0);
        assert(clears == (i != 1) && ioctl_order[0] == NBD_SET_SOCK); checks++;
    }
    reset(); owner[0] = FOREIGN;
    assert(attach(&e, 0) == -1 && owner[0] == FOREIGN && clears == 0 && nioctls == 1);
    assert(resources() == 0); checks++;
    reset(); forkfail = 1;
    assert(attach(&e, 0) == -1 && resources() == 0 && waits == 0 && clears == 1); checks++;
    reset(); close_kernel = 1;
    assert(attach(&e, 0) == -1 && resources() == 0 && waits == 1 && clears == 0); checks++;
    reset(); configfail = 3; close_kernel = 1;
    assert(attach(&e, 0) == -1 && resources() == 0 && clears == 1 && errno == EINVAL); checks++;
    reset(); assert(attach(&e, 0) == 0 && resources() == 2 && e.child > 0 && e.started);
    assert(ioctl_order[0] == NBD_SET_SOCK && ioctl_order[1] == NBD_SET_BLKSIZE &&
           ioctl_order[2] == NBD_SET_SIZE_BLOCKS && ioctl_order[3] == NBD_SET_FLAGS);
    assert(cleanup_exports(&e, 1) == 0 && resources() == 0 && clears == 0 && waits == 1); checks++;
    reset(); assert(attach(&e, 0) == 0); eintr = 1;
    assert(cleanup_exports(&e, 1) == 0 && waits == 2 && resources() == 0); checks++;
    reset(); assert(attach(&e, 0) == 0); waitfail = 1;
    assert(cleanup_exports(&e, 1) == 1 && resources() == 0 && clears == 0); checks++;
    reset(); assert(attach(&e, 0) == 0); childstatus = 1;
    assert(cleanup_exports(&e, 1) == 1 && resources() == 0 && clears == 0); checks++;
    reset(); assert(attach(&e, 0) == 0); rebind = 1;
    assert(cleanup_exports(&e, 1) == 0 && owner[0] == FOREIGN && clears == 0); checks++;
    reset(); assert(attach(&e, 0) == 0); closefail = 1;
    assert(cleanup_exports(&e, 1) == 1 && resources() == 0); checks++;
    reset(); nparts = 2; parts[0] = p; parts[1] = p; strcpy(parts[1].name, "Data");
    char *names[] = { "System", "Data" }; openfail = 1;
    assert(serve(names, 2) == 1 && resources() == 0 && waits == 1 && clears == 0); checks++;
    reset(); stop = 1;
    assert(serve(names, 2) == 0 && resources() == 0 && waits == 2 && clears == 0); checks++;
    reset(); forkfail = 1;
    assert(serve(NULL, 0) == 1 && resources() == 0 && waits == 0); checks++;
    reset(); e.nbd = e.sock = -1; e.child = -1; e.owned = e.started = 0;
    assert(cleanup_exports(&e, 1) == 0 && waits == 0); checks++;
    reset(); assert(attach(&e, 0) == 0);
    pid_t child = e.child;
    assert(check_workers(&e, 1) == 0 && e.child == child); checks++;
    eintr = 1;
    assert(check_workers(&e, 1) == 0 && e.child == child); checks++;
    reap_now = 1; childstatus = 1;
    assert(check_workers(&e, 1) == 1 && e.child == 0);
    unsigned before = waits;
    assert(cleanup_exports(&e, 1) == 0 && waits == before && clears == 0 && resources() == 0); checks++;
    reset(); assert(attach(&e, 0) == 0); waitfail = 1;
    assert(check_workers(&e, 1) == 1 && e.child == 0);
    assert(cleanup_exports(&e, 1) == 0 && resources() == 0); checks++;
    reset(); assert(attach(&e, 0) == 0); reap_now = 1;
    assert(check_workers(&e, 1) == 1 && e.child == 0); /* even exit0 before peer cleanup */
    assert(cleanup_exports(&e, 1) == 0 && resources() == 0); checks++;
    reset(); assert(attach(&e, 0) == 0); reap_now = 1;
    __wrap_close(e.sock); e.sock = -1;
    assert(check_workers(&e, 1) == 0 && e.child == 0);
    assert(cleanup_exports(&e, 1) == 0 && resources() == 0); checks++;
    reset(); reap_now = childstatus = 1;
    assert(serve(NULL, 0) == 1 && resources() == 0 && clears == 0 && waits == 1); checks++;
    reset(); pollfail = 1;
    assert(serve(NULL, 0) == 1 && resources() == 0 && clears == 0 && waits == 1); checks++;
    for (int i = 0; i <= 7; i++) child_case(i);
    printf("iosnand-lifecycle: %u offline fake-API ownership/unwind/worker cases passed\n", checks);
    return 0;
}
