// gltrace -- drive OpenGL ES 2 on the iPad's own jailbroken iOS 8.4.1 and
// capture what the GL driver (IMGSGX543GLDriver) submits to the kernel, so the
// SGX543 command stream can be reverse engineered without a driver of our own.
// See docs/research/p105-gpu.md.  Read only: it renders offscreen and logs.
//
// It runs three frames into one offscreen FBO, changing as little as possible
// between them so the command buffer can be diffed:
//   frame a: clear only          (baseline)
//   frame b: one triangle, uColor = orange
//   frame c: same triangle, uColor = teal   (only a uniform changes)
// After each it writes the full contents of every shared buffer the driver
// mapped (IOConnectMapMemory) to /var/root/gt_<a|b|c>_map<type>.bin.  Diffing
// a vs b isolates the draw (VDM/state/USSE/PDS); b vs c isolates the colour
// constant.  Every IOConnectCall* into the kernel is logged to stdout.
//
// Build: tools/iosgpu/build.sh (macOS + Xcode; stubs/*.tbd, ldid, no entitlements).
#import <OpenGLES/EAGL.h>
#import <OpenGLES/ES2/gl.h>
#import <OpenGLES/ES2/glext.h>
#import <CoreVideo/CoreVideo.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include "fishhook.h"

typedef unsigned int mach_port_t;
typedef int kern_return_t;
typedef unsigned int vm_address_t;
typedef unsigned int vm_size_t;

long write(int, const void *, unsigned long);
int close(int);   /* open() and the O_* flags come from <fcntl.h> */
void *malloc(unsigned long);
void free(void *);
extern mach_port_t mach_task_self_;
#define mach_task_self() mach_task_self_
kern_return_t vm_read_overwrite(mach_port_t, vm_address_t, vm_size_t, vm_address_t, vm_size_t *);
/* 32-bit vm_region_recurse_64 with a vm_region_submap_info_64 as 19 words:
 * [0] protection, [5] user_tag */
kern_return_t vm_region_recurse_64(mach_port_t, vm_address_t *, vm_size_t *,
                                   unsigned int *, int *, unsigned int *);

/* the CPU arena the GL driver keeps its GPU-shared buffers in (map0/1/2 and the
 * submit structs' pointers all landed here); dumped as a sparse image so the
 * same file offset is the same address across frames a/b/c */
#define ARENA_LO 0x00200000u
#define ARENA_HI 0x02000000u

/* ---- the IOKit calls we intercept ------------------------------------- */

static kern_return_t (*orig_call)(mach_port_t, uint32_t, const uint64_t *, uint32_t,
                                  const void *, size_t, uint64_t *, uint32_t *,
                                  void *, size_t *);
static kern_return_t (*orig_call_struct)(mach_port_t, uint32_t, const void *, size_t,
                                         void *, size_t *);
static kern_return_t (*orig_call_scalar)(mach_port_t, uint32_t, const uint64_t *, uint32_t,
                                         uint64_t *, uint32_t *);
static kern_return_t (*orig_map)(mach_port_t, uint32_t, mach_port_t,
                                 vm_address_t *, vm_size_t *, uint32_t);

static struct { vm_address_t addr; vm_size_t size; uint32_t type; } maps[64];
static int nmaps;

static void dump(const char *tag, const unsigned char *p, size_t n, size_t cap)
{
    size_t m = n < cap ? n : cap;
    printf("    %s (%zu bytes):", tag, n);
    for (size_t i = 0; i < m; i++) {
        if (i % 16 == 0)
            printf("\n      %04zx:", i);
        printf(" %02x", p[i]);
    }
    if (n > m)
        printf(" ...");
    printf("\n");
}

/* "gltrace tqpatch WORD VALUE": at the first IOKit call made while the first
 * transfer (payload word 2 non-zero) sits in the command buffer -- the kernel
 * takes the commands at the callStruct before the submit -- set payload word
 * WORD to VALUE.  Pointing
 * a GPU access at nothing makes the GPU hang on purpose; the kernel then resets
 * it and its hang report (SGXDriver543's register dump, kernelcache 0x80bf9230,
 * IOLog) shows iOS's own register state in the middle of a transfer, and the
 * driver's and the registers' kernel addresses. */
static int patch_word = -1;
static uint32_t patch_val;
static int patched;
/* "gltrace rpatch WORD VALUE": the same for the triangle's render commands
 * (type 1, 0xdc bytes) -- any whose word 2 differs from the clear frame's */
static int rpatch_armed;
static uint32_t rpatch_clear_w2;

static void patch_renders(void)
{
    for (int i = 0; i < nmaps; i++) {
        unsigned char *b = (unsigned char *)(uintptr_t)maps[i].addr;

        if (maps[i].type != 0)
            continue;
        for (unsigned p = 0; p + 0x80 + 0xdc <= maps[i].size; p += 4) {
            uint32_t *h = (uint32_t *)(b + p), off = h[4], *pl;

            if (off < 0x14 || off > 0x80 || (off & 3) || b[p + off] != 1)
                continue;
            if (h[0] != off + 0xdc && h[1] != off + 0xdc &&
                h[2] != off + 0xdc && h[3] != off + 0xdc)
                continue;
            pl = (uint32_t *)(b + p + off);
            if (pl[2] == rpatch_clear_w2 || pl[patch_word] == patch_val)
                continue;
            printf("== rpatch: render at 0x%08x, payload word %d 0x%08x -> 0x%08x\n",
                   maps[i].addr + p, patch_word, pl[patch_word], patch_val);
            pl[patch_word] = patch_val;
        }
    }
}

static void patch_transfer(void)
{
    for (int i = 0; i < nmaps; i++) {
        unsigned char *b = (unsigned char *)(uintptr_t)maps[i].addr;

        if (maps[i].type != 0)
            continue;
        for (unsigned p = 0; p + 0x80 + 0x7c <= maps[i].size; p += 4) {
            uint32_t *h = (uint32_t *)(b + p), off = h[4], *pl;

            if (off < 0x14 || off > 0x80 || (off & 3) || b[p + off] != 2)
                continue;
            if (h[0] != off + 0x7c && h[1] != off + 0x7c &&
                h[2] != off + 0x7c && h[3] != off + 0x7c)
                continue;
            pl = (uint32_t *)(b + p + off);
            if (pl[1] != 0 || pl[2] == 0)
                continue;
            printf("== tqpatch: transfer at 0x%08x, payload word %d 0x%08x -> 0x%08x\n",
                   maps[i].addr + p, patch_word, pl[patch_word], patch_val);
            pl[patch_word] = patch_val;
            patched = 1;
            return;
        }
    }
}

