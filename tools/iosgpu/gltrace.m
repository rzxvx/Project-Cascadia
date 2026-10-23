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
// "gltrace corpus OUTDIR FILE.glsl..." is the shader oracle (M11,
// docs/research/p105-mesa.md): each file a vertex and a fragment shader,
// drawn once; the pages of the GL driver's GPU buffers the draw changed
// (the new USSE and PDS programs among them) go to OUTDIR/NAME.pages.
// tools/shadercap.sh runs it, tools/iosgpu/corpus.py reads the result.
// "gltrace es1 OUTDIR [CASE...]" does the same for OpenGL ES 1's fixed
// function: flat shading, logic ops, the alpha test, clip planes, point
// sprites, fog (M35).  Both log each case's render commands.
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

/* (extension and OpenGL ES 1 names the corpus's cases use, M35) */
#ifndef GL_TEXTURE_COMPARE_MODE_EXT
#define GL_TEXTURE_COMPARE_MODE_EXT 0x884C
#define GL_TEXTURE_COMPARE_FUNC_EXT 0x884D
#define GL_COMPARE_REF_TO_TEXTURE_EXT 0x884E
#endif
#ifndef GL_TEXTURE_MAX_LEVEL_APPLE
#define GL_TEXTURE_MAX_LEVEL_APPLE 0x813D
#endif
#ifndef GL_TEXTURE_MAX_ANISOTROPY_EXT
#define GL_TEXTURE_MAX_ANISOTROPY_EXT 0x84FE
#endif
#ifndef GL_ANY_SAMPLES_PASSED_EXT
#define GL_ANY_SAMPLES_PASSED_EXT 0x8C2F
#define GL_QUERY_RESULT_EXT 0x8866
#endif
#ifndef GL_HALF_FLOAT_OES
#define GL_HALF_FLOAT_OES 0x8D61
#endif
#ifndef GL_DEPTH_STENCIL_OES
#define GL_DEPTH_STENCIL_OES 0x84F9
#define GL_UNSIGNED_INT_24_8_OES 0x84FA
#endif
#ifndef GL_BGRA_EXT
#define GL_BGRA_EXT 0x80E1
#endif
#ifndef GL_SRGB_ALPHA_EXT
#define GL_SRGB_ALPHA_EXT 0x8C42
#endif
#ifndef GL_RGBA8_OES
#define GL_RGBA8_OES 0x8058
#endif
#ifndef GL_RGBA16F_EXT
#define GL_RGBA16F_EXT 0x881A
#endif
void glGenQueriesEXT(GLsizei n, GLuint *ids);
void glBeginQueryEXT(GLenum target, GLuint id);
void glEndQueryEXT(GLenum target);
void glGetQueryObjectuivEXT(GLuint id, GLenum pname, GLuint *params);

typedef unsigned int mach_port_t;
typedef int kern_return_t;
typedef unsigned int vm_address_t;
typedef unsigned int vm_size_t;

long write(int, const void *, unsigned long);
long read(int, void *, unsigned long);
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
static int quiet;	/* corpus: no line per IOKit call */

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
    if (!quiet) {
        printf("[call]        conn %u sel %-4u scalars %u struct %zu\n", c, sel, inc, insc);
        if (ins && insc)
            dump("in", ins, insc, 128);
    }
    if (rpatch_armed)
        patch_renders();
    else if (patch_word >= 0 && !patched)
        patch_transfer();
    return orig_call(c, sel, in, inc, ins, insc, out, outc, outs, outsc);
}

static kern_return_t my_call_struct(mach_port_t c, uint32_t sel, const void *ins, size_t insc,
                                    void *outs, size_t *outsc)
{
    if (!quiet) {
        printf("[callStruct]  conn %u sel %-4u struct %zu\n", c, sel, insc);
        if (ins && insc)
            dump("in", ins, insc, 128);
    }
    if (rpatch_armed)
        patch_renders();
    else if (patch_word >= 0 && !patched)
        patch_transfer();
    return orig_call_struct(c, sel, ins, insc, outs, outsc);
}

