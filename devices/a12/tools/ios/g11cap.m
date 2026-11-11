// Off-screen G11 Metal render reference for iPad 8 (T8020/21B91).
// Captures bounded user-space AGX allocations while retaining the Metal job.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <mach/mach.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

enum { WIDTH = 300, HEIGHT = 200, CHUNK = 65536, MAX_HITS = 64 };
static const char *shader_source =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "vertex float4 vmain(const device float2 *v [[buffer(0)]],"
    " uint id [[vertex_id]]) { return float4(v[id], 0, 1); }\n"
    "fragment float4 fmain() { return float4(0, 1, 0, 1); }\n";

static void write_hit(const char *dir, unsigned n, uint64_t address,
                      const uint8_t *data, size_t length)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/hit-%03u-%016llx.bin", dir, n,
             (unsigned long long)address);
    FILE *f = fopen(path, "wb");
    if (f) {
        fwrite(data, 1, length, f);
        fclose(f);
    }
}

// VM tag 100 is AGX on the A13 reference. Record all regions, but by default
// read only tag-100 regions, in bounded chunks. The tag can be overridden.
static int scan(const char *dir, uint64_t vertex_va, unsigned wanted_tag)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/regions.txt", dir);
    FILE *regions = fopen(path, "w");
    snprintf(path, sizeof(path), "%s/hits.txt", dir);
    FILE *hits = fopen(path, "w");
    uint8_t *buf = malloc(CHUNK);
    if (!regions || !hits || !buf) {
        perror("capture files/buffer");
        if (regions) fclose(regions);
        if (hits) fclose(hits);
        free(buf);
        return -1;
    }
    const uint64_t marker[2] = { 0x4a31373147465831ULL, 0x504d545241434531ULL };
    unsigned count = 0;
    size_t scanned = 0;
    vm_address_t cursor = 0;
    while (count < MAX_HITS && scanned < (128u << 20)) {
        vm_address_t addr = cursor;
        vm_size_t size = 0;
        natural_t depth = 1;
        vm_region_submap_info_data_64_t info;
        mach_msg_type_number_t n = VM_REGION_SUBMAP_INFO_COUNT_64;
        kern_return_t kr = vm_region_recurse_64(mach_task_self(), &addr, &size,
                          &depth, (vm_region_recurse_info_t)&info, &n);
        if (kr != KERN_SUCCESS || !size || addr + size <= cursor) break;
        cursor = addr + size;
        if (info.is_submap) continue;
        fprintf(regions, "%016llx-%016llx prot=%d tag=%u\n",
                (unsigned long long)addr, (unsigned long long)cursor,
                info.protection, info.user_tag);
        if (!(info.protection & VM_PROT_READ) || info.user_tag != wanted_tag)
            continue;
        for (vm_size_t off = 0; off < size && scanned < (128u << 20) &&
             count < MAX_HITS; off += CHUNK) {
            vm_size_t want = size - off < CHUNK ? size - off : CHUNK;
            vm_size_t got = 0;
            if (vm_read_overwrite(mach_task_self(), addr + off, want,
                                  (vm_address_t)buf, &got) != KERN_SUCCESS)
                continue;
            scanned += got;
            for (size_t p = 0; p + 8 <= got && count < MAX_HITS; p += 4) {
                const char *kind = NULL;
                if (!memcmp(buf + p, &vertex_va, 8)) kind = "vertex-gpu-va";
                else if (p + sizeof(marker) <= got &&
                         !memcmp(buf + p, marker, sizeof(marker))) kind = "marker";
                if (!kind) continue;
                uint64_t found = addr + off + p;
                size_t lo = p > 256 ? p - 256 : 0;
                size_t hi = p + 768 < got ? p + 768 : got;
                fprintf(hits, "%s VA=%016llx region=%016llx tag=%u\n",
                        kind, (unsigned long long)found,
                        (unsigned long long)addr, info.user_tag);
                write_hit(dir, count++, addr + off + lo, buf + lo, hi - lo);
            }
        }
    }
    fprintf(hits, "scanned=%zu bytes hits=%u tag=%u\n", scanned, count, wanted_tag);
    fclose(regions);
    fclose(hits);
    free(buf);
    printf("scan: %zu bytes, %u hits, tag %u\n", scanned, count, wanted_tag);
    return 0;
}

