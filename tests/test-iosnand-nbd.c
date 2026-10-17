/* Offline production NBD request tests: only process-local socketpairs.
 * No kernel NBD attach, ioctl, device open or PPN operation is performed. */
#define IOSNAND_IOCTL test_ioctl
static int test_ioctl(int, unsigned long, void *);
#define main iosnand_device_main
#include "../tools/nand/iosnand.c"
#undef main
#include <assert.h>
#include <pthread.h>

int ppn_open(void) { abort(); }
int ppn_claim(int bus) { (void)bus; abort(); }
int ppn_lock(int bus) { (void)bus; abort(); }
int ppn_read_page(int bus, int ce, uint32_t row, uint8_t *buf, uint32_t len, uint8_t *st)
{ (void)bus; (void)ce; (void)row; (void)buf; (void)len; (void)st; abort(); }
static int test_ioctl(int fd, unsigned long request, void *arg)
{ (void)fd; (void)request; (void)arg; abort(); }

void *__real_malloc(size_t);
static unsigned alloc_calls;
static size_t alloc_size;
static int alloc_fail;
void *__wrap_malloc(size_t n)
{
    alloc_calls++;
    alloc_size = n;
    if (alloc_fail) { errno = ENOMEM; return NULL; }
    return __real_malloc(n);
}

struct worker { struct export e; int result; };
static void *one(void *arg)
{
    struct worker *w = arg;
    w->result = serve_one(&w->e);
    return NULL;
}