static kern_return_t my_call(mach_port_t c, uint32_t sel, const uint64_t *in, uint32_t inc,
                             const void *ins, size_t insc, uint64_t *out, uint32_t *outc,
                             void *outs, size_t *outsc)
{
    printf("[call]        conn %u sel %-4u scalars %u struct %zu\n", c, sel, inc, insc);
    if (ins && insc)
        dump("in", ins, insc, 128);
    if (rpatch_armed)
        patch_renders();
    else if (patch_word >= 0 && !patched)
        patch_transfer();
    return orig_call(c, sel, in, inc, ins, insc, out, outc, outs, outsc);
}

static kern_return_t my_call_struct(mach_port_t c, uint32_t sel, const void *ins, size_t insc,
                                    void *outs, size_t *outsc)
{
    printf("[callStruct]  conn %u sel %-4u struct %zu\n", c, sel, insc);
    if (ins && insc)
        dump("in", ins, insc, 128);
    if (rpatch_armed)
        patch_renders();
    else if (patch_word >= 0 && !patched)
        patch_transfer();
    return orig_call_struct(c, sel, ins, insc, outs, outsc);
}

static kern_return_t my_call_scalar(mach_port_t c, uint32_t sel, const uint64_t *in, uint32_t inc,
                                    uint64_t *out, uint32_t *outc)
{
    printf("[callScalar]  conn %u sel %-4u scalars %u\n", c, sel, inc);
    if (rpatch_armed)
        patch_renders();
    else if (patch_word >= 0 && !patched)
        patch_transfer();
    return orig_call_scalar(c, sel, in, inc, out, outc);
}

static kern_return_t my_map(mach_port_t c, uint32_t type, mach_port_t task,
                            vm_address_t *addr, vm_size_t *size, uint32_t opts)
{
    kern_return_t r = orig_map(c, type, task, addr, size, opts);
    if (r == 0 && addr && size) {
        printf("[map]         conn %u type %u -> 0x%08x + 0x%x\n", c, type, *addr, *size);
        if (nmaps < 64) {
            maps[nmaps].addr = *addr;
            maps[nmaps].size = *size;
            maps[nmaps].type = type;
            nmaps++;
        }
    }
    return r;
}

static void install_hooks(void)
{
    struct rebinding r[] = {
        { "IOConnectCallMethod", my_call, (void **)&orig_call },
        { "IOConnectCallStructMethod", my_call_struct, (void **)&orig_call_struct },
        { "IOConnectCallScalarMethod", my_call_scalar, (void **)&orig_call_scalar },
        { "IOConnectMapMemory", my_map, (void **)&orig_map },
    };
    rebind_symbols(r, sizeof(r) / sizeof(r[0]));
}

/* write every mapped buffer to /var/root/gt_<label>_map<type>.bin */
static void snapshot(const char *label)
{
    for (int i = 0; i < nmaps; i++) {
        char path[64];
        snprintf(path, sizeof path, "/var/root/gt_%s_map%u.bin", label, maps[i].type);
        int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) {
            write(fd, (const void *)(uintptr_t)maps[i].addr, maps[i].size);
            close(fd);
        }
    }
    printf("== snapshot '%s': %d buffers written\n", label, nmaps);
}

/* dump the CPU arena as a sparse image: mapped pages copied in, holes left zero */
static void snapshot_arena(const char *label)
{
    unsigned span = ARENA_HI - ARENA_LO;
    unsigned char *img = malloc(span);
    if (!img)
        return;
    memset(img, 0, span);
    unsigned mapped = 0;
    /* also log contiguous mapped extents, so a program's page can be located
     * even when the heap has grown past the old fixed window */
    unsigned run_start = 0;
    int in_run = 0;
    for (unsigned off = 0; off < span; off += 0x1000) {
        vm_size_t got = 0;
        int ok = (vm_read_overwrite(mach_task_self(), ARENA_LO + off, 0x1000,
                                    (vm_address_t)(img + off), &got) == 0 && got == 0x1000);
        if (ok)
            mapped++;
        if (ok && !in_run) { in_run = 1; run_start = off; }
        else if (!ok && in_run) {
            in_run = 0;
            printf("   ext %s: 0x%06x..0x%06x\n", label, ARENA_LO + run_start, ARENA_LO + off);
        }
    }
    if (in_run)
        printf("   ext %s: 0x%06x..0x%06x\n", label, ARENA_LO + run_start, ARENA_HI);
    char path[64];
    snprintf(path, sizeof path, "/var/root/gt_%s_arena.bin", label);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
        write(fd, img, span);
        close(fd);
    }
    free(img);
    printf("== arena '%s': %u/%u pages mapped\n", label, mapped, span / 0x1000);
}

/* Find the GL driver's hardware commands in the arena: a header whose word 4
 * is the payload offset, a payload that starts with its type (1 = render,
 * 0xdc bytes; 2 = transfer, 0x7c bytes, word 1 zero) and the total size
 * among the header words -- what IMGSGXGLContext::copyAndValidateVendorPayload
 * (kernelcache 0x80bf64d4) expects.  Transfers are printed whole. */
static uint32_t tq_w2, tq_w12, tq_w13;	/* GPU addresses in the first transfer */
static int print_renders;		/* "gltrace render": print render payloads too */
static uint32_t last_render_w2;