static kern_return_t my_call_scalar(mach_port_t c, uint32_t sel, const uint64_t *in, uint32_t inc,
                                    uint64_t *out, uint32_t *outc)
{
    if (!quiet)
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
        if (!quiet)
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

/* The render commands (type 1) in the arena that are new or changed since
 * the last call, printed whole -- header, payload, records: what a case's
 * draw asked of the kernel (the 3D registers the kext builds from it,
 * rtemu.py).  M35: the punch-through pass's flags, an occlusion query's. */
static struct { uint32_t at; uint64_t hash; } seen_render[256];
static int nseen_render;

static void new_render_payloads(const char *label)
{
    unsigned span = ARENA_HI - ARENA_LO;
    unsigned char *img = malloc(span);
    static struct { uint32_t at; uint64_t hash; } now[256];
    int nnow = 0, printed = 0;

    if (!img)
        return;
    memset(img, 0, span);
    for (unsigned off = 0; off < span; off += 0x1000) {
        vm_size_t got = 0;
        vm_read_overwrite(mach_task_self(), ARENA_LO + off, 0x1000,
                          (vm_address_t)(img + off), &got);
    }
    for (unsigned p = 0; p + 0x200 < span && nnow < 256; p += 4) {
        const uint32_t *h = (const uint32_t *)(img + p), *pl;
        uint32_t off = h[4];
        uint64_t hash = 0xcbf29ce484222325ull;
        int old = 0;

        if (off < 0x14 || off > 0x80 || (off & 3) || img[p + off] != 1 ||
            (h[0] != off + 0xdc && h[1] != off + 0xdc && h[2] != off + 0xdc &&
             h[3] != off + 0xdc))
            continue;
        pl = (const uint32_t *)(img + p + off);
        for (unsigned i = 0; i < 0xdc / 4; i++)
            hash = (hash ^ pl[i]) * 0x100000001b3ull;
        now[nnow].at = ARENA_LO + p;
        now[nnow++].hash = hash;
        for (int i = 0; i < nseen_render; i++)
            old |= seen_render[i].at == ARENA_LO + p && seen_render[i].hash == hash;
        if (old)
            continue;
        printed++;
        printf("   render command at 0x%08x, header", ARENA_LO + p);
        for (unsigned i = 0; i < off / 4; i++)
            printf(" %08x", h[i]);
        printf("\n   payload:");
        for (unsigned i = 0; i < 0xdc / 4; i++)
            printf("%s%08x", i % 8 ? " " : "\n     ", pl[i]);
        printf("\n   records (%u at +0x%x):", h[1], h[2]);
        for (unsigned i = 0; i < h[1] && i < 64 && p + h[2] + 8 * i + 8 <= span; i++)
            printf("%s%08x %08x", i % 4 ? "  " : "\n     ",
                   ((const uint32_t *)(img + p + h[2]))[2 * i],
                   ((const uint32_t *)(img + p + h[2]))[2 * i + 1]);
        printf("\n");
    }
    for (int i = 0; i < nnow; i++) {
        seen_render[i].at = now[i].at;
        seen_render[i].hash = now[i].hash;
    }
    nseen_render = nnow;
    free(img);
    printf("   %s: %d new render command(s)\n", label, printed);
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

/* ---- "gltrace corpus OUTDIR FILE...": the shader oracle -------------------
 *
 * Each FILE holds a vertex shader, a line starting "// fragment", and a
 * fragment shader.  Every program is linked, given an array for each
 * attribute (the one named "p" a triangle over the whole 64x64 target,
 * the others 0.25 * (location + 1) + 0.0625 * vertex + 0.015625 * component)
 * and a value for each uniform (uniform i: i + 1 + 0.0625 * component;
 * ints 16 * (i + 1) + component; samplers a 4x4 texture or cube map, one
 * unit each), and drawn once.  Then every page of the process's IOKit
 * mappings (VM tag 21: the GPU buffers the kernel mapped in) that differs
 * from before the draw is written to OUTDIR/NAME.pages as {u32 address,
 * 4096 bytes} records: the USSE and PDS programs the GL driver made for the
 * shaders, their constants, the draw's state and command.  A first draw
 * with a trivial shader takes the driver's own setup out of the way; every
 * page that is not zero after it goes to OUTDIR/baseline.pages, so what
 * each case changed can be told byte by byte.
 *
 * A case's file may say how it is drawn, a line each (M34):
 *   // mode: points|lines|line_strip|line_loop|triangle_strip|triangle_fan
 *            (triangles by default; "p" then a few vertices across the target)
 *   // width: N        glLineWidth(N)
 *   // blend: alpha|add   SRC_ALPHA, ONE_MINUS_SRC_ALPHA -- or ONE, ONE
 *   // depth: less     a depth buffer, the test and the writes on
 *   // tex2d: W H [mip] [linear]   the 2D texture: its size, every level
 *                      (glGenerateMipmap), linear filters
 *   // cube: N [linear]   the cube map's faces N x N
 *   // draws: N        drawn N times in the frame, the 2D texture another
 *                      (a second of the same size) every other draw
 *   // indexed         glDrawElements, 16-bit indices
 *   // pages: all      every page that is not zero, not only those changed */

struct page_hash {
    uint32_t addr;
    uint64_t hash;
};
static struct page_hash *ph;
static int nph;

static uint64_t hash_page(const uint32_t *w)
{
    uint64_t h = 0xcbf29ce484222325ull;
    for (int i = 0; i < 0x400; i++)
        h = (h ^ w[i]) * 0x100000001b3ull;
    return h;
}

static int old_hash(uint32_t addr, uint64_t *h)
{
    int lo = 0, hi = nph - 1;
    while (lo <= hi) {
        int m = (lo + hi) / 2;
        if (ph[m].addr == addr) {
            *h = ph[m].hash;
            return 1;
        }
        if (ph[m].addr < addr)
            lo = m + 1;
        else
            hi = m - 1;
    }
    return 0;
}

/* the pages that changed since the last call -- or, with ALL, every page
 * that is not zero -- to PATH (none if NULL); returns how many */
static int changed_pages(const char *path, int all)
{
    static uint32_t pg[0x400];
    struct page_hash *np = 0;
    int nnp = 0, cap = 0, changed = 0, fd = -1;
    vm_address_t a = 0;

    if (path && (fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644)) < 0)
        printf("   cannot write %s\n", path);
    for (;;) {
        vm_size_t sz = 0;
        unsigned int depth = 99, cnt = 19;
        int info[19];
        if (vm_region_recurse_64(mach_task_self(), &a, &sz, &depth, info, &cnt))
            break;
        if (info[5] == 21 && (info[0] & 1) && sz <= 0x1000000) {
            for (uint32_t o = 0; o < sz; o += 0x1000) {
                vm_size_t got = 0;
                uint64_t h, was;
                if (vm_read_overwrite(mach_task_self(), a + o, 0x1000, (vm_address_t)pg, &got) ||
                    got != 0x1000)
                    continue;
                h = hash_page(pg);
                if (nnp == cap) {
                    struct page_hash *t = malloc(sizeof(*t) * (cap = cap ? cap * 2 : 4096));
                    if (!t)
                        break;
                    if (np) {
                        memcpy(t, np, sizeof(*t) * nnp);
                        free(np);
                    }
                    np = t;
                }
                np[nnp].addr = a + o;
                np[nnp].hash = h;
                nnp++;
                if (all) {
                    int k;
                    for (k = 0; k < 0x400 && !pg[k]; k++)
                        ;
                    if (k == 0x400)
                        continue;
                } else if (old_hash(a + o, &was) && was == h)
                    continue;
                changed++;
                if (fd >= 0) {
                    uint32_t at = a + o;
                    write(fd, &at, 4);
                    write(fd, pg, 0x1000);
                }
            }
        }
        a += sz;
    }
    if (fd >= 0)
        close(fd);
    free(ph);
    ph = np;
    nph = nnp;
    return changed;
}

static char shader_text[0x10000];

/* FILE's two shaders, split at the "// fragment" line */
static int read_shaders(const char *path, const char **vs, const char **fs)
{
    int fd = open(path, O_RDONLY), n = 0;
    long r;
    char *f;

    if (fd < 0)
        return 0;
    while (n < (int)sizeof(shader_text) - 1 &&
           (r = read(fd, shader_text + n, sizeof(shader_text) - 1 - n)) > 0)
        n += r;
    close(fd);
    shader_text[n] = 0;
    f = strstr(shader_text, "// fragment");
    if (!f)
        return 0;
    *vs = shader_text;
    *fs = strchr(f, '\n') ? strchr(f, '\n') + 1 : f + strlen(f);
    *f = 0;		/* the vertex shader ends where the line starts */
    return 1;
}

static GLuint compile_logged(GLenum type, const char *src, const char *what)
{
    GLuint s = glCreateShader(type);
    GLint ok = 0;
    glShaderSource(s, 1, &src, 0);
    glCompileShader(s);
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetShaderInfoLog(s, sizeof log, 0, log);
        printf("   %s shader does not compile: %s\n", what, log);
        return 0;
    }
    return s;
}

static GLuint link_logged(const char *vs, const char *fs)
{
    GLuint v = compile_logged(GL_VERTEX_SHADER, vs, "vertex");
    GLuint f = compile_logged(GL_FRAGMENT_SHADER, fs, "fragment");
    GLuint p;
    GLint ok = 0;

    if (!v || !f)
        return 0;
    p = glCreateProgram();
    glAttachShader(p, v);
    glAttachShader(p, f);
    glLinkProgram(p);
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetProgramInfoLog(p, sizeof log, 0, log);
        printf("   does not link: %s\n", log);
        return 0;
    }
    return p;
}

static float attr_data[16][8][4];

/* how a case is drawn (its "// key: value" lines) */
struct case_opts {
    GLenum mode;
    int nverts;
    float width;
    int blend;          /* 0 off, 1 alpha, 2 add */
    int depth;
    int tex_w, tex_h, tex_mip, tex_linear;
    int cube_n, cube_linear;
    int draws;
    int indexed;
    int all_pages;
    /* M35 */
    int texfmt;         /* TEXFMT_* */
    int compare;        /* the 2D texture's compare mode on (EXT_shadow_samplers) */
    int maxlevel;       /* GL_TEXTURE_MAX_LEVEL_APPLE, -1 not set */
    int aniso;          /* GL_TEXTURE_MAX_ANISOTROPY_EXT, 0 not set */
    int wrap;           /* 0 clamp to edge, 1 repeat, 2 mirrored repeat */
    int rtfmt;          /* the target: 0 the RGBA8 texture, else a renderbuffer of RT_* */
    int query;          /* an occlusion query around the draws */
    float offset_f, offset_u;   /* glPolygonOffset, when either is set */
};

enum { TEXFMT_RGBA8, TEXFMT_RGB565, TEXFMT_RGBA4444, TEXFMT_RGBA5551, TEXFMT_RGB8, TEXFMT_L8,
       TEXFMT_A8, TEXFMT_LA8, TEXFMT_HALF, TEXFMT_FLOAT, TEXFMT_BGRA8, TEXFMT_SRGB8A8,
       TEXFMT_DEPTH16, TEXFMT_DEPTH32, TEXFMT_DEPTH24S8, TEXFMT_N };
static const char *texfmt_names[TEXFMT_N] = {
    "rgba8", "rgb565", "rgba4444", "rgba5551", "rgb8", "l8", "a8", "la8", "half", "float",
    "bgra8", "srgb8a8", "depth16", "depth32", "depth24s8",
};
enum { RT_TEX, RT_RGB565, RT_RGBA4, RT_RGB5A1, RT_RGBA8, RT_HALF, RT_N };
static const char *rt_names[RT_N] = { "tex", "rgb565", "rgba4", "rgb5a1", "rgba8", "half" };

