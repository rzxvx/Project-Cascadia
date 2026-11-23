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

int main(int argc, char **argv)
{
	@autoreleasepool {
		if (argc < 2) {
			fprintf(stderr, "usage: gpucap OUTDIR [NTHREADS]\n");
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