static unsigned checks;
static void request(uint32_t type, uint64_t from, uint32_t len,
                    uint32_t error, unsigned allocs, int fail)
{
    int sv[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    struct part p = { .size = LWVM_CHUNK, .nchunks = 1, .phys = { 0 } };
    struct worker w = { .e = { .p = &p, .sock = sv[0] }, .result = 99 };
    struct nbd_request rq = { .magic = htonl(NBD_REQUEST_MAGIC),
        .type = htonl(type), .from = htobe64(from), .len = htonl(len) };
    memcpy(rq.handle, "seqcheck", 8);
    alloc_calls = 0; alloc_size = 0; alloc_fail = fail;
    pthread_t thread;
    assert(pthread_create(&thread, NULL, one, &w) == 0);
    assert(write_full(sv[1], &rq, sizeof(rq)) == 0);
    uint8_t buf[8192];
    memset(buf, 0xa5, sizeof(buf));
    if ((type & 0xffff) == NBD_CMD_WRITE)
        for (uint32_t left = len; left;) {
            unsigned n = left < sizeof(buf) ? left : sizeof(buf);
            assert(write_full(sv[1], buf, n) == 0);
            left -= n;
        }
    struct nbd_reply rp;
    assert(read_full(sv[1], &rp, sizeof(rp)) == 0);
    assert(ntohl(rp.magic) == NBD_REPLY_MAGIC && ntohl(rp.error) == error);
    assert(memcmp(rp.handle, rq.handle, 8) == 0);
    if ((type & 0xffff) == NBD_CMD_READ && !error)
        for (uint32_t left = len; left;) {
            unsigned n = left < sizeof(buf) ? left : sizeof(buf);
            assert(read_full(sv[1], buf, n) == 0);
            for (unsigned i = 0; i < n; i++) assert(buf[i] == 0);
            left -= n;
        }
    assert(pthread_join(thread, NULL) == 0 && w.result == 0);
    assert(alloc_calls == allocs);
    if (allocs) assert(alloc_size == len);
    /* No hidden response payload on errors / non-read commands. */
    assert(recv(sv[1], buf, 1, MSG_DONTWAIT) == -1 && errno == EAGAIN);
    close(sv[0]); close(sv[1]);
    checks++;
}

static void disconnect_or_bad_header(int malformed)
{
    int sv[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    struct part p = { .size = LWVM_CHUNK };
    struct export e = { .sock = sv[0], .p = &p };
    struct nbd_request rq = { .magic = htonl(malformed ? 1 : NBD_REQUEST_MAGIC),
        .type = htonl(NBD_CMD_DISC), .len = htonl(UINT32_MAX) };
    alloc_calls = 0;
    assert(write_full(sv[1], &rq, sizeof(rq)) == 0);
    assert(serve_one(&e) == -1 && alloc_calls == 0);
    char b;
    assert(recv(sv[1], &b, 1, MSG_DONTWAIT) == -1 && errno == EAGAIN);
    close(sv[0]); close(sv[1]);
    checks++;
}

static void truncated_request(int payload)
{
    int sv[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    struct part p = { .size = LWVM_CHUNK };
    struct export e = { .sock = sv[0], .p = &p };
    struct nbd_request rq = { .magic = htonl(NBD_REQUEST_MAGIC),
        .type = htonl(NBD_CMD_WRITE), .len = htonl(UINT32_MAX) };
    alloc_calls = 0;
    assert(write_full(sv[1], &rq, sizeof(rq) - !payload) == 0);
    uint8_t data[4096] = { 0 };
    if (payload) assert(write_full(sv[1], data, sizeof(data)) == 0);
    assert(shutdown(sv[1], SHUT_WR) == 0);
    /* A nominal 4 GiB write never allocates it: fixed-buffer draining
     * terminates on EOF, without a partial success/error frame. */
    assert(serve_one(&e) == -1 && alloc_calls == 0);
    assert(recv(sv[1], data, 1, MSG_DONTWAIT) == -1 && errno == EAGAIN);
    close(sv[0]); close(sv[1]);
    checks++;
}

static void write_then_flush(void)
{
    int sv[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    struct part p = { .size = LWVM_CHUNK };
    struct export e = { .sock = sv[0], .p = &p };
    struct nbd_request rq = { .magic = htonl(NBD_REQUEST_MAGIC),
        .type = htonl(NBD_CMD_WRITE), .len = htonl(4096) };
    memcpy(rq.handle, "write001", 8);
    uint8_t data[4096];
    memset(data, 0xa5, sizeof(data));
    alloc_calls = 0;
    assert(write_full(sv[1], &rq, sizeof(rq)) == 0);
    assert(write_full(sv[1], data, sizeof(data)) == 0);
    rq.type = htonl(NBD_CMD_FLUSH); rq.len = 0;
    memcpy(rq.handle, "flush002", 8);
    assert(write_full(sv[1], &rq, sizeof(rq)) == 0);
    assert(serve_one(&e) == 0 && serve_one(&e) == 0 && alloc_calls == 0);
    struct nbd_reply rp;
    assert(read_full(sv[1], &rp, sizeof(rp)) == 0);
    assert(ntohl(rp.error) == EPERM && !memcmp(rp.handle, "write001", 8));
    assert(read_full(sv[1], &rp, sizeof(rp)) == 0);
    assert(ntohl(rp.error) == 0 && !memcmp(rp.handle, "flush002", 8));
    /* Rejected write payload was drained exactly, preserving framing. */
    assert(recv(sv[1], data, 1, MSG_DONTWAIT) == -1 && errno == EAGAIN);
    close(sv[0]); close(sv[1]);
    checks++;
}

int main(void)
{
    alarm(20); /* fail loudly instead of hanging if a framing test regresses */
    l2p = __real_malloc(NLBA * sizeof(*l2p));
    assert(l2p);
    memset(l2p, 0xff, NLBA * sizeof(*l2p));
    request(NBD_CMD_READ, UINT64_MAX, UINT32_MAX, EIO, 0, 1);
    request(NBD_CMD_READ, 0, UINT32_MAX, EIO, 0, 1);
    request(NBD_CMD_READ, LWVM_CHUNK + 1, (1 << 20) + 1, EIO, 0, 1);
    request(NBD_CMD_READ, LWVM_CHUNK, 1, EIO, 0, 0);
    request(NBD_CMD_READ, LWVM_CHUNK - 1, 2, EIO, 0, 0);
    request(NBD_CMD_READ, LWVM_CHUNK + 1, 0, EIO, 0, 0);
    request(NBD_CMD_READ, LWVM_CHUNK, 0, 0, 0, 0);
    request(NBD_CMD_READ, 0, 0, 0, 0, 0);
    request(NBD_CMD_READ, 0, 4096, 0, 0, 0);
    request(NBD_CMD_READ, 4095, 2, 0, 0, 0);
    request(NBD_CMD_READ, LWVM_CHUNK - 4096, 4096, 0, 0, 0);
    request(NBD_CMD_READ, 0, (1 << 20) + 1, ENOMEM, 1, 1);
    request(NBD_CMD_READ, 0, (1 << 20) + 1, 0, 1, 0);
    request(NBD_CMD_WRITE, 0, 0, EPERM, 0, 0);
    request(NBD_CMD_WRITE, 0, 4096, EPERM, 0, 0);
    request(NBD_CMD_WRITE | NBD_CMD_FLAG_FUA, 0, (1 << 20) + 1, EPERM, 0, 0);
    request(NBD_CMD_TRIM, UINT64_MAX, UINT32_MAX, EPERM, 0, 0);
    request(6 /* WRITE_ZEROES, not advertised */, UINT64_MAX, UINT32_MAX, EPERM, 0, 0);
    request(0x7777, UINT64_MAX, UINT32_MAX, EPERM, 0, 0);
    request(NBD_CMD_FLUSH, 0, 0, 0, 0, 0); /* no writable state */
    disconnect_or_bad_header(0);
    disconnect_or_bad_header(1);
    truncated_request(0);
    truncated_request(1);
    write_then_flush();
    free(l2p);
    printf("iosnand-nbd: %u offline production socketpair/bounds/readonly cases passed\n", checks);
    return 0;
}