static const char *opt_value(const char *text, const char *key)
{
    char pat[64];
    const char *at;

    snprintf(pat, sizeof pat, "// %s", key);
    at = strstr(text, pat);
    if (!at)
        return NULL;
    at += strlen(pat);
    while (*at == ':' || *at == ' ')
        at++;
    return at;
}

static void case_options(const char *text, struct case_opts *o)
{
    const char *v;

    memset(o, 0, sizeof(*o));
    o->mode = GL_TRIANGLES;
    o->nverts = 3;
    o->draws = 1;
    o->tex_w = o->tex_h = 4;
    o->cube_n = 4;
    if ((v = opt_value(text, "mode"))) {
        static const struct { const char *name; GLenum mode; int n; } m[] = {
            { "points", GL_POINTS, 3 }, { "line_strip", GL_LINE_STRIP, 4 },
            { "line_loop", GL_LINE_LOOP, 4 }, { "lines", GL_LINES, 4 },
            { "triangle_strip", GL_TRIANGLE_STRIP, 4 }, { "triangle_fan", GL_TRIANGLE_FAN, 4 },
        };
        for (unsigned i = 0; i < sizeof(m) / sizeof(m[0]); i++)
            if (!strncmp(v, m[i].name, strlen(m[i].name))) {
                o->mode = m[i].mode;
                o->nverts = m[i].n;
                break;
            }
    }
    if ((v = opt_value(text, "width")))
        o->width = atof(v);
    if ((v = opt_value(text, "blend")))
        o->blend = !strncmp(v, "add", 3) ? 2 : 1;
    o->depth = opt_value(text, "depth") != NULL;
    if ((v = opt_value(text, "tex2d"))) {
        o->tex_w = atoi(v);
        while (*v && *v != ' ')
            v++;
        o->tex_h = atoi(v);
        o->tex_mip = strstr(v, "mip") && strstr(v, "mip") < strchr(v, '\n');
        o->tex_linear = strstr(v, "linear") && strstr(v, "linear") < strchr(v, '\n');
    }
    if ((v = opt_value(text, "cube"))) {
        o->cube_n = atoi(v);
        o->cube_linear = strstr(v, "linear") && strstr(v, "linear") < strchr(v, '\n');
    }
    if ((v = opt_value(text, "draws")))
        o->draws = atoi(v) > 0 ? atoi(v) : 1;
    o->indexed = opt_value(text, "indexed") != NULL;
    o->all_pages = (v = opt_value(text, "pages")) && !strncmp(v, "all", 3);
    o->maxlevel = -1;
    if ((v = opt_value(text, "texfmt")))
        for (int i = 0; i < TEXFMT_N; i++)
            if (!strncmp(v, texfmt_names[i], strlen(texfmt_names[i])) &&
                (v[strlen(texfmt_names[i])] == '\n' || v[strlen(texfmt_names[i])] == ' ' ||
                 !v[strlen(texfmt_names[i])]))
                o->texfmt = i;
    o->compare = opt_value(text, "compare") != NULL;
    if ((v = opt_value(text, "maxlevel")))
        o->maxlevel = atoi(v);
    if ((v = opt_value(text, "aniso")))
        o->aniso = atoi(v);
    if ((v = opt_value(text, "wrap")))
        o->wrap = !strncmp(v, "repeat", 6) ? 1 : !strncmp(v, "mirror", 6) ? 2 : 0;
    if ((v = opt_value(text, "rt")))
        for (int i = 1; i < RT_N; i++)
            if (!strncmp(v, rt_names[i], strlen(rt_names[i])))
                o->rtfmt = i;
    o->query = opt_value(text, "query") != NULL;
    if ((v = opt_value(text, "offset"))) {
        o->offset_f = atof(v);
        while (*v && *v != ' ' && *v != '\n')
            v++;
        o->offset_u = atof(v);
    }
}

static struct case_opts copt;
static GLuint corpus_target_tex;    /* the 64 x 64 texture the FBO draws into (main's) */

static void corpus_attribs(GLuint prog)
{
    static const float tri[3][4] = { { -1, -1, 0, 1 }, { 3, -1, 0, 1 }, { -1, 3, 0, 1 } };
    /* for the other modes: a quad's corners, in strip order for the
     * strips and lines, round it for the fans and loops */
    static const float quad[4][4] = { { -0.75f, -0.75f, 0, 1 }, { 0.75f, -0.75f, 0, 1 },
                                      { -0.75f, 0.75f, 0, 1 }, { 0.75f, 0.75f, 0, 1 } };
    static const float round[4][4] = { { -0.75f, -0.75f, 0, 1 }, { 0.75f, -0.75f, 0, 1 },
                                       { 0.75f, 0.75f, 0, 1 }, { -0.75f, 0.75f, 0, 1 } };
    int round_mode = copt.mode == GL_TRIANGLE_FAN || copt.mode == GL_LINE_LOOP;
    GLint n = 0;

    for (int l = 0; l < 16; l++)
        glDisableVertexAttribArray(l);
    glGetProgramiv(prog, GL_ACTIVE_ATTRIBUTES, &n);
    for (int i = 0; i < n; i++) {
        char name[64];
        GLint size = 0, loc;
        GLenum type = 0;
        int cols, comps;

        glGetActiveAttrib(prog, i, sizeof name, 0, &size, &type, name);
        loc = glGetAttribLocation(prog, name);
        cols = type == GL_FLOAT_MAT2 ? 2 : type == GL_FLOAT_MAT3 ? 3 : type == GL_FLOAT_MAT4 ? 4 : 1;
        comps = type == GL_FLOAT ? 1 :
                type == GL_FLOAT_VEC2 || type == GL_FLOAT_MAT2 ? 2 :
                type == GL_FLOAT_VEC3 || type == GL_FLOAT_MAT3 ? 3 : 4;
        printf("   attribute %s: type 0x%x, %d, location %d\n", name, type, size, loc);
        for (int c = 0; c < cols && loc >= 0 && loc + c < 16; c++) {
            for (int k = 0; k < copt.nverts; k++)
                for (int j = 0; j < 4; j++)
                    attr_data[loc + c][k][j] = strcmp(name, "p") ?
                        0.25f * (loc + c + 1) + 0.0625f * k + 0.015625f * j :
                        copt.mode == GL_TRIANGLES ? tri[k][j] :
                        round_mode ? round[k][j] : quad[k][j];
            glEnableVertexAttribArray(loc + c);
            glVertexAttribPointer(loc + c, comps, GL_FLOAT, GL_FALSE, 16, attr_data[loc + c]);
        }
    }
}

