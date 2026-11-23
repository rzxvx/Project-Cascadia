// SPDX-License-Identifier: GPL-2.0
/*
 * gpucap: G5 ground truth from iPadOS (devices/a10/docs/gpu.md, "G5").
 * Runs our own compute kernels through Metal on the jailbroken iPad and
 * saves this process's GPU memory (the IOAccelerator-tagged regions, VM tag
 * 100) so the Mac can find what Apple's stack puts around the USC code: the
 * PDS program and data, the USC base, the CDM kernel records.
 *
 *   gpucap OUTDIR [NTHREADS]
 *
 * The kernels are the same source as devices/a13/tools/gpu/isa/g5-kernels.metal
 * (so the USC code can be compared byte for byte with metal-tt's output).
 * store_const runs into buffer A (filled with 0xabadbeef first), then into
 * buffer B in a second command buffer: the two dispatches differ only in
 * the binding. Snapshots: OUTDIR/{enc,sched,done}/<cpu addr>-<size>.bin plus
 * index.txt (cpu address, size, protection, tag); OUTDIR/info.txt has the
 * buffers' CPU pointers and GPU addresses.
 */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <mach/mach.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

static NSString *const kSource =
    @"#include <metal_stdlib>\n"
     "using namespace metal;\n"
     "kernel void store_const(device uint *out [[buffer(0)]], uint tid [[thread_position_in_grid]]) {\n"
     "    out[tid] = 0xcafe0000u + tid;\n"
     "}\n"
     "kernel void empty() {}\n";

static void snapshot(const char *dir)
{
	char path[1024];
	mkdir(dir, 0755);
	snprintf(path, sizeof(path), "%s/index.txt", dir);
	FILE *idx = fopen(path, "w");
	vm_address_t addr = 0;
	unsigned total = 0;

	for (;;) {
		vm_size_t size = 0;
		natural_t depth = 1;
		struct vm_region_submap_info_64 info;
		mach_msg_type_number_t count = VM_REGION_SUBMAP_INFO_COUNT_64;

		if (vm_region_recurse_64(mach_task_self(), &addr, &size, &depth,
					 (vm_region_recurse_info_t)&info, &count) != KERN_SUCCESS)
			break;
		if (info.user_tag == 100 && (info.protection & VM_PROT_READ) && size <= (64u << 20)) {
			snprintf(path, sizeof(path), "%s/%lx-%lx.bin", dir, (unsigned long)addr, (unsigned long)size);
			FILE *f = fopen(path, "wb");
			if (f) {
				fwrite((void *)addr, 1, size, f);
				fclose(f);
			}
			fprintf(idx, "%lx %lx prot=%d tag=%u\n", (unsigned long)addr, (unsigned long)size, info.protection, info.user_tag);
			total++;
		}
		addr += size;
	}
	fclose(idx);
	printf("%s: %u regions\n", dir, total);
}

/* G6: a render pass into a linear 64x64 BGRA8 texture that lives in a shared
 * buffer (so the CPU reads the pixels): "clear" = no draws, the load action
 * clears to (0.25, 0.5, 0.75, 1) -> BGRA bytes 0xbf 0x80 0x40 0xff; "tri" =
 * the same plus one constant-colour (1, 0, 0, 1) triangle over the left half. */
static NSString *const kRenderSource =
    @"#include <metal_stdlib>\n"
     "using namespace metal;\n"
     "vertex float4 tri_vs(uint vid [[vertex_id]]) {\n"
     "    float2 p[3] = { float2(-1, -1), float2(0, -1), float2(-1, 1) };\n"
     "    return float4(p[vid], 0, 1);\n"
     "}\n"
     "fragment float4 tri_fs() { return float4(1, 0, 0, 1); }\n";

static int render(id<MTLDevice> dev, const char *out, int tri)
{
	char dir[1024];
	NSError *err = nil;
	const NSUInteger w = 64, h = 64, stride = 256;
	id<MTLBuffer> fb = [dev newBufferWithLength:stride * h options:MTLResourceStorageModeShared];
	for (unsigned i = 0; i < stride * h / 4; i++)
		((uint32_t *)fb.contents)[i] = 0xabadbeef;
	MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
		width:w height:h mipmapped:NO];
	td.storageMode = MTLStorageModeShared;
	td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
	id<MTLTexture> rt = [fb newTextureWithDescriptor:td offset:0 bytesPerRow:stride];
	if (!rt) {
		fprintf(stderr, "no linear render target\n");
		return 1;
	}
	id<MTLRenderPipelineState> rps = nil;
	if (tri) {
		id<MTLLibrary> lib = [dev newLibraryWithSource:kRenderSource options:nil error:&err];
		if (!lib) {
			fprintf(stderr, "compile: %s\n", err.description.UTF8String);
			return 1;
		}
		MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
		pd.vertexFunction = [lib newFunctionWithName:@"tri_vs"];
		pd.fragmentFunction = [lib newFunctionWithName:@"tri_fs"];
		pd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
		rps = [dev newRenderPipelineStateWithDescriptor:pd error:&err];
		if (!rps) {
			fprintf(stderr, "pipeline: %s\n", err.description.UTF8String);
			return 1;
		}
	}
	id<MTLCommandQueue> q = [dev newCommandQueue];
	id<MTLCommandBuffer> cb = [q commandBuffer];
	MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
	rp.colorAttachments[0].texture = rt;
	rp.colorAttachments[0].loadAction = MTLLoadActionClear;
	rp.colorAttachments[0].storeAction = MTLStoreActionStore;
	rp.colorAttachments[0].clearColor = MTLClearColorMake(0.25, 0.5, 0.75, 1.0);
	id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:rp];
	if (tri) {
		[e setRenderPipelineState:rps];
		[e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
	}
	[e endEncoding];
	snprintf(dir, sizeof(dir), "%s/enc", out);
	snapshot(dir);
	__block int scheduled = 0;
	[cb addScheduledHandler:^(id<MTLCommandBuffer> c) {
		(void)c;
		char d[1024];
		snprintf(d, sizeof(d), "%s/sched", out);
		snapshot(d);
		scheduled = 1;
	}];
	[cb commit];
	[cb waitUntilCompleted];
	snprintf(dir, sizeof(dir), "%s/done", out);
	snapshot(dir);
	const uint32_t *px = fb.contents;
	snprintf(dir, sizeof(dir), "%s/info.txt", out);
	FILE *f = fopen(dir, "w");
	fprintf(f, "render %s, %lux%lu BGRA8 linear, stride %lu, fb cpu %p, status %ld, scheduled %d\n",
		tri ? "tri" : "clear", (unsigned long)w, (unsigned long)h, (unsigned long)stride,
		fb.contents, (long)cb.status, scheduled);
	fprintf(f, "px(0,0) %08x px(63,0) %08x px(0,63) %08x px(63,63) %08x\n",
		px[0], px[63], px[63 * stride / 4], px[63 * stride / 4 + 63]);
	fclose(f);
	printf("px(0,0) %08x px(63,0) %08x px(0,63) %08x px(63,63) %08x\n",
	       px[0], px[63], px[63 * stride / 4], px[63 * stride / 4 + 63]);
	return 0;
}