static void scan_payloads(const char *label)
{
    unsigned span = ARENA_HI - ARENA_LO, nrender = 0, ntransfer = 0;
    unsigned char *img = malloc(span);
    if (!img)
        return;
    memset(img, 0, span);
    for (unsigned off = 0; off < span; off += 0x1000) {
        vm_size_t got = 0;
        vm_read_overwrite(mach_task_self(), ARENA_LO + off, 0x1000,
                          (vm_address_t)(img + off), &got);
    }
    for (unsigned p = 0; p + 0x200 < span; p += 4) {
        const uint32_t *h = (const uint32_t *)(img + p), *pl;
        uint32_t off = h[4], size, t;
        if (off < 0x14 || off > 0x80 || (off & 3))
            continue;
        t = img[p + off];
        size = t == 1 ? 0xdc : t == 2 ? 0x7c : 0;
        if (!size || (h[0] != off + size && h[1] != off + size &&
                      h[2] != off + size && h[3] != off + size))
            continue;
        pl = (const uint32_t *)(img + p + off);
        if (t == 1) {
            nrender++;
            last_render_w2 = pl[2];
            if (print_renders) {
                unsigned nrec = h[1], recoff = h[2];
                printf("== %s: render command at 0x%08x, header", label, ARENA_LO + p);
                for (unsigned i = 0; i < off / 4; i++)
                    printf(" %08x", h[i]);
                printf("\n   payload:");
                for (unsigned i = 0; i < size / 4; i++)
                    printf("%s%08x", i % 8 ? " " : "\n   ", pl[i]);
                printf("\n   records (%u at +0x%x):", nrec, recoff);
                for (unsigned i = 0; i < nrec && i < 64 && p + recoff + 8 * i + 8 <= span; i++)
                    printf("%s%08x %08x", i % 4 ? "  " : "\n   ",
                           ((const uint32_t *)(img + p + recoff))[2 * i],
                           ((const uint32_t *)(img + p + recoff))[2 * i + 1]);
                printf("\n");
            }
            continue;
        }
        if (pl[1] != 0)
            continue;
        if (!ntransfer) {
            tq_w2 = pl[2];
            tq_w12 = pl[12];
            tq_w13 = pl[13];
        }
        ntransfer++;
        printf("== %s: transfer command at 0x%08x, header", label, ARENA_LO + p);
        for (unsigned i = 0; i < off / 4; i++)
            printf(" %08x", h[i]);
        printf("\n   payload:");
        for (unsigned i = 0; i < size / 4; i++)
            printf("%s%08x", i % 8 ? " " : "\n   ", pl[i]);
        printf("\n");
    }
    free(img);
    printf("== %s: %u render, %u transfer command(s) in the arena\n",
           label, nrender, ntransfer);
}

/* Walk every writable region of the process: print the map, find words equal
 * to the GPU addresses the first transfer carries (a bookkeeping record next
 * to one may hold the CPU mapping), and pages holding three alike 0x1c0-byte
 * blocks at +0, +0x1c0, +0x380 (the per-mip-level state), dumped to
 * /var/root/gt_blk_<addr>.bin. */
static void scan_regions(void)
{
    vm_address_t a = 0;
    unsigned char *pg = malloc(0x2000);
    int dumped = 0, found = 0;
    if (!pg)
        return;
    printf("== regions (looking for 0x%08x and 0x%08x)\n", tq_w12, tq_w13);
    for (;;) {
        vm_size_t sz = 0;
        unsigned int depth = 99, cnt = 19;
        int info[19];
        if (vm_region_recurse_64(mach_task_self(), &a, &sz, &depth, info, &cnt))
            break;
        int prot = info[0], tag = info[5];
        if (a >= 0x2000000 || tag)
            printf("   %08x-%08x prot %d tag %d\n", a, a + sz, prot, tag);
        if ((prot & 3) == 3 && sz <= 0x4000000) {
            for (vm_address_t q = a; q < a + sz; q += 0x1000) {
                vm_size_t got = 0;
                if (vm_read_overwrite(mach_task_self(), q, 0x1000,
                                      (vm_address_t)pg, &got) || got != 0x1000)
                    continue;
                const uint32_t *w = (const uint32_t *)pg;
                for (int i = 0; i < 0x400; i++)
                    if ((w[i] == tq_w12 || w[i] == tq_w13) && w[i] && found < 40) {
                        found++;
                        printf("   0x%08x at %08x:", w[i], q + 4 * i);
                        for (int j = i - 4; j < i + 8; j++)
                            if (j >= 0 && j < 0x400)
                                printf(" %08x", w[j]);
                        printf("\n");
                    }
                int nz = 0, eq1 = 0, eq2 = 0;
                for (int i = 0; i < 0x70; i++) {
                    uint32_t x = w[i], y = w[0x70 + i], z = w[0xe0 + i];
                    nz += x != 0;
                    eq1 += x && x == y;
                    eq2 += y && y == z;
                }
                if (nz >= 8 && eq1 * 2 >= nz && eq2 * 2 >= nz && w[0] != w[4] &&
                    dumped < 8) {
                    char path[64];
                    snprintf(path, sizeof path, "/var/root/gt_blk_%08x.bin", q);
                    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
                    if (fd >= 0) {
                        write(fd, pg, 0x1000);
                        close(fd);
                    }
                    dumped++;
                    printf("   blocks at %08x:", q);
                    for (int i = 0; i < 8; i++)
                        printf(" %08x", w[i]);
                    printf("\n");
                }
            }
        }
        a += sz;
    }
    free(pg);
    printf("== regions done: %d matches, %d pages dumped\n", found, dumped);
}

/* ---- resources: GPU address -> CPU mapping ------------------------------ */

static struct { uint32_t lo, hi; } wr[1024];	/* writable regions */
static int nwr;

static void load_regions(void)
{
    vm_address_t a = 0;
    nwr = 0;
    for (;;) {
        vm_size_t sz = 0;
        unsigned int depth = 99, cnt = 19;
        int info[19];
        if (vm_region_recurse_64(mach_task_self(), &a, &sz, &depth, info, &cnt))
            break;
        if ((info[0] & 3) == 3 && sz <= 0x4000000 && nwr < 1024) {
            wr[nwr].lo = a;
            wr[nwr].hi = a + sz;
            nwr++;
        }
        a += sz;
    }
}

static int region_of(uint32_t x)
{
    for (int i = 0; i < nwr; i++)
        if (x >= wr[i].lo && x < wr[i].hi)
            return i;
    return -1;
}

/* call fn(page address, words) for every readable writable page */
static void each_page(void (*fn)(uint32_t, const uint32_t *))
{
    static uint32_t pg[0x400];
    for (int r = 0; r < nwr; r++)
        for (uint32_t q = wr[r].lo; q < wr[r].hi; q += 0x1000) {
            vm_size_t got = 0;
            if (vm_read_overwrite(mach_task_self(), q, 0x1000, (vm_address_t)pg, &got) == 0 &&
                got == 0x1000)
                fn(q, pg);
        }
}

static uint32_t gva[16];
static int ngva;

static void add_gva(uint32_t g)
{
    if (g < 0x80000000u || g >= 0xa0000000u)
        return;
    for (int i = 0; i < ngva; i++)
        if (gva[i] == g)
            return;
    if (ngva < 16)
        gva[ngva++] = g;
}

/* pass 1: the resource list holds w12 two words before w2 */
static void find_list(uint32_t q, const uint32_t *w)
{
    for (int i = 0; i + 2 < 0x400; i++)
        if (w[i] == tq_w12 && w[i + 2] == tq_w2) {
            printf("   resource list at %08x:", q + 4 * (i - 4));
            for (int j = i - 6; j < i + 16 && j < 0x400; j++)
                if (j >= 0) {
                    printf(" %08x", w[j]);
                    add_gva(w[j]);
                }
            printf("\n");
        }
}