static void corpus_uniforms(GLuint prog, GLuint tex2d, GLuint texcube)
{
    GLint n = 0, unit = 0;

    glGetProgramiv(prog, GL_ACTIVE_UNIFORMS, &n);
    for (int i = 0; i < n; i++) {
        static float f[64 * 16];
        static GLint iv[64 * 4];
        char name[64];
        GLint size = 0, loc;
        GLenum type = 0;

        glGetActiveUniform(prog, i, sizeof name, 0, &size, &type, name);
        loc = glGetUniformLocation(prog, name);
        if (size > 64)
            size = 64;
        for (int e = 0; e < size; e++)
            for (int j = 0; j < 16; j++) {
                f[e * 16 + j] = (i + 1) + 0.0625f * j;
                if (j < 4)
                    iv[e * 4 + j] = 16 * (i + 1) + j;
            }
        printf("   uniform %s: type 0x%x, %d, location %d, value %d + 0.0625 * component\n",
               name, type, size, loc, i + 1);
        switch (type) {
        case GL_FLOAT: glUniform1fv(loc, size, f); break;
        case GL_FLOAT_VEC2: glUniform2fv(loc, size, f); break;
        case GL_FLOAT_VEC3: glUniform3fv(loc, size, f); break;
        case GL_FLOAT_VEC4: glUniform4fv(loc, size, f); break;
        case GL_INT: case GL_BOOL: glUniform1iv(loc, size, iv); break;
        case GL_INT_VEC2: case GL_BOOL_VEC2: glUniform2iv(loc, size, iv); break;
        case GL_INT_VEC3: case GL_BOOL_VEC3: glUniform3iv(loc, size, iv); break;
        case GL_INT_VEC4: case GL_BOOL_VEC4: glUniform4iv(loc, size, iv); break;
        case GL_FLOAT_MAT2: glUniformMatrix2fv(loc, size, GL_FALSE, f); break;
        case GL_FLOAT_MAT3: glUniformMatrix3fv(loc, size, GL_FALSE, f); break;
        case GL_FLOAT_MAT4: glUniformMatrix4fv(loc, size, GL_FALSE, f); break;
        case GL_SAMPLER_2D:
        case GL_SAMPLER_CUBE:
            for (int e = 0; e < size && unit < 8; e++, unit++) {
                glActiveTexture(GL_TEXTURE0 + unit);
                if (type == GL_SAMPLER_2D)
                    glBindTexture(GL_TEXTURE_2D, tex2d);
                else
                    glBindTexture(GL_TEXTURE_CUBE_MAP, texcube);
                iv[e] = unit;
            }
            glUniform1iv(loc, size, iv);
            glActiveTexture(GL_TEXTURE0);
            break;
        }
    }
}

/* a float as a half (no denormals; the corpus's values are 0..1) */
static unsigned short to_half(float f)
{
    union { float f; uint32_t u; } v = { f };
    int e = (int)((v.u >> 23) & 0xff) - 127 + 15;

    if (f == 0 || e <= 0)
        return 0;
    return (unsigned short)((v.u >> 16 & 0x8000) | (e > 30 ? 0x7c00 : e << 10 |
                                                     (v.u >> 13 & 0x3ff)));
}

/* a w x h texture of the corpus's texels (texel i: (i & 3) * 64, (i >> 2) *
 * 64 on a 4 x 4 -- the same pattern repeated), blue b -- in the case's
 * format (M35: the texture words iOS writes for each), its compare mode,
 * maximum level, anisotropy and wrap */
static void corpus_tex2d(GLuint tex, int w, int h, int mip, int linear, int b)
{
    unsigned char *t = malloc((size_t)w * h * 16);
    GLenum internal = GL_RGBA, format = GL_RGBA, type = GL_UNSIGNED_BYTE;
    static const GLenum wraps[3] = { GL_CLAMP_TO_EDGE, GL_REPEAT, GL_MIRRORED_REPEAT };

    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            int i = y * w + x;
            unsigned r = (x & 3) * 64, g = (y & 3) * 64, a = 255 - 16 * ((x + y) & 3);
            unsigned short *q16 = (unsigned short *)t + i;

            switch (copt.texfmt) {
            case TEXFMT_RGB565:
                *q16 = (unsigned short)((r >> 3) << 11 | (g >> 2) << 5 | b >> 3);
                format = internal = GL_RGB;
                type = GL_UNSIGNED_SHORT_5_6_5;
                break;
            case TEXFMT_RGBA4444:
                *q16 = (unsigned short)((r >> 4) << 12 | (g >> 4) << 8 | (b >> 4) << 4 | a >> 4);
                type = GL_UNSIGNED_SHORT_4_4_4_4;
                break;
            case TEXFMT_RGBA5551:
                *q16 = (unsigned short)((r >> 3) << 11 | (g >> 3) << 6 | (b >> 3) << 1 | a >> 7);
                type = GL_UNSIGNED_SHORT_5_5_5_1;
                break;
            case TEXFMT_RGB8:
                t[3 * i] = r, t[3 * i + 1] = g, t[3 * i + 2] = b;
                format = internal = GL_RGB;
                break;
            case TEXFMT_L8:
                t[i] = r + g / 4;
                format = internal = GL_LUMINANCE;
                break;
            case TEXFMT_A8:
                t[i] = r + g / 4;
                format = internal = GL_ALPHA;
                break;
            case TEXFMT_LA8:
                t[2 * i] = r, t[2 * i + 1] = g;
                format = internal = GL_LUMINANCE_ALPHA;
                break;
            case TEXFMT_HALF:
                q16 = (unsigned short *)t + 4 * i;
                q16[0] = to_half(r / 255.0f), q16[1] = to_half(g / 255.0f);
                q16[2] = to_half(b / 255.0f), q16[3] = to_half(a / 255.0f);
                type = GL_HALF_FLOAT_OES;
                break;
            case TEXFMT_FLOAT:
                ((float *)t)[4 * i] = r / 255.0f, ((float *)t)[4 * i + 1] = g / 255.0f;
                ((float *)t)[4 * i + 2] = b / 255.0f, ((float *)t)[4 * i + 3] = a / 255.0f;
                type = GL_FLOAT;
                break;
            case TEXFMT_BGRA8:
                t[4 * i] = b, t[4 * i + 1] = g, t[4 * i + 2] = r, t[4 * i + 3] = a;
                format = internal = GL_BGRA_EXT;
                break;
            case TEXFMT_SRGB8A8:
                t[4 * i] = r, t[4 * i + 1] = g, t[4 * i + 2] = b, t[4 * i + 3] = a;
                format = internal = GL_SRGB_ALPHA_EXT;
                break;
            case TEXFMT_DEPTH16:
                *q16 = (unsigned short)(i * 65535 / (w * h));
                format = internal = GL_DEPTH_COMPONENT;
                type = GL_UNSIGNED_SHORT;
                break;
            case TEXFMT_DEPTH32:
                ((uint32_t *)t)[i] = (uint32_t)((double)i / (w * h) * 4294967295.0);
                format = internal = GL_DEPTH_COMPONENT;
                type = GL_UNSIGNED_INT;
                break;
            case TEXFMT_DEPTH24S8:
                ((uint32_t *)t)[i] = (uint32_t)(i * 0xffffff / (w * h)) << 8 | (i & 0xff);
                format = internal = GL_DEPTH_STENCIL_OES;
                type = GL_UNSIGNED_INT_24_8_OES;
                break;
            default:
                t[4 * i] = r, t[4 * i + 1] = g, t[4 * i + 2] = b, t[4 * i + 3] = 255;
            }
        }
    glBindTexture(GL_TEXTURE_2D, tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, internal, w, h, 0, format, type, t);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    if (mip)
        glGenerateMipmap(GL_TEXTURE_2D);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                    mip ? (linear ? GL_LINEAR_MIPMAP_LINEAR : GL_NEAREST_MIPMAP_NEAREST) :
                    linear ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, linear ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wraps[copt.wrap]);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wraps[copt.wrap]);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE_EXT,
                    copt.compare ? GL_COMPARE_REF_TO_TEXTURE_EXT : GL_NONE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_FUNC_EXT, GL_LEQUAL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL_APPLE, copt.maxlevel >= 0 ? copt.maxlevel : 1000);
    if (copt.aniso)
        glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY_EXT, copt.aniso);
    else if (strstr((const char *)glGetString(GL_EXTENSIONS), "anisotropic"))
        glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY_EXT, 1);
    glGetError();       /* (a parameter this iOS does not take) */
    free(t);
}

static void corpus_cube(GLuint tex, int n, int linear)
{
    unsigned char *t = malloc((size_t)n * n * 4);

    glBindTexture(GL_TEXTURE_CUBE_MAP, tex);
    for (int face = 0; face < 6; face++) {
        for (int i = 0; i < n * n; i++) {
            t[i * 4] = (i % n & 3) * 64;
            t[i * 4 + 1] = (i / n & 3) * 64;
            t[i * 4 + 2] = 40 * face;
            t[i * 4 + 3] = 255;
        }
        glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + face, 0, GL_RGBA, n, n, 0, GL_RGBA,
                     GL_UNSIGNED_BYTE, t);
    }
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, linear ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, linear ? GL_LINEAR : GL_NEAREST);
    free(t);
}

