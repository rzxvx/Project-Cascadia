// g12cap: a reference G12 compute submission from iOS's own Metal driver
// (Cascadia a13/docs/research/gpu.md, G5.1).
//
// Runs the G5 kernel (out[tid] = 0xcafe0000 + tid) through Metal with an
// unusual grid (77 threadgroups of 13 threads = 1001 threads), checks the
// result, then searches its own address space for what the user-mode
// driver (AGXMetalG12) wrote with the CPU: the CDM control stream (grid
// and threadgroup sizes as three u32 each), the USC words and the shader
// code. Every hit is dumped with its surroundings, and each region that
// holds one is saved whole, into OUTDIR (default /var/mobile/g12cap).
//
//   g12cap [OUTDIR] [REPEAT] [all]
// "all": also save every region the GPU driver maps (VM tag 100) whole.
// G12CAP_SLEEP=SECS: stay alive that long after the scan (the GPU context
// and the kernel's work command with it, for kmem).
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <mach/mach.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

static NSString *const kSource =
    @"#include <metal_stdlib>\n"
     "using namespace metal;\n"
     "kernel void store_const(device uint *out [[buffer(0)]],\n"
     "                        uint tid [[thread_position_in_grid]]) {\n"
     "    if (tid < 1001) out[tid] = 0xcafe0000u + tid;\n"
     "}\n";

enum { GROUPS = 77, TG = 13, THREADS = GROUPS * TG };

struct pattern {
    const char *name;
    uint8_t bytes[32];
    size_t len;
};

static struct pattern pats[16];
static int npats;
static int save_all;

static void add_u32s(const char *name, const uint32_t *v, int n)
{
    struct pattern *p = &pats[npats++];
    p->name = name;
    memcpy(p->bytes, v, n * 4);
    p->len = n * 4;
}

static void add_bytes(const char *name, const char *hex)
{
    struct pattern *p = &pats[npats++];
    p->name = name;
    p->len = 0;
    for (; hex[0] && hex[1]; hex += 2) {
        unsigned b;
        sscanf(hex, "%2x", &b);
        p->bytes[p->len++] = b;
    }
}

static void hexdump(FILE *f, uint64_t addr, const uint8_t *d, size_t n)
{
    for (size_t i = 0; i < n; i += 16) {
        fprintf(f, "%016lx ", (unsigned long)(addr + i));
        for (size_t j = 0; j < 16 && i + j < n; j++)
            fprintf(f, " %02x", d[i + j]);
        fprintf(f, "\n");
    }
}

extern const void *_dyld_get_shared_cache_range(size_t *length);

struct region {
    vm_address_t addr;
    vm_size_t size;
    vm_region_submap_info_data_64_t info;
};

#define MAXREG 8192
#define SCRATCH (64ull << 20)