/* pass 2: a record is "<pages> 0xa <gpu address>"; print it and every word
 * in it that lies in a writable region */
static int ndumps;
static void find_records(uint32_t q, const uint32_t *w)
{
    for (int i = 2; i < 0x400; i++) {
        int k;
        for (k = 0; k < ngva; k++)
            if (w[i] == gva[k])
                break;
        if (k == ngva || w[i - 1] != 0xa || !w[i - 2] || w[i - 2] > 0x4000)
            continue;
        uint32_t pages = w[i - 2];
        printf("   record %08x @%08x pages %u:", w[i], q + 4 * i, pages);
        int lo = i - 24 < 0 ? 0 : i - 24, hi = i + 24 > 0x400 ? 0x400 : i + 24;
        for (int j = lo; j < hi; j++)
            printf("%s%08x", (j - lo) % 8 ? " " : "\n     ", w[j]);
        printf("\n");
        for (int j = lo; j < hi; j++) {
            int r = region_of(w[j]);
            if (r < 0 || (w[j] & 0xfff))
                continue;
            printf("     cand CPU %08x (word %+d) in %08x-%08x\n", w[j], j - i,
                   wr[r].lo, wr[r].hi);
            if (w[j] + pages * 0x1000 <= wr[r].hi && ndumps < 24) {
                char path[64];
                snprintf(path, sizeof path, "/var/root/gt_res_%08x_%08x.bin", w[i], w[j]);
                int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
                if (fd >= 0) {
                    static unsigned char buf[0x1000];
                    for (uint32_t o = 0; o < pages * 0x1000; o += 0x1000) {
                        vm_size_t got = 0;
                        memset(buf, 0, sizeof buf);
                        vm_read_overwrite(mach_task_self(), w[j] + o, 0x1000,
                                          (vm_address_t)buf, &got);
                        write(fd, buf, 0x1000);
                    }
                    close(fd);
                    ndumps++;
                    printf("     dumped %u pages -> %s\n", pages, path);
                }
            }
        }
    }
}

/* Every IOKit mapping in the process (VM tag 21: GPU buffers the kernel
 * mapped in) to /var/root/gt_iokit.bin as {u32 lo, u32 hi, bytes...}
 * records, so GPU addresses can be matched to contents offline. */
static void dump_iokit_to(const char *path)
{
    vm_address_t a = 0;
    static unsigned char pg[0x1000];
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    unsigned total = 0, n = 0;
    if (fd < 0)
        return;
    for (;;) {
        vm_size_t sz = 0;
        unsigned int depth = 99, cnt = 19;
        int info[19];
        if (vm_region_recurse_64(mach_task_self(), &a, &sz, &depth, info, &cnt))
            break;
        if (info[5] == 21 && (info[0] & 1) && sz <= 0x400000) {
            uint32_t hdr[2] = { a, a + sz };
            write(fd, hdr, 8);
            for (uint32_t o = 0; o < sz; o += 0x1000) {
                vm_size_t got = 0;
                memset(pg, 0, sizeof pg);
                vm_read_overwrite(mach_task_self(), a + o, 0x1000, (vm_address_t)pg, &got);
                write(fd, pg, 0x1000);
            }
            printf("   iokit %08x-%08x prot %d\n", a, a + sz, info[0]);
            total += sz;
            n++;
        }
        a += sz;
    }
    close(fd);
    printf("== iokit: %u regions, %u KiB -> %s\n", n, total / 1024, path);
}

static void dump_iokit(void)
{
    dump_iokit_to("/var/root/gt_iokit.bin");
}

static void scan_resources(void)
{
    load_regions();
    ngva = 0;
    add_gva(tq_w2);
    add_gva(tq_w12);
    add_gva(tq_w13);
    printf("== resources: %d writable regions; w2 %08x w12 %08x w13 %08x\n",
           nwr, tq_w2, tq_w12, tq_w13);
    each_page(find_list);
    printf("   GPU addresses:");
    for (int i = 0; i < ngva; i++)
        printf(" %08x", gva[i]);
    printf("\n");
    ndumps = 0;
    each_page(find_records);
    printf("== resources done, %d dumps\n", ndumps);
}


/* "gltrace linear": the pixel back end's emit programs.  Every readable page
 * of the process is searched for the emit instruction (top half-word 0xfb24,
 * as in the transfer's level programs) with the LIMMs that load its state
 * words r0..r5 right before it; each distinct set is printed once.  Run after
 * a render into an ordinary (twiddled) texture and after one into a linear,
 * IOSurface-backed CVPixelBuffer, the difference is the linear-layout
 * encoding. */
static uint32_t seen_emit[64][7];
static int nseen;

static uint32_t limm_imm(uint64_t w)
{
    return (uint32_t)(((w >> 44) & 0x3f) << 26 | ((w >> 36) & 0x1f) << 21 | (w & 0x1fffff));
}

static void scan_emits(const char *label)
{
    vm_address_t a = 0;
    static uint64_t pg[0x1000 / 8 + 16];
    int hits = 0;
    printf("== emits after %s\n", label);
    for (;;) {
        vm_size_t sz = 0;
        unsigned int depth = 99, cnt = 19;
        int info[19];
        if (vm_region_recurse_64(mach_task_self(), &a, &sz, &depth, info, &cnt))
            break;
        if ((info[0] & 1) && sz <= 0x2000000) {
            for (uint32_t o = 0; o < sz; o += 0x1000) {
                vm_size_t got = 0;
                memset(pg, 0, sizeof pg);
                if (vm_read_overwrite(mach_task_self(), a + o, 0x1000, (vm_address_t)pg, &got) || got != 0x1000)
                    continue;
                for (int i = 6; i < 0x1000 / 8; i++) {
                    uint64_t w = pg[i];
                    if ((w >> 48) != 0xfb24 && (w >> 48) != 0xfb25 && (w >> 48) != 0xfb26)
                        continue;
                    uint32_t r[6] = { 0 }, have = 0;
                    for (int k = 1; k <= 8 && i - k >= 0; k++) {
                        uint64_t x = pg[i - k];
                        if ((x >> 56) != 0xfc || ((x >> 52) & 3) != 2)
                            break;
                        unsigned reg = (unsigned)(x >> 21) & 0x7f;
                        if (reg < 6 && !(have & (1u << reg))) {
                            r[reg] = limm_imm(x);
                            have |= 1u << reg;
                        }
                    }
                    if (have != 0x3f)
                        continue;
                    uint32_t key[7] = { (uint32_t)w, r[0], r[2], r[3], r[4], r[5], (uint32_t)(w >> 32) };
                    int dup = 0;
                    for (int j = 0; j < nseen; j++)
                        if (!memcmp(seen_emit[j], key, sizeof key))
                            dup = 1;
                    if (dup)
                        continue;
                    if (nseen < 64)
                        memcpy(seen_emit[nseen++], key, sizeof key);
                    hits++;
                    printf("   %08x: emit %016llx r0 %08x r1 %08x r2 %08x r3 %08x r4 %08x r5 %08x\n",
                           a + o + i * 8, w, r[0], r[1], r[2], r[3], r[4], r[5]);
                }
            }
        }
        a += sz;
    }
    printf("== %d new emit set(s)\n", hits);
}