static int corpus(int argc, char **argv)
{
    static const char *vs0 = "attribute vec4 p; void main() { gl_Position = p; }";
    static const char *fs0 = "void main() { gl_FragColor = vec4(1.0); }";
    unsigned char texels[4 * 4 * 4];
    GLuint tex2d, tex2d_b, texcube, p0, depth;
    const char *out = argv[2];
    char path0[512];
    int n0;
    int ok = 0;

    quiet = 1;
    printf("== extensions: %s\n", (const char *)glGetString(GL_EXTENSIONS));
    for (int i = 0; i < 16; i++) {
        texels[i * 4] = (i & 3) * 64;
        texels[i * 4 + 1] = (i >> 2) * 64;
        texels[i * 4 + 2] = 128;
        texels[i * 4 + 3] = 255;
    }
    /* a depth buffer for the cases that test depth */
    glGenRenderbuffers(1, &depth);
    glBindRenderbuffer(GL_RENDERBUFFER, depth);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, 64, 64);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depth);
    glGenTextures(1, &tex2d_b);
    glGenTextures(1, &tex2d);
    glBindTexture(GL_TEXTURE_2D, tex2d);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 4, 4, 0, GL_RGBA, GL_UNSIGNED_BYTE, texels);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenTextures(1, &texcube);
    glBindTexture(GL_TEXTURE_CUBE_MAP, texcube);
    for (int face = 0; face < 6; face++) {
        for (int i = 0; i < 16; i++)
            texels[i * 4 + 2] = 40 * face;
        glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + face, 0, GL_RGBA, 4, 4, 0, GL_RGBA,
                     GL_UNSIGNED_BYTE, texels);
    }
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glViewport(0, 0, 64, 64);

    /* the driver's own first-use setup, out of the way */
    p0 = link_logged(vs0, fs0);
    if (!p0)
        return printf("== corpus: the trivial shader does not build\n"), 1;
    for (int k = 0; k < 2; k++) {
        glUseProgram(p0);
        corpus_attribs(p0);
        glClear(GL_COLOR_BUFFER_BIT);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glFinish();
    }
    snprintf(path0, sizeof path0, "%s/baseline.pages", out);
    n0 = changed_pages(path0, 1);
    printf("== corpus: %d pages in the GPU buffers, %d not zero -> %s\n", nph, n0, path0);
    new_render_payloads("baseline");

    for (int i = 3; i < argc; i++) {
        const char *vs, *fs, *base = strrchr(argv[i], '/'), *dot;
        char name[128], path[512];
        unsigned char px[4] = { 0 };
        GLuint prog;
        int n;

        base = base ? base + 1 : argv[i];
        dot = strrchr(base, '.');
        snprintf(name, sizeof name, "%.*s", dot ? (int)(dot - base) : (int)strlen(base), base);
        printf("== case %s\n", name);
        if (!read_shaders(argv[i], &vs, &fs)) {
            printf("   no \"// fragment\" line in %s\n", argv[i]);
            continue;
        }
        case_options(vs, &copt);
        if (copt.mode != GL_TRIANGLES || copt.width || copt.blend || copt.depth ||
            copt.tex_w != 4 || copt.tex_h != 4 || copt.tex_mip || copt.tex_linear ||
            copt.cube_n != 4 || copt.cube_linear || copt.draws > 1 || copt.indexed)
            printf("   drawn: mode 0x%x, %d vertices, width %g, blend %d, depth %d, tex2d %dx%d%s%s, "
                   "cube %d%s, %d draws%s\n", copt.mode, copt.nverts, copt.width, copt.blend,
                   copt.depth, copt.tex_w, copt.tex_h, copt.tex_mip ? " mip" : "",
                   copt.tex_linear ? " linear" : "", copt.cube_n, copt.cube_linear ? " linear" : "",
                   copt.draws, copt.indexed ? ", indexed" : "");
        if (copt.texfmt || copt.compare || copt.maxlevel >= 0 || copt.aniso || copt.wrap ||
            copt.rtfmt || copt.query || copt.offset_f || copt.offset_u)
            printf("   texture %s%s, maxlevel %d, aniso %d, wrap %d; target %s; query %d; "
                   "offset %g %g\n", texfmt_names[copt.texfmt], copt.compare ? " compare" : "",
                   copt.maxlevel, copt.aniso, copt.wrap, rt_names[copt.rtfmt], copt.query,
                   copt.offset_f, copt.offset_u);
        /* the textures as the case wants them, made before the pages are
         * taken: their upload is not the draw's */
        corpus_tex2d(tex2d, copt.tex_w, copt.tex_h, copt.tex_mip, copt.tex_linear, 128);
        corpus_tex2d(tex2d_b, copt.tex_w, copt.tex_h, copt.tex_mip, copt.tex_linear, 32);
        corpus_cube(texcube, copt.cube_n, copt.cube_linear);
        changed_pages(0, 0);	/* whatever happened since the last case */
        prog = link_logged(vs, fs);
        if (!prog)
            continue;
        glUseProgram(prog);
        corpus_attribs(prog);
        corpus_uniforms(prog, tex2d, texcube);
        if (copt.width)
            glLineWidth(copt.width);
        if (copt.blend) {
            glEnable(GL_BLEND);
            if (copt.blend == 2)
                glBlendFunc(GL_ONE, GL_ONE);
            else
                glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        }
        if (copt.depth) {
            glEnable(GL_DEPTH_TEST);
            glDepthFunc(GL_LESS);
            glDepthMask(GL_TRUE);
        }
        /* another target format: a renderbuffer of it in the texture's place */
        GLuint rb = 0, query = 0, passed = 0;
        if (copt.rtfmt) {
            static const GLenum rtf[RT_N] = { 0, GL_RGB565, GL_RGBA4, GL_RGB5_A1, GL_RGBA8_OES,
                                              GL_RGBA16F_EXT };
            glGenRenderbuffers(1, &rb);
            glBindRenderbuffer(GL_RENDERBUFFER, rb);
            glRenderbufferStorage(GL_RENDERBUFFER, rtf[copt.rtfmt], 64, 64);
            glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb);
            printf("   target %s: framebuffer status 0x%x\n", rt_names[copt.rtfmt],
                   glCheckFramebufferStatus(GL_FRAMEBUFFER));
        }
        if (copt.offset_f || copt.offset_u) {
            glEnable(GL_POLYGON_OFFSET_FILL);
            glPolygonOffset(copt.offset_f, copt.offset_u);
        }
        glClearColor(0, 0, 0, 0);
        glClearDepthf(1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        if (copt.query) {
            glGenQueriesEXT(1, &query);
            glBeginQueryEXT(GL_ANY_SAMPLES_PASSED_EXT, query);
        }
        for (int d = 0; d < copt.draws; d++) {
            static const GLushort idx[8] = { 0, 1, 2, 3, 0, 2, 1, 3 };
            if (copt.draws > 1) {
                glActiveTexture(GL_TEXTURE0);
                glBindTexture(GL_TEXTURE_2D, d & 1 ? tex2d_b : tex2d);
            }
            if (copt.indexed)
                glDrawElements(copt.mode, copt.nverts, GL_UNSIGNED_SHORT, idx);
            else
                glDrawArrays(copt.mode, 0, copt.nverts);
        }
        if (copt.query)
            glEndQueryEXT(GL_ANY_SAMPLES_PASSED_EXT);
        glFinish();
        glReadPixels(32, 32, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
        if (copt.query) {
            glGetQueryObjectuivEXT(query, GL_QUERY_RESULT_EXT, &passed);
            printf("   occlusion query: %u\n", passed);
        }
        if (copt.offset_f || copt.offset_u)
            glDisable(GL_POLYGON_OFFSET_FILL);
        if (rb) {
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                                   corpus_target_tex, 0);
            glDeleteRenderbuffers(1, &rb);
        }
        if (copt.blend)
            glDisable(GL_BLEND);
        if (copt.depth)
            glDisable(GL_DEPTH_TEST);
        if (copt.width)
            glLineWidth(1);
        snprintf(path, sizeof path, "%s/%s.pages", out, name);
        n = changed_pages(path, copt.all_pages);
        printf("   drawn: GL error 0x%x, centre pixel %02x %02x %02x %02x, %d pages changed -> %s\n",
               glGetError(), px[0], px[1], px[2], px[3], n, path);
        new_render_payloads(name);
        ok++;
    }
    printf("== corpus done: %d of %d cases drawn\n", ok, argc - 3);
    return 0;
}