static int scan(const char *outdir, vm_address_t skip_lo, vm_address_t skip_hi)
{
    char path[512];
    static struct region reg[MAXREG];
    int nreg = 0;

    // list the regions first: the scratch mapping and the files' buffers
    // must not be scanned themselves
    for (vm_address_t addr = 0; nreg < MAXREG;) {
        vm_size_t size = 0;
        natural_t depth = 1;
        vm_region_submap_info_data_64_t info;
        mach_msg_type_number_t cnt = VM_REGION_SUBMAP_INFO_COUNT_64;
        if (vm_region_recurse_64(mach_task_self(), &addr, &size, &depth,
                                   (vm_region_recurse_info_t)&info, &cnt) != KERN_SUCCESS)
            break;
        if (!info.is_submap) {
            reg[nreg].addr = addr;
            reg[nreg].size = size;
            reg[nreg].info = info;
            nreg++;
        }
        addr += size;
    }

    size_t sc_len = 0;
    vm_address_t sc_lo = (vm_address_t)_dyld_get_shared_cache_range(&sc_len);
    vm_address_t sc_hi = sc_lo + sc_len;

    uint8_t *buf = mmap(NULL, SCRATCH, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (buf == MAP_FAILED) {
        perror("mmap");
        return 1;
    }

    snprintf(path, sizeof(path), "%s/hits.txt", outdir);
    FILE *hits = fopen(path, "w");
    snprintf(path, sizeof(path), "%s/regions.txt", outdir);
    FILE *regs = fopen(path, "w");
    if (!hits || !regs) {
        perror(path);
        return 1;
    }
    fprintf(regs, "shared cache %016lx-%016lx, image %016lx-%016lx\n",
            sc_lo, sc_hi, skip_lo, skip_hi);

    int nhits = 0, nsaved = 0;
    for (int r = 0; r < nreg; r++) {
        vm_address_t addr = reg[r].addr;
        vm_size_t size = reg[r].size;
        vm_region_submap_info_data_64_t *info = &reg[r].info;
        fprintf(regs, "%016lx-%016lx prot %d/%d tag %u share %d\n",
                addr, addr + size, info->protection, info->max_protection,
                info->user_tag, info->share_mode);
        int skip = !(info->protection & VM_PROT_READ) ||
                   (addr >= skip_lo && addr < skip_hi) ||
                   (addr >= sc_lo && addr < sc_hi) || size > SCRATCH;
        if (skip)
            continue;
        vm_size_t got = 0;
        if (vm_read_overwrite(mach_task_self(), addr, size,
                                   (vm_address_t)buf, &got) != KERN_SUCCESS)
            continue;
        int region_hit = 0;
        for (int p = 0; p < npats; p++) {
            for (size_t o = 0; o + pats[p].len <= got; o += 4) {
                if (memcmp(buf + o, pats[p].bytes, pats[p].len))
                    continue;
                nhits++;
                region_hit = 1;
                fprintf(hits, "== %s at %016lx (region %016lx+%lx, tag %u)\n",
                        pats[p].name, addr + o, addr, (unsigned long)o,
                        info->user_tag);
                size_t lo = o >= 0x100 ? o - 0x100 : 0;
                size_t hi = o + 0x200 < got ? o + 0x200 : got;
                hexdump(hits, addr + lo, buf + lo, hi - lo);
            }
        }
        if (region_hit || (save_all && info->user_tag == 100)) {
            snprintf(path, sizeof(path), "%s/region-%016lx.bin", outdir, addr);
            FILE *f = fopen(path, "wb");
            if (f) {
                fwrite(buf, 1, got, f);
                fclose(f);
                nsaved++;
            }
        }
    }
    fclose(hits);
    fclose(regs);
    printf("scan: %d regions, %d hits, %d regions saved in %s\n", nreg, nhits, nsaved, outdir);
    return 0;
}

int main(int argc, char **argv)
{
    @autoreleasepool {
        const char *outdir = argc > 1 ? argv[1] : "/var/mobile/g12cap";
        int repeat = argc > 2 ? atoi(argv[2]) : 1;
        save_all = argc > 3 && !strcmp(argv[3], "all");
        mkdir(outdir, 0755);

        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        if (!dev) {
            fprintf(stderr, "no Metal device\n");
            return 1;
        }
        printf("device: %s\n", dev.name.UTF8String);

        NSError *err = nil;
        id<MTLLibrary> lib = [dev newLibraryWithSource:kSource options:nil error:&err];
        if (!lib) {
            fprintf(stderr, "compile: %s\n", err.description.UTF8String);
            return 1;
        }
        id<MTLComputePipelineState> pso =
            [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"store_const"]
                                               error:&err];
        if (!pso) {
            fprintf(stderr, "pipeline: %s\n", err.description.UTF8String);
            return 1;
        }
        id<MTLBuffer> out = [dev newBufferWithLength:4096 * 4 options:MTLResourceStorageModeShared];
        memset(out.contents, 0, 4096 * 4);
        printf("out buffer: cpu %p gpu %#llx\n", out.contents, (unsigned long long)out.gpuAddress);

        id<MTLCommandQueue> q = [dev newCommandQueue];
        for (int r = 0; r < repeat; r++) {
            id<MTLCommandBuffer> cb = [q commandBuffer];
            id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
            [enc setComputePipelineState:pso];
            [enc setBuffer:out offset:0 atIndex:0];
            [enc dispatchThreadgroups:MTLSizeMake(GROUPS, 1, 1)
                threadsPerThreadgroup:MTLSizeMake(TG, 1, 1)];
            [enc endEncoding];
            [cb commit];
            [cb waitUntilCompleted];
            if (cb.error)
                fprintf(stderr, "command buffer: %s\n", cb.error.description.UTF8String);
        }

        uint32_t *o = out.contents;
        int bad = 0;
        for (int i = 0; i < THREADS; i++)
            if (o[i] != 0xcafe0000u + i)
                bad++;
        printf("result: %d of %d words wrong (out[0] %#x, out[1000] %#x)\n",
               bad, THREADS, o[0], o[1000]);

        uint32_t global_threads[3] = {THREADS, 1, 1};
        uint32_t global_groups[3] = {GROUPS, 1, 1};
        uint32_t local[3] = {TG, 1, 1};
        add_u32s("CDM global size (threads)", global_threads, 3);
        add_u32s("CDM global size (groups)", global_groups, 3);
        add_u32s("CDM local size", local, 3);
        // the offline G12 compile of the same kernel (metal-isa.sh) starts
        // with get_sr r?, thread_position_in_grid
        add_bytes("G12 shader get_sr", "f2051004");
        // the out buffer's GPU address, as the USC / uniform words would
        // carry it
        uint64_t gva = out.gpuAddress;
        struct pattern *p = &pats[npats++];
        p->name = "out buffer gpuAddress";
        memcpy(p->bytes, &gva, 8);
        p->len = 8;

        // skip our own __TEXT/__DATA (the patterns are constants there)
        extern const struct mach_header_64 _mh_execute_header;
        vm_address_t lo = (vm_address_t)&_mh_execute_header;
        int ret = scan(outdir, lo, lo + 0x100000);
        const char *sl = getenv("G12CAP_SLEEP");
        if (sl) {
            printf("sleeping %s s (pid %d)\n", sl, getpid());
            fflush(stdout);
            sleep(atoi(sl));
        }
        return ret;
    }
}