/* A render into a linear IOSurface-backed BGRA buffer of W x H; prints the
 * buffer's bytesPerRow and a few pixels as the GPU left them. */
static void linear_render(EAGLContext *ctx, GLuint prog, GLint uColor, int W, int H)
{
    const void *k[1] = { kCVPixelBufferIOSurfacePropertiesKey };
    CFDictionaryRef empty = CFDictionaryCreate(kCFAllocatorDefault, 0, 0, 0,
            &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    const void *v[1] = { empty };
    CFDictionaryRef attrs = CFDictionaryCreate(kCFAllocatorDefault, k, v, 1,
            &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CVPixelBufferRef pb = 0;
    CVReturn cr = CVPixelBufferCreate(kCFAllocatorDefault, W, H, kCVPixelFormatType_32BGRA, attrs, &pb);
    CVOpenGLESTextureCacheRef cache = 0;
    if (!cr)
        cr = CVOpenGLESTextureCacheCreate(kCFAllocatorDefault, 0, (__bridge CVEAGLContext)ctx, 0, &cache);
    CVOpenGLESTextureRef cvtex = 0;
    if (!cr)
        cr = CVOpenGLESTextureCacheCreateTextureFromImage(kCFAllocatorDefault, cache, pb, 0,
                GL_TEXTURE_2D, GL_RGBA, W, H, GL_BGRA_EXT, GL_UNSIGNED_BYTE, 0, &cvtex);
    if (cr) {
        printf("== linear %dx%d: CoreVideo error %d\n", W, H, cr);
        return;
    }
    printf("== linear %dx%d: bytesPerRow %lu\n", W, H, (unsigned long)CVPixelBufferGetBytesPerRow(pb));
    GLuint fbo;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glBindTexture(CVOpenGLESTextureGetTarget(cvtex), CVOpenGLESTextureGetName(cvtex));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           CVOpenGLESTextureGetName(cvtex), 0);
    printf("   fbo status 0x%x\n", glCheckFramebufferStatus(GL_FRAMEBUFFER));
    glViewport(0, 0, W, H);
    glUseProgram(prog);
    glClearColor(0, 0, 0.2f, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glUniform4f(uColor, 1.0f, 0.5f, 0.0f, 1.0f);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glFinish();
    CVPixelBufferLockBaseAddress(pb, 0);
    const uint32_t *px = CVPixelBufferGetBaseAddress(pb);
    size_t bpr = CVPixelBufferGetBytesPerRow(pb);
    if (px)
        printf("   pixels: row0 %08x %08x, centre %08x\n", px[0], px[W - 1],
               px[(H / 2) * (bpr / 4) + W / 2]);
    CVPixelBufferUnlockBaseAddress(pb, 0);
}


/* "gltrace linsrc": a render that samples (a) an ordinary 64x64 texture and
 * (b) a linear, IOSurface-backed 200x120 BGRA texture; the GPU buffers are
 * dumped after each, to find how a linear source is described. */
static void linsrc(EAGLContext *ctx, GLuint prog3, int LW, int LH)
{
    GLuint fbo, rt;
    glGenTextures(1, &rt);
    glBindTexture(GL_TEXTURE_2D, rt);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 64, 64, 0, GL_RGBA, GL_UNSIGNED_BYTE, 0);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, rt, 0);
    glViewport(0, 0, 64, 64);
    glUseProgram(prog3);
    glUniform1i(glGetUniformLocation(prog3, "uTex"), 0);
    glActiveTexture(GL_TEXTURE0);

    static uint32_t tw[64 * 64];
    for (int i = 0; i < 64 * 64; i++)
        tw[i] = 0xff000000u | (uint32_t)i * 0x10203u;
    GLuint t;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 64, 64, 0, GL_RGBA, GL_UNSIGNED_BYTE, tw);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glFinish();
    printf("== a: sampled a 64x64 texture\n");
    dump_iokit_to("/var/root/gt_ls_a.bin");

    const void *k[1] = { kCVPixelBufferIOSurfacePropertiesKey };
    CFDictionaryRef empty = CFDictionaryCreate(kCFAllocatorDefault, 0, 0, 0,
            &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    const void *v[1] = { empty };
    CFDictionaryRef attrs = CFDictionaryCreate(kCFAllocatorDefault, k, v, 1,
            &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CVPixelBufferRef pb = 0;
    CVOpenGLESTextureCacheRef cache = 0;
    CVOpenGLESTextureRef cvtex = 0;
    CVReturn cr = CVPixelBufferCreate(kCFAllocatorDefault, LW, LH, kCVPixelFormatType_32BGRA, attrs, &pb);
    if (!cr)
        cr = CVOpenGLESTextureCacheCreate(kCFAllocatorDefault, 0, (__bridge CVEAGLContext)ctx, 0, &cache);
    if (!cr) {
        CVPixelBufferLockBaseAddress(pb, 0);
        uint32_t *px = CVPixelBufferGetBaseAddress(pb);
        size_t bpr = CVPixelBufferGetBytesPerRow(pb);
        for (int y = 0; y < LH; y++)
            for (int x = 0; x < LW; x++)
                px[y * (bpr / 4) + x] = 0xff000000u | (uint32_t)(y << 8) | (uint32_t)x;
        CVPixelBufferUnlockBaseAddress(pb, 0);
        printf("== b: linear 200x120 source, bytesPerRow %lu\n", (unsigned long)bpr);
        cr = CVOpenGLESTextureCacheCreateTextureFromImage(kCFAllocatorDefault, cache, pb, 0,
                GL_TEXTURE_2D, GL_RGBA, LW, LH, GL_BGRA_EXT, GL_UNSIGNED_BYTE, 0, &cvtex);
    }
    if (cr) {
        printf("== b: CoreVideo error %d\n", cr);
        return;
    }
    glBindTexture(CVOpenGLESTextureGetTarget(cvtex), CVOpenGLESTextureGetName(cvtex));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glFinish();
    unsigned char c[4] = { 0 };
    glReadPixels(32, 32, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, c);
    printf("== b: sampled the linear texture, centre %02x %02x %02x %02x, GL error 0x%x\n",
           c[0], c[1], c[2], c[3], glGetError());
    dump_iokit_to("/var/root/gt_ls_b.bin");
}

/* ---- GLES ------------------------------------------------------------- */

static GLuint make_shader(GLenum type, const char *src)
{
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, 0);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(s, sizeof log, 0, log);
        printf("shader error: %s\n", log);
    }
    return s;
}