/* ---- "gltrace es1 OUTDIR [CASE...]": OpenGL ES 1's fixed function (M35) ----
 *
 * The same GPU, the same driver: what GLES 2 has no word for -- flat shading,
 * logic ops, the alpha test, user clip planes, point sprites, fog, two-sided
 * lighting -- shows how the hardware does it.  Each case draws a triangle
 * over the 64 x 64 target (red, green, blue at its corners, texture
 * coordinates 0..1) -- points or lines where it says -- as corpus mode does:
 * OUTDIR/NAME.pages (the pages that are not zero), the render commands in
 * the log.  No CASE: all of them. */
#define GL_FLAT_ 0x1D00
#define GL_SMOOTH_ 0x1D01
#define GL_COLOR_LOGIC_OP_ 0x0BF2
#define GL_ALPHA_TEST_ 0x0BC0
#define GL_CLIP_PLANE0_ 0x3000
#define GL_POINT_SPRITE_OES_ 0x8861
#define GL_COORD_REPLACE_OES_ 0x8862
#define GL_POINT_SIZE_ARRAY_OES_ 0x8B9C
#define GL_FOG_ 0x0B60
#define GL_FOG_MODE_ 0x0B65
#define GL_FOG_START_ 0x0B63
#define GL_FOG_END_ 0x0B64
#define GL_FOG_COLOR_ 0x0B66
#define GL_LINE_SMOOTH_ 0x0B20
#define GL_LIGHTING_ 0x0B50
#define GL_LIGHT0_ 0x4000
#define GL_LIGHT_MODEL_TWO_SIDE_ 0x0B52
#define GL_DIFFUSE_ 0x1201
#define GL_POSITION_ 0x1203
#define GL_AMBIENT_AND_DIFFUSE_ 0x1602
#define GL_TEXTURE_ENV_ 0x2300
#define GL_TEXTURE_ENV_MODE_ 0x2200
#define GL_ADD_ 0x0104
#define GL_MODULATE_ 0x2100
#define GL_VERTEX_ARRAY_ 0x8074
#define GL_NORMAL_ARRAY_ 0x8075
#define GL_COLOR_ARRAY_ 0x8076
#define GL_TEXTURE_COORD_ARRAY_ 0x8078
#define GL_MODELVIEW_ 0x1700
#define GL_PROJECTION_ 0x1701
#define GL_FRAMEBUFFER_OES_ 0x8D40
#define GL_RENDERBUFFER_OES_ 0x8D41
#define GL_COLOR_ATTACHMENT0_OES_ 0x8CE0
#define GL_DEPTH_ATTACHMENT_OES_ 0x8D00
#define GL_DEPTH_COMPONENT16_OES_ 0x81A5
void glShadeModel(GLenum mode);
void glLogicOp(GLenum opcode);
void glAlphaFunc(GLenum func, GLclampf ref);
void glClipPlanef(GLenum plane, const GLfloat *equation);
void glPointSize(GLfloat size);
void glTexEnvi(GLenum target, GLenum pname, GLint param);
void glFogf(GLenum pname, GLfloat param);
void glFogfv(GLenum pname, const GLfloat *params);
void glEnableClientState(GLenum array);
void glDisableClientState(GLenum array);
void glVertexPointer(GLint size, GLenum type, GLsizei stride, const GLvoid *pointer);
void glColorPointer(GLint size, GLenum type, GLsizei stride, const GLvoid *pointer);
void glTexCoordPointer(GLint size, GLenum type, GLsizei stride, const GLvoid *pointer);
void glNormalPointer(GLenum type, GLsizei stride, const GLvoid *pointer);
void glPointSizePointerOES(GLenum type, GLsizei stride, const GLvoid *pointer);
void glMatrixMode(GLenum mode);
void glLoadIdentity(void);
void glLightModelf(GLenum pname, GLfloat param);
void glLightfv(GLenum light, GLenum pname, const GLfloat *params);
void glMaterialfv(GLenum face, GLenum pname, const GLfloat *params);
void glGenFramebuffersOES(GLsizei n, GLuint *framebuffers);
void glBindFramebufferOES(GLenum target, GLuint framebuffer);
void glFramebufferTexture2DOES(GLenum target, GLenum attachment, GLenum textarget,
                               GLuint texture, GLint level);
void glGenRenderbuffersOES(GLsizei n, GLuint *renderbuffers);
void glBindRenderbufferOES(GLenum target, GLuint renderbuffer);
void glRenderbufferStorageOES(GLenum target, GLenum internalformat, GLsizei width,
                              GLsizei height);
void glFramebufferRenderbufferOES(GLenum target, GLenum attachment, GLenum renderbuffertarget,
                                  GLuint renderbuffer);

static const char *es1_names[] = {
    "e00_smooth", "e01_flat", "e02_flat_textured", "e03_logic_xor", "e04_logic_and",
    "e05_logic_or", "e06_logic_invert", "e07_logic_copy_inverted", "e08_logic_nand",
    "e09_logic_set", "e10_logic_clear", "e11_alpha_greater", "e12_alpha_greater_depth",
    "e13_alpha_less", "e14_clip_plane", "e15_point_sprite", "e16_point_size_array",
    "e17_fog_linear", "e18_line_smooth", "e19_polygon_offset", "e20_two_side_light",
    "e21_texenv_add", "e22_points", "e23_lines", "e24_flat_lines",
};
#define NES1 (int)(sizeof(es1_names) / sizeof(es1_names[0]))