int main(int argc, char **argv)
{
	@autoreleasepool {
		if (argc < 2) {
			fprintf(stderr, "usage: gpucap OUTDIR [NTHREADS | clear | tri]\n");
			return 2;
		}
		const char *out = argv[1];
		NSUInteger n = argc > 2 ? strtoul(argv[2], NULL, 0) : 1;
		char dir[1024];
		mkdir(out, 0755);

		id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
		if (!dev) {
			fprintf(stderr, "no Metal device (entitlements?)\n");
			return 1;
		}
		if (argc > 2 && (!strcmp(argv[2], "clear") || !strcmp(argv[2], "tri")))
			return render(dev, out, !strcmp(argv[2], "tri"));
		NSError *err = nil;
		id<MTLLibrary> lib = [dev newLibraryWithSource:kSource options:nil error:&err];
		if (!lib) {
			fprintf(stderr, "compile: %s\n", err.description.UTF8String);
			return 1;
		}
		id<MTLComputePipelineState> ps = [dev newComputePipelineStateWithFunction:
			[lib newFunctionWithName:@"store_const"] error:&err];
		if (!ps) {
			fprintf(stderr, "pipeline: %s\n", err.description.UTF8String);
			return 1;
		}
		id<MTLBuffer> a = [dev newBufferWithLength:0x4000 options:MTLResourceStorageModeShared];
		id<MTLBuffer> b = [dev newBufferWithLength:0x4000 options:MTLResourceStorageModeShared];
		for (unsigned i = 0; i < 0x1000; i++) {
			((uint32_t *)a.contents)[i] = 0xabadbeef;
			((uint32_t *)b.contents)[i] = 0xabadbeef;
		}
		id<MTLCommandQueue> q = [dev newCommandQueue];

		id<MTLCommandBuffer> cb[2];
		for (int k = 0; k < 2; k++) {
			cb[k] = [q commandBuffer];
			id<MTLComputeCommandEncoder> e = [cb[k] computeCommandEncoder];
			[e setComputePipelineState:ps];
			[e setBuffer:(k ? b : a) offset:0 atIndex:0];
			[e dispatchThreadgroups:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(n, 1, 1)];  /* A10: no non-uniform dispatchThreads */
			[e endEncoding];
		}
		snprintf(dir, sizeof(dir), "%s/enc", out);
		snapshot(dir);
		__block int scheduled = 0;
		[cb[0] addScheduledHandler:^(id<MTLCommandBuffer> c) {
			(void)c;
			char d[1024];
			snprintf(d, sizeof(d), "%s/sched", out);
			snapshot(d);
			scheduled = 1;
		}];
		[cb[0] commit];
		[cb[1] commit];
		[cb[1] waitUntilCompleted];
		snprintf(dir, sizeof(dir), "%s/done", out);
		snapshot(dir);

		snprintf(dir, sizeof(dir), "%s/info.txt", out);
		FILE *f = fopen(dir, "w");
		fprintf(f, "device %s\n", dev.name.UTF8String);
		fprintf(f, "threads %lu, pipeline maxThreads %lu, scheduled snapshot %d\n",
			(unsigned long)n, (unsigned long)ps.maxTotalThreadsPerThreadgroup, scheduled);
		uint64_t ga = 0, gb = 0;
		@try {
			ga = a.gpuAddress;
			gb = b.gpuAddress;
		} @catch (NSException *x) {
			(void)x;          /* gpuAddress may be unsupported on A10 */
		}
		fprintf(f, "A cpu %p gpu %#llx: %08x %08x\n", a.contents, ga,
			((uint32_t *)a.contents)[0], ((uint32_t *)a.contents)[1]);
		fprintf(f, "B cpu %p gpu %#llx: %08x %08x\n", b.contents, gb,
			((uint32_t *)b.contents)[0], ((uint32_t *)b.contents)[1]);
		fprintf(f, "status %ld %ld\n", (long)cb[0].status, (long)cb[1].status);
		fclose(f);
		printf("A[0] = %08x, B[0] = %08x\n", ((uint32_t *)a.contents)[0], ((uint32_t *)b.contents)[0]);
	}
	return 0;
}