int main(int argc, char **argv)
{
    int tq = argc > 1 && (!strcmp(argv[1], "tq") || !strcmp(argv[1], "tqpatch"));
    /* "gltrace tq N": an N x N destination, so the mip transfers span
     * several 32x32 regions (default 64: every level fits one) */
    int tqsize = (tq && argc > 2 && !strcmp(argv[1], "tq")) ? atoi(argv[2]) : 64;

    int rpatch = argc > 3 && !strcmp(argv[1], "rpatch");
    if (rpatch) {
        patch_word = (int)strtoul(argv[2], 0, 0);
        patch_val = (uint32_t)strtoul(argv[3], 0, 0);
        if (patch_word < 2 || patch_word > 54)
            return printf("rpatch: word 2..54\n"), 1;
    }
    if (argc > 3 && !strcmp(argv[1], "tqpatch")) {
        patch_word = (int)strtoul(argv[2], 0, 0);
        patch_val = (uint32_t)strtoul(argv[3], 0, 0);
        if (patch_word < 3 || patch_word > 30)
            return printf("tqpatch: word 3..30\n"), 1;
    }
    setvbuf(stdout, 0, _IONBF, 0);	/* the GPU reset may take the process down */
    install_hooks();
    printf("== hooks installed; creating GLES2 context\n");

    id ctx = [[EAGLContext alloc] initWithAPI:kEAGLRenderingAPIOpenGLES2];
    if (!ctx || ![EAGLContext setCurrentContext:ctx]) {
        printf("no GLES2 context -- headless EAGL may be denied on this build\n");
        return 1;
    }
    printf("== context ok, renderer \"%s\"\n", (const char *)glGetString(GL_RENDERER));

    GLuint tex, fbo;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 64, 64, 0, GL_RGBA, GL_UNSIGNED_BYTE, 0);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);

    const char *vs = "attribute vec4 p; void main(){ gl_Position = p; }";
    /* two fragment programs differing by ONE op: prog1 outputs the uniform,
     * prog2 outputs uColor*uColor.  Both are compiled and drawn in this one
     * process, so both USSE programs sit in the arena at once; the two are
     * near-identical (that is our fragment program) and their one-instruction
     * difference is the MUL -- a first operand-level decode. */
    const char *fs1 = "precision mediump float; uniform vec4 uColor;"
                      "void main(){ gl_FragColor = uColor; }";
    const char *fs2 = "precision mediump float; uniform vec4 uColor;"
                      "void main(){ gl_FragColor = uColor * uColor; }";
    GLuint vsh = make_shader(GL_VERTEX_SHADER, vs);
    GLuint prog = glCreateProgram();
    glAttachShader(prog, vsh);
    glAttachShader(prog, make_shader(GL_FRAGMENT_SHADER, fs1));
    glBindAttribLocation(prog, 0, "p");
    glLinkProgram(prog);
    GLuint prog2 = glCreateProgram();
    glAttachShader(prog2, vsh);
    glAttachShader(prog2, make_shader(GL_FRAGMENT_SHADER, fs2));
    glBindAttribLocation(prog2, 0, "p");
    glLinkProgram(prog2);
    /* prog3: samples a texture -- gives a clean SMP to validate against.
     * texcoord is derived in the vertex shader, so no extra attribute. */
    const char *vs2 = "attribute vec4 p; varying vec2 t;"
                      "void main(){ gl_Position = p; t = p.xy*0.5+0.5; }";
    const char *fs3 = "precision mediump float; varying vec2 t; uniform sampler2D uTex;"
                      "void main(){ gl_FragColor = texture2D(uTex, t); }";
    GLuint prog3 = glCreateProgram();
    glAttachShader(prog3, make_shader(GL_VERTEX_SHADER, vs2));
    glAttachShader(prog3, make_shader(GL_FRAGMENT_SHADER, fs3));
    glBindAttribLocation(prog3, 0, "p");
    glLinkProgram(prog3);
    /* e: right after linking the texture program -- catch its fragment USSE in
     * the compiler's CPU staging buffer before any draw can free/reuse it
     * (the SMP program is absent from post-draw snapshots -> it lives in
     * GPU-only memory once uploaded). */
    glUseProgram(prog3);
    snapshot_arena("e");
    GLuint smptex;
    glGenTextures(1, &smptex);
    glBindTexture(GL_TEXTURE_2D, smptex);
    static const unsigned char texels[] = { 255,0,0,255, 0,255,0,255,
                                            0,0,255,255, 255,255,0,255 };
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 2, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, texels);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    glUseProgram(prog);
    GLint uColor = glGetUniformLocation(prog, "uColor");

    static const float tri[] = { 0.0f, 0.6f, -0.6f, -0.6f, 0.6f, -0.6f };
    glViewport(0, 0, 64, 64);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, tri);

    if (argc > 1 && (!strcmp(argv[1], "render") || rpatch)) {
        /* the smallest render: a clear, then a clear and one triangle */
        print_renders = 1;
        glClearColor(0, 0, 0.2f, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        glFinish();
        scan_payloads("clear");
        if (rpatch) {
            rpatch_clear_w2 = last_render_w2;
            rpatch_armed = 1;
            printf("== rpatch armed: clear frame's word 2 is 0x%08x\n", rpatch_clear_w2);
        } else
            dump_iokit_to("/var/root/gt_r_clear.bin");
        glClear(GL_COLOR_BUFFER_BIT);
        glUniform4f(uColor, 1.0f, 0.5f, 0.0f, 1.0f);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glFinish();
        scan_payloads("triangle");
        if (!rpatch)
            dump_iokit_to("/var/root/gt_r_tri.bin");
        unsigned char c[4] = { 0 };
        glReadPixels(32, 32, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, c);
        printf("== centre pixel %02x %02x %02x %02x\n", c[0], c[1], c[2], c[3]);
        return 0;
    }
    if (argc > 1 && !strcmp(argv[1], "tmplsz")) {
        /* the textured quad again at several render target sizes, to find
         * every field that depends on the size: one frame per W x H */
        const char *vst = "attribute vec4 p; attribute vec2 a; varying mediump vec2 t;"
                          "void main(){ gl_Position = p; t = a; }";
        const char *fst = "precision mediump float; varying vec2 t; uniform sampler2D uTex;"
                          "void main(){ gl_FragColor = texture2D(uTex, t); }";
        GLuint pt = glCreateProgram();
        glAttachShader(pt, make_shader(GL_VERTEX_SHADER, vst));
        glAttachShader(pt, make_shader(GL_FRAGMENT_SHADER, fst));
        glBindAttribLocation(pt, 0, "p");
        glBindAttribLocation(pt, 1, "a");
        glLinkProgram(pt);
        glUseProgram(pt);
        static const float quad[] = { -0.6f, -0.6f, 0.6f, -0.6f, 0.6f, 0.6f,
                                      -0.6f, -0.6f, 0.6f, 0.6f, -0.6f, 0.6f };
        static const float uv[] = { 0, 0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1 };
        unsigned char tx[4 * 4 * 4];
        for (int i = 0; i < 16; i++) {
            tx[i * 4] = (i & 3) < 2 ? 255 : 0; tx[i * 4 + 1] = (i & 3) < 2 ? 0 : 255;
            tx[i * 4 + 2] = 0; tx[i * 4 + 3] = 255;
        }
        GLuint qt;
        glGenTextures(1, &qt);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, qt);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 4, 4, 0, GL_RGBA, GL_UNSIGNED_BYTE, tx);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glUniform1i(glGetUniformLocation(pt, "uTex"), 0);
        glEnableVertexAttribArray(0);
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, quad);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 0, uv);
        static const int sz[][2] = { { 64, 64 }, { 128, 128 }, { 256, 256 }, { 256, 128 } };
        print_renders = 1;
        for (int k = 0; k < 4; k++) {
            int w = sz[k][0], h = sz[k][1];
            GLuint rt, fb;
            glGenTextures(1, &rt);
            glBindTexture(GL_TEXTURE_2D, rt);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, 0);
            glGenFramebuffers(1, &fb);
            glBindFramebuffer(GL_FRAMEBUFFER, fb);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, rt, 0);
            glBindTexture(GL_TEXTURE_2D, qt);
            glViewport(0, 0, w, h);
            glClearColor(0, 0, 0.2f, 1);
            glClear(GL_COLOR_BUFFER_BIT);
            glDrawArrays(GL_TRIANGLES, 0, 6);
            glFinish();
            char label[32], path[64];
            snprintf(label, sizeof(label), "tex%dx%d", w, h);
            snprintf(path, sizeof(path), "/var/root/gt_s_%dx%d.bin", w, h);
            scan_payloads(label);
            dump_iokit_to(path);
            unsigned char c[4] = { 0 };
            glReadPixels(w / 2 - 4, h / 2 - 4, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, c);
            printf("== %s: pixel %02x %02x %02x %02x\n", label, c[0], c[1], c[2], c[3]);
        }
        return 0;
    }
    if (argc > 1 && !strcmp(argv[1], "tmpl")) {
        /* templates for building frames under Linux: a triangle with a
         * colour per vertex, then a textured quad without and with alpha
         * blending.  Each frame: payloads, all IOKit regions, two pixels. */
        const char *vsc = "attribute vec4 p; attribute vec4 c; varying lowp vec4 v;"
                          "void main(){ gl_Position = p; v = c; }";
        const char *fsc = "varying lowp vec4 v; void main(){ gl_FragColor = v; }";
        const char *vst = "attribute vec4 p; attribute vec2 a; varying mediump vec2 t;"
                          "void main(){ gl_Position = p; t = a; }";
        const char *fst = "precision mediump float; varying vec2 t; uniform sampler2D uTex;"
                          "void main(){ gl_FragColor = texture2D(uTex, t); }";
        GLuint pc = glCreateProgram(), pt = glCreateProgram();
        glAttachShader(pc, make_shader(GL_VERTEX_SHADER, vsc));
        glAttachShader(pc, make_shader(GL_FRAGMENT_SHADER, fsc));
        glBindAttribLocation(pc, 0, "p");
        glBindAttribLocation(pc, 1, "c");
        glLinkProgram(pc);
        glAttachShader(pt, make_shader(GL_VERTEX_SHADER, vst));
        glAttachShader(pt, make_shader(GL_FRAGMENT_SHADER, fst));
        glBindAttribLocation(pt, 0, "p");
        glBindAttribLocation(pt, 1, "a");
        glLinkProgram(pt);

        static const float cols[] = { 1, 0, 0, 1, 0, 1, 0, 1, 0, 0, 1, 1 };
        static const float quad[] = { -0.6f, -0.6f, 0.6f, -0.6f, 0.6f, 0.6f,
                                      -0.6f, -0.6f, 0.6f, 0.6f, -0.6f, 0.6f };
        static const float uv[] = { 0, 0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1 };
        /* 4x4: left red, right green; bottom rows opaque, top rows alpha 128 */
        unsigned char tx[4 * 4 * 4];
        for (int y = 0; y < 4; y++)
            for (int x = 0; x < 4; x++) {
                unsigned char *t = tx + (y * 4 + x) * 4;
                t[0] = x < 2 ? 255 : 0; t[1] = x < 2 ? 0 : 255; t[2] = 0;
                t[3] = y < 2 ? 255 : 128;
            }
        GLuint qt;
        glGenTextures(1, &qt);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, qt);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 4, 4, 0, GL_RGBA, GL_UNSIGNED_BYTE, tx);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindTexture(GL_TEXTURE_2D, tex);	/* the render target again */
        print_renders = 1;
        glEnableVertexAttribArray(0);
        glEnableVertexAttribArray(1);
        static const char *names[] = { "vcolor", "tex", "texblend" };
        static const char *files[] = { "/var/root/gt_t_vcolor.bin", "/var/root/gt_t_tex.bin",
                                       "/var/root/gt_t_texblend.bin" };
        for (int f = 0; f < 3; f++) {
            glClearColor(0, 0, 0.2f, 1);
            glClear(GL_COLOR_BUFFER_BIT);
            if (f == 0) {
                glUseProgram(pc);
                glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, tri);
                glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, 0, cols);
                glDrawArrays(GL_TRIANGLES, 0, 3);
            } else {
                glUseProgram(pt);
                glUniform1i(glGetUniformLocation(pt, "uTex"), 0);
                glBindTexture(GL_TEXTURE_2D, qt);
                if (f == 2) {
                    glEnable(GL_BLEND);
                    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
                } else
                    glDisable(GL_BLEND);
                glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, quad);
                glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 0, uv);
                glDrawArrays(GL_TRIANGLES, 0, 6);
            }
            glFinish();
            scan_payloads(names[f]);
            dump_iokit_to(files[f]);
            unsigned char a[4] = { 0 }, b[4] = { 0 };
            glReadPixels(20, 20, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, a);
            glReadPixels(44, 44, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, b);
            printf("== %s: pixel (20,20) %02x %02x %02x %02x, (44,44) %02x %02x %02x %02x\n",
                   names[f], a[0], a[1], a[2], a[3], b[0], b[1], b[2], b[3]);
            if (f)
                glBindTexture(GL_TEXTURE_2D, tex);
        }
        return 0;
    }
    if (argc > 1 && !strcmp(argv[1], "depth")) {
        /* two overlapping triangles, the near one (green, z -0.5) drawn
         * first: without the depth test the far one (orange, z 0.5) covers
         * the overlap, with it the green stays.  One frame each way, so the
         * diff shows what the depth test changes; centre pixel = overlap. */
        static const float near_tri[] = { -0.7f, 0.7f, -0.5f, -0.7f, -0.7f, -0.5f,
                                          0.5f, 0.0f, -0.5f };
        static const float far_tri[] = { 0.7f, 0.7f, 0.5f, -0.5f, 0.0f, 0.5f,
                                         0.7f, -0.7f, 0.5f };
        GLuint rb;
        glGenRenderbuffers(1, &rb);
        glBindRenderbuffer(GL_RENDERBUFFER, rb);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, 64, 64);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, rb);
        print_renders = 1;
        for (int pass = 0; pass < 2; pass++) {
            if (pass)
                glEnable(GL_DEPTH_TEST);
            else
                glDisable(GL_DEPTH_TEST);
            glClearColor(0, 0, 0.2f, 1);
            glClearDepthf(1.0f);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, near_tri);
            glUniform4f(uColor, 0.0f, 1.0f, 0.0f, 1.0f);
            glDrawArrays(GL_TRIANGLES, 0, 3);
            glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, far_tri);
            glUniform4f(uColor, 1.0f, 0.5f, 0.0f, 1.0f);
            glDrawArrays(GL_TRIANGLES, 0, 3);
            glFinish();
            scan_payloads(pass ? "depth on" : "depth off");
            dump_iokit_to(pass ? "/var/root/gt_d_on.bin" : "/var/root/gt_d_off.bin");
            unsigned char c[4] = { 0 };
            glReadPixels(32, 32, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, c);
            printf("== %s: centre pixel %02x %02x %02x %02x\n", pass ? "depth on" : "depth off",
                   c[0], c[1], c[2], c[3]);
        }
        return 0;
    }
    if (argc > 1 && !strcmp(argv[1], "linsrc")) {
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, tri);
        linsrc(ctx, prog3, argc > 3 ? atoi(argv[2]) : 200, argc > 3 ? atoi(argv[3]) : 120);
        return 0;
    }
    if (argc > 1 && !strcmp(argv[1], "linear")) {
        scan_emits("setup");
        glClearColor(0, 0, 0.2f, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        glUniform4f(uColor, 1.0f, 0.5f, 0.0f, 1.0f);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glFinish();
        scan_emits("render into a 64x64 texture");
        linear_render(ctx, prog, uColor, 200, 120);
        scan_emits("render into a linear 200x120 buffer");
        linear_render(ctx, prog, uColor, 64, 64);
        scan_emits("render into a linear 64x64 buffer");
        return 0;
    }

    /* "gltrace tq": operations the driver may do with the transfer queue
     * instead of a render -- a copy out of the FBO into another texture, and
     * mipmap generation -- each followed by a scan for transfer commands. */
    if (tq) {
        glClearColor(0, 0, 0.2f, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        glUniform4f(uColor, 1.0f, 0.5f, 0.0f, 1.0f);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glFinish();
        scan_payloads("tq-before");

        GLuint dst;
        glGenTextures(1, &dst);
        glBindTexture(GL_TEXTURE_2D, dst);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, tqsize, tqsize, 0, GL_RGBA, GL_UNSIGNED_BYTE, 0);
        printf("== f: glCopyTexSubImage2D 64x64 from the FBO into %dx%d\n", tqsize, tqsize);
        glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, 64, 64);
        glFinish();
        scan_payloads("f");

        printf("== g: glGenerateMipmap on it\n");
        glGenerateMipmap(GL_TEXTURE_2D);
        glFinish();
        printf("== g: glFinish returned, GL error 0x%x\n", glGetError());
        scan_payloads("g");
        if (patch_word >= 0)
            return 0;
        scan_resources();
        dump_iokit();
        return 0;
    }

    /* a: clear only */
    printf("== frame a (clear only)\n");
    glClearColor(0, 0, 0.2f, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glFinish();
    snapshot("a"); snapshot_arena("a");

    /* b: triangle, orange */
    printf("== frame b (triangle, orange)\n");
    glClear(GL_COLOR_BUFFER_BIT);
    glUniform4f(uColor, 1.0f, 0.5f, 0.0f, 1.0f);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glFinish();
    snapshot("b"); snapshot_arena("b");

    /* c: triangle with prog2 (uColor*uColor) -- a different fragment program */
    printf("== frame c (triangle, prog2 = uColor*uColor)\n");
    glUseProgram(prog2);
    glUniform4f(glGetUniformLocation(prog2, "uColor"), 1.0f, 0.5f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glFinish();
    snapshot("c"); snapshot_arena("c");

    /* d: textured triangle (prog3) -- a clean SMP */
    printf("== frame d (triangle, prog3 = texture2D)\n");
    glUseProgram(prog3);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, smptex);
    glUniform1i(glGetUniformLocation(prog3, "uTex"), 0);
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glFinish();
    snapshot("d"); snapshot_arena("d");

    unsigned char px[4] = { 0 };
    glReadPixels(32, 32, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
    printf("== centre pixel after d: %02x %02x %02x %02x (textured)\n",
           px[0], px[1], px[2], px[3]);
    return 0;
}