int main(int argc, char **argv)
{
    @autoreleasepool {
        const char *dir = argc > 1 ? argv[1] : "/var/mobile/g11cap";
        unsigned tag = argc > 2 ? (unsigned)strtoul(argv[2], NULL, 0) : 100;
        unsigned hold = argc > 3 ? (unsigned)strtoul(argv[3], NULL, 0) : 60;
        if (mkdir(dir, 0755) && access(dir, W_OK)) { perror(dir); return 1; }
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (!device) { fprintf(stderr, "no Metal device\n"); return 1; }
        NSError *error = nil;
        NSString *source = [NSString stringWithUTF8String:shader_source];
        id<MTLLibrary> library = [device newLibraryWithSource:source options:nil error:&error];
        if (!library) { fprintf(stderr, "shader: %s\n", error.description.UTF8String); return 1; }
        MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
        pd.vertexFunction = [library newFunctionWithName:@"vmain"];
        pd.fragmentFunction = [library newFunctionWithName:@"fmain"];
        pd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
        id<MTLRenderPipelineState> pipeline = [device newRenderPipelineStateWithDescriptor:pd error:&error];
        if (!pipeline) { fprintf(stderr, "pipeline: %s\n", error.description.UTF8String); return 1; }

        // Keep a distinctive marker in an AGX buffer; the shader reads the
        // three float2 vertices starting after it.
        struct { uint64_t marker[2]; float xy[6]; } input = {
            { 0x4a31373147465831ULL, 0x504d545241434531ULL },
            { -0.5f, -0.5f, 0.5f, -0.5f, 0.0f, 0.5f }
        };
        id<MTLBuffer> vertices = [device newBufferWithBytes:&input length:sizeof(input)
                                                   options:MTLResourceStorageModeShared];
        MTLTextureDescriptor *td = [MTLTextureDescriptor
            texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                        width:WIDTH height:HEIGHT mipmapped:NO];
        td.usage = MTLTextureUsageRenderTarget;
        td.storageMode = MTLStorageModeShared;
        id<MTLTexture> target = [device newTextureWithDescriptor:td];
        id<MTLCommandQueue> queue = [device newCommandQueue];
        if (!vertices || !target || !queue) { fprintf(stderr, "allocation failed\n"); return 1; }
        MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
        pass.colorAttachments[0].texture = target;
        pass.colorAttachments[0].loadAction = MTLLoadActionClear;
        pass.colorAttachments[0].storeAction = MTLStoreActionStore;
        pass.colorAttachments[0].clearColor = MTLClearColorMake(1, 0, 0, 1);
        id<MTLCommandBuffer> cb = [queue commandBuffer];
        id<MTLRenderCommandEncoder> encoder = [cb renderCommandEncoderWithDescriptor:pass];
        if (!cb || !encoder) { fprintf(stderr, "encoder failed\n"); return 1; }
        [encoder setRenderPipelineState:pipeline];
        [encoder setVertexBuffer:vertices offset:sizeof(input.marker) atIndex:0];
        [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        [encoder endEncoding];
        [cb commit];
        [cb waitUntilCompleted];
        if (cb.status != MTLCommandBufferStatusCompleted || cb.error) {
            fprintf(stderr, "render: %s\n", cb.error.description.UTF8String);
            return 1;
        }
        uint8_t *pixels = calloc(WIDTH * HEIGHT, 4);
        if (!pixels) return 1;
        [target getBytes:pixels bytesPerRow:WIDTH * 4
             fromRegion:MTLRegionMake2D(0, 0, WIDTH, HEIGHT) mipmapLevel:0];
        const uint8_t *corner = pixels;
        const uint8_t *center = pixels + ((HEIGHT / 2) * WIDTH + WIDTH / 2) * 4;
        int ok = corner[0] == 255 && corner[1] == 0 && corner[2] == 0 && corner[3] == 255 &&
                 center[0] == 0 && center[1] == 255 && center[2] == 0 && center[3] == 255;
        printf("G11 offscreen %ux%u corner=%u,%u,%u,%u center=%u,%u,%u,%u %s\n",
               WIDTH, HEIGHT, corner[0], corner[1], corner[2], corner[3],
               center[0], center[1], center[2], center[3], ok ? "PASS" : "FAIL");
        printf("vertex buffer GPU VA=%#llx; pid=%d\n",
               (unsigned long long)vertices.gpuAddress, getpid());
        free(pixels);
        if (!ok) return 2;
        int rc = scan(dir, vertices.gpuAddress, tag);
        printf("holding Metal objects for %u seconds (pid %d)\n", hold, getpid());
        fflush(stdout);
        sleep(hold);
        return rc ? 3 : 0;
    }
}