static void es1_case(int k)
{
    static const float tri[3][2] = { { -1, -1 }, { 3, -1 }, { -1, 3 } };
    static const float pts[4][2] = { { -0.5f, -0.5f }, { 0.5f, -0.5f }, { -0.5f, 0.5f },
                                     { 0.5f, 0.5f } };
    static const float col[4][4] = { { 1, 0, 0, 0.25f }, { 0, 1, 0, 0.75f }, { 0, 0, 1, 1 },
                                     { 1, 1, 0, 0.5f } };
    static const float tc[4][2] = { { 0, 0 }, { 2, 0 }, { 0, 2 }, { 1, 1 } };
    static const float nrm[4][3] = { { 0, 0, 1 }, { 0, 0, 1 }, { 0, 0, 1 }, { 0, 0, 1 } };
    static const float sizes[4] = { 4, 8, 16, 32 };
    static const GLenum ops[] = { 0x1506 /* XOR */, 0x1501 /* AND */, 0x1507 /* OR */,
                                  0x150A /* INVERT */, 0x150C /* COPY_INVERTED */,
                                  0x150E /* NAND */, 0x150F /* SET */, 0x1500 /* CLEAR */ };
    GLenum mode = GL_TRIANGLES;
    int n = 3;

    glEnableClientState(GL_VERTEX_ARRAY_);
    glEnableClientState(GL_COLOR_ARRAY_);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY_);
    glVertexPointer(2, GL_FLOAT, 0, k >= 15 && k != 17 && k != 19 && k != 20 && k != 21 ? pts : tri);
    glColorPointer(4, GL_FLOAT, 0, col);
    glTexCoordPointer(2, GL_FLOAT, 0, tc);
    switch (k) {
    case 1: glShadeModel(GL_FLAT_); break;
    case 2: glShadeModel(GL_FLAT_); glEnable(GL_TEXTURE_2D); break;
    case 3: case 4: case 5: case 6: case 7: case 8: case 9: case 10:
        glEnable(GL_COLOR_LOGIC_OP_);
        glLogicOp(ops[k - 3]);
        break;
    case 11: glEnable(GL_ALPHA_TEST_); glAlphaFunc(GL_GREATER, 0.5f); break;
    case 12:
        glEnable(GL_ALPHA_TEST_);
        glAlphaFunc(GL_GREATER, 0.5f);
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LESS);
        break;
    case 13: glEnable(GL_ALPHA_TEST_); glAlphaFunc(GL_LESS, 0.5f); break;
    case 14: {
        static const float eq[4] = { 1, 1, 0, 0 };
        glClipPlanef(GL_CLIP_PLANE0_, eq);
        glEnable(GL_CLIP_PLANE0_);
        break;
    }
    case 15:
        glEnable(GL_TEXTURE_2D);
        glEnable(GL_POINT_SPRITE_OES_);
        glTexEnvi(GL_POINT_SPRITE_OES_, GL_COORD_REPLACE_OES_, GL_TRUE);
        glPointSize(16);
        mode = GL_POINTS, n = 4;
        break;
    case 16:
        glEnableClientState(GL_POINT_SIZE_ARRAY_OES_);
        glPointSizePointerOES(GL_FLOAT, 0, sizes);
        mode = GL_POINTS, n = 4;
        break;
    case 17: {
        static const float fogc[4] = { 1, 1, 1, 1 };
        glEnable(GL_FOG_);
        glFogf(GL_FOG_MODE_, GL_LINEAR);
        glFogf(GL_FOG_START_, 0);
        glFogf(GL_FOG_END_, 1);
        glFogfv(GL_FOG_COLOR_, fogc);
        break;
    }
    case 18: glEnable(GL_LINE_SMOOTH_); mode = GL_LINES, n = 4; break;
    case 19:
        glEnable(GL_DEPTH_TEST);
        glEnable(GL_POLYGON_OFFSET_FILL);
        glPolygonOffset(1, 2);
        break;
    case 20: {
        static const float pos[4] = { 0, 0, 1, 0 }, dif[4] = { 1, 1, 1, 1 };
        glEnable(GL_LIGHTING_);
        glEnable(GL_LIGHT0_);
        glLightfv(GL_LIGHT0_, GL_POSITION_, pos);
        glLightfv(GL_LIGHT0_, GL_DIFFUSE_, dif);
        glMaterialfv(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE_, dif);
        glLightModelf(GL_LIGHT_MODEL_TWO_SIDE_, 1);
        glEnableClientState(GL_NORMAL_ARRAY_);
        glNormalPointer(GL_FLOAT, 0, nrm);
        break;
    }
    case 21: glEnable(GL_TEXTURE_2D); glTexEnvi(GL_TEXTURE_ENV_, GL_TEXTURE_ENV_MODE_, GL_ADD_); break;
    case 22: mode = GL_POINTS, n = 4; glPointSize(8); break;
    case 23: mode = GL_LINES, n = 4; break;
    case 24: glShadeModel(GL_FLAT_); mode = GL_LINES, n = 4; break;
    }
    glDrawArrays(mode, 0, n);
    glFinish();
    glShadeModel(GL_SMOOTH_);
    glDisable(GL_TEXTURE_2D);
    glDisable(GL_COLOR_LOGIC_OP_);
    glDisable(GL_ALPHA_TEST_);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CLIP_PLANE0_);
    glDisable(GL_POINT_SPRITE_OES_);
    glDisableClientState(GL_POINT_SIZE_ARRAY_OES_);
    glDisable(GL_FOG_);
    glDisable(GL_LINE_SMOOTH_);
    glDisable(GL_POLYGON_OFFSET_FILL);
    glDisable(GL_LIGHTING_);
    glDisable(GL_LIGHT0_);
    glDisableClientState(GL_NORMAL_ARRAY_);
    glTexEnvi(GL_TEXTURE_ENV_, GL_TEXTURE_ENV_MODE_, GL_MODULATE_);
    glPointSize(1);
}

static int es1_corpus(int argc, char **argv)
{
    const char *out = argv[2];
    unsigned char texels[4 * 4 * 4];
    GLuint tex, rt, fbo, depth;
    char path[512];
    int ok = 0;

    quiet = 1;
    printf("== extensions: %s\n", (const char *)glGetString(GL_EXTENSIONS));
    glGenTextures(1, &rt);
    glBindTexture(GL_TEXTURE_2D, rt);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 64, 64, 0, GL_RGBA, GL_UNSIGNED_BYTE, 0);
    glGenFramebuffersOES(1, &fbo);
    glBindFramebufferOES(GL_FRAMEBUFFER_OES_, fbo);
    glFramebufferTexture2DOES(GL_FRAMEBUFFER_OES_, GL_COLOR_ATTACHMENT0_OES_, GL_TEXTURE_2D, rt, 0);
    glGenRenderbuffersOES(1, &depth);
    glBindRenderbufferOES(GL_RENDERBUFFER_OES_, depth);
    glRenderbufferStorageOES(GL_RENDERBUFFER_OES_, GL_DEPTH_COMPONENT16_OES_, 64, 64);
    glFramebufferRenderbufferOES(GL_FRAMEBUFFER_OES_, GL_DEPTH_ATTACHMENT_OES_,
                                 GL_RENDERBUFFER_OES_, depth);
    glViewport(0, 0, 64, 64);
    glMatrixMode(GL_PROJECTION_);
    glLoadIdentity();
    glMatrixMode(GL_MODELVIEW_);
    glLoadIdentity();
    for (int i = 0; i < 16; i++) {
        texels[i * 4] = (i & 3) * 64;
        texels[i * 4 + 1] = (i >> 2) * 64;
        texels[i * 4 + 2] = 128;
        texels[i * 4 + 3] = 255;
    }
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 4, 4, 0, GL_RGBA, GL_UNSIGNED_BYTE, texels);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    /* the driver's own first-use setup, out of the way */
    for (int k = 0; k < 2; k++) {
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        es1_case(0);
    }
    snprintf(path, sizeof path, "%s/baseline.pages", out);
    printf("== es1: %d pages not zero -> %s\n", changed_pages(path, 1), path);
    new_render_payloads("baseline");
    for (int k = 0; k < NES1; k++) {
        unsigned char px[4] = { 0 };
        int want = argc <= 3;

        for (int i = 3; i < argc; i++)
            want |= !strcmp(argv[i], es1_names[k]);
        if (!want)
            continue;
        printf("== case %s\n", es1_names[k]);
        glClearColor(0, 0, 0, 0);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glFinish();
        changed_pages(0, 0);
        es1_case(k);
        glReadPixels(32, 32, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
        snprintf(path, sizeof path, "%s/%s.pages", out, es1_names[k]);
        printf("   drawn: GL error 0x%x, centre pixel %02x %02x %02x %02x, %d pages -> %s\n",
               glGetError(), px[0], px[1], px[2], px[3], changed_pages(path, 1), path);
        new_render_payloads(es1_names[k]);
        ok++;
    }
    printf("== es1 done: %d cases drawn\n", ok);
    return 0;
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
    int es1 = argc > 2 && !strcmp(argv[1], "es1");
    printf("== hooks installed; creating GLES%d context\n", es1 ? 1 : 2);

    id ctx = [[EAGLContext alloc] initWithAPI:es1 ? kEAGLRenderingAPIOpenGLES1 :
                                                    kEAGLRenderingAPIOpenGLES2];
    if (!ctx || ![EAGLContext setCurrentContext:ctx]) {
        printf("no GLES context -- headless EAGL may be denied on this build\n");
        return 1;
    }
    printf("== context ok, renderer \"%s\"\n", (const char *)glGetString(GL_RENDERER));
    if (es1)
        return es1_corpus(argc, argv);

    GLuint tex, fbo;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 64, 64, 0, GL_RGBA, GL_UNSIGNED_BYTE, 0);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);

    if (argc > 2 && !strcmp(argv[1], "corpus")) {
        corpus_target_tex = tex;
        return corpus(argc, argv);
    }

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
    if (argc > 1 && !strcmp(argv[1], "mod")) {
        /* texture x vertex colour (SDL's colour/alpha mod) in three blend
         * modes -- BLEND, ADD, MOD -- each a frame with two quads and two
         * textures; one interleaved array, 32 bytes a vertex: u v x y r g b a */
        const char *vsm = "attribute vec4 p; attribute vec2 a; attribute vec4 c;"
                          "varying mediump vec2 t; varying lowp vec4 v;"
                          "void main(){ gl_Position = p; t = a; v = c; }";
        const char *fsm = "precision mediump float; varying vec2 t; varying lowp vec4 v;"
                          "uniform sampler2D uTex;"
                          "void main(){ gl_FragColor = texture2D(uTex, t) * v; }";
        GLuint pm = glCreateProgram();
        glAttachShader(pm, make_shader(GL_VERTEX_SHADER, vsm));
        glAttachShader(pm, make_shader(GL_FRAGMENT_SHADER, fsm));
        glBindAttribLocation(pm, 0, "p");
        glBindAttribLocation(pm, 1, "a");
        glBindAttribLocation(pm, 2, "c");
        glLinkProgram(pm);
        glUseProgram(pm);
        glUniform1i(glGetUniformLocation(pm, "uTex"), 0);
        static float vb[12 * 8];
        static const float box[2][4] = { { -0.9f, -0.9f, 0.1f, 0.1f }, { -0.1f, -0.1f, 0.9f, 0.9f } };
        static const float col[2][4] = { { 1, 0.5f, 0.5f, 1 }, { 0.5f, 1, 1, 0.5f } };
        static const int corner[6][2] = { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 0 }, { 1, 1 }, { 0, 1 } };
        for (int q = 0; q < 2; q++)
            for (int k = 0; k < 6; k++) {
                float *o = vb + (q * 6 + k) * 8;
                o[0] = corner[k][0]; o[1] = corner[k][1];
                o[2] = corner[k][0] ? box[q][2] : box[q][0];
                o[3] = corner[k][1] ? box[q][3] : box[q][1];
                for (int c = 0; c < 4; c++) o[4 + c] = col[q][c];
            }
        unsigned char t1[8 * 8 * 4], t2[4 * 4 * 4];
        for (int i = 0; i < 64; i++) {
            int c = ((i & 7) ^ (i >> 3)) & 1;
            t1[i * 4] = c ? 255 : 0; t1[i * 4 + 1] = c ? 255 : 0; t1[i * 4 + 2] = 255;
            t1[i * 4 + 3] = 255;
        }
        for (int i = 0; i < 16; i++) {
            t2[i * 4] = 255; t2[i * 4 + 1] = 255; t2[i * 4 + 2] = 0; t2[i * 4 + 3] = 200;
        }
        GLuint tx[2];
        glGenTextures(2, tx);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, tx[0]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 8, 8, 0, GL_RGBA, GL_UNSIGNED_BYTE, t1);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glBindTexture(GL_TEXTURE_2D, tx[1]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 4, 4, 0, GL_RGBA, GL_UNSIGNED_BYTE, t2);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glEnableVertexAttribArray(0);
        glEnableVertexAttribArray(1);
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 32, vb);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 32, vb + 2);
        glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, 32, vb + 4);
        glEnable(GL_BLEND);
        static const GLenum bf[3][2] = { { GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA },
                                         { GL_SRC_ALPHA, GL_ONE }, { GL_DST_COLOR, GL_ZERO } };
        static const char *names[3] = { "blend", "add", "mod" };
        print_renders = 1;
        for (int m = 0; m < 3; m++) {
            char label[32], path[64];
            glBlendFunc(bf[m][0], bf[m][1]);
            glClearColor(0.3f, 0.3f, 0.3f, 1);
            glClear(GL_COLOR_BUFFER_BIT);
            glBindTexture(GL_TEXTURE_2D, tx[0]);
            glDrawArrays(GL_TRIANGLES, 0, 6);
            glBindTexture(GL_TEXTURE_2D, tx[1]);
            glDrawArrays(GL_TRIANGLES, 6, 6);
            glFinish();
            snprintf(label, sizeof(label), "mod_%s", names[m]);
            snprintf(path, sizeof(path), "/var/root/gt_m_%s.bin", names[m]);
            scan_payloads(label);
            dump_iokit_to(path);
            unsigned char a[4] = { 0 }, b[4] = { 0 };
            glReadPixels(16, 16, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, a);
            glReadPixels(48, 48, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, b);
            printf("== %s: pixel (16,16) %02x %02x %02x %02x, (48,48) %02x %02x %02x %02x\n",
                   label, a[0], a[1], a[2], a[3], b[0], b[1], b[2], b[3]);
        }
        return 0;
    }
    if (argc > 1 && !strcmp(argv[1], "twotex")) {
        /* two quads, two textures, blending on: how one frame carries several
         * draws with their own sampler state (sprites) */
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
        glUniform1i(glGetUniformLocation(pt, "uTex"), 0);
        static const float q1[] = { -0.9f, -0.9f, 0.1f, -0.9f, 0.1f, 0.1f,
                                    -0.9f, -0.9f, 0.1f, 0.1f, -0.9f, 0.1f };
        static const float q2[] = { -0.1f, -0.1f, 0.9f, -0.1f, 0.9f, 0.9f,
                                    -0.1f, -0.1f, 0.9f, 0.9f, -0.1f, 0.9f };
        static const float uv[] = { 0, 0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1 };
        unsigned char t1[8 * 8 * 4], t2[4 * 4 * 4];
        for (int i = 0; i < 64; i++) {	/* 8x8: blue/white checker, opaque */
            int c = ((i & 7) ^ (i >> 3)) & 1;
            t1[i * 4] = c ? 255 : 0; t1[i * 4 + 1] = c ? 255 : 0; t1[i * 4 + 2] = 255;
            t1[i * 4 + 3] = 255;
        }
        for (int i = 0; i < 16; i++) {	/* 4x4: yellow, half transparent */
            t2[i * 4] = 255; t2[i * 4 + 1] = 255; t2[i * 4 + 2] = 0; t2[i * 4 + 3] = 128;
        }
        GLuint tx[2];
        glGenTextures(2, tx);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, tx[0]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 8, 8, 0, GL_RGBA, GL_UNSIGNED_BYTE, t1);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glBindTexture(GL_TEXTURE_2D, tx[1]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 4, 4, 0, GL_RGBA, GL_UNSIGNED_BYTE, t2);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glEnableVertexAttribArray(0);
        glEnableVertexAttribArray(1);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        print_renders = 1;
        glClearColor(0, 0, 0.2f, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        glBindTexture(GL_TEXTURE_2D, tx[0]);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, q1);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 0, uv);
        glDrawArrays(GL_TRIANGLES, 0, 6);
        glBindTexture(GL_TEXTURE_2D, tx[1]);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, q2);
        glDrawArrays(GL_TRIANGLES, 0, 6);
        glFinish();
        scan_payloads("twotex");
        dump_iokit_to("/var/root/gt_2t.bin");
        unsigned char a[4] = { 0 }, b[4] = { 0 };
        glReadPixels(16, 16, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, a);
        glReadPixels(48, 48, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, b);
        printf("== twotex: pixel (16,16) %02x %02x %02x %02x, (48,48) %02x %02x %02x %02x\n",
               a[0], a[1], a[2], a[3], b[0], b[1], b[2], b[3]);
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
