// SPDX-License-Identifier: MIT
/*
 * pvr-uapi-test: the powervr UAPI on the iPad 7's GPU (Cascadia G7), from
 * userspace, with nothing but ioctls -- what Mesa's pvr winsys does first:
 * device queries, a VM context, a buffer object mapped to the CPU and into
 * the GPU's VM, the kernel's reserved page refused. Built static for aarch64
 * by tools/gpu/pvr-uapi-test.sh.
 *
 *   ./pvr-uapi-test [/dev/dri/renderD128]
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <drm/drm.h>
#include <drm/pvr_drm.h>

static int fails;

#define CHECK(cond, ...) do { \
	if (cond) { printf("  ok    "); } else { printf("  FAIL  "); fails++; } \
	printf(__VA_ARGS__); printf("\n"); } while (0)

static int query(int fd, uint32_t type, void *p, uint32_t size)
{
	struct drm_pvr_ioctl_dev_query_args q = { .type = type, .size = size, .pointer = (uintptr_t)p };

	return ioctl(fd, DRM_IOCTL_PVR_DEV_QUERY, &q) ? -errno : (int)q.size;
}

/* a BO, CPU-mapped */
static uint32_t *new_bo(int fd, uint64_t size, uint32_t *handle)
{
	struct drm_pvr_ioctl_create_bo_args bo = { .size = size, .flags = DRM_PVR_BO_ALLOW_CPU_USERSPACE_ACCESS };
	struct drm_pvr_ioctl_get_bo_mmap_offset_args off = { 0 };
	void *p;

	if (ioctl(fd, DRM_IOCTL_PVR_CREATE_BO, &bo))
		return NULL;
	off.handle = bo.handle;
	if (ioctl(fd, DRM_IOCTL_PVR_GET_BO_MMAP_OFFSET, &off))
		return NULL;
	p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, off.offset);
	*handle = bo.handle;
	return p == MAP_FAILED ? NULL : p;
}

static int vm_map(int fd, uint32_t vm, uint32_t bo, uint64_t va, uint64_t size)
{
	struct drm_pvr_ioctl_vm_map_args m = { .vm_context_handle = vm, .handle = bo,
					       .device_addr = va, .size = size };

	return ioctl(fd, DRM_IOCTL_PVR_VM_MAP, &m);
}

/* store_const's USC code from the self-test's firmware file (G9PT: name[24], off, size) */
static int load_store_const(uint8_t *dst, uint32_t *size)
{
	static uint8_t buf[4096];
	FILE *f = fopen("/lib/firmware/apple/g9p-selftest.bin", "rb");
	uint32_t n, i;
	size_t len;

	if (!f)
		return -1;
	len = fread(buf, 1, sizeof(buf), f);
	fclose(f);
	if (len < 8 || memcmp(buf, "G9PT", 4))
		return -1;
	memcpy(&n, buf + 4, 4);
	for (i = 0; i < n && 8 + 32 * (i + 1) <= len; i++) {
		uint32_t off, sz;

		memcpy(&off, buf + 8 + 32 * i + 24, 4);
		memcpy(&sz, buf + 8 + 32 * i + 28, 4);
		if (!strncmp((char *)buf + 8 + 32 * i, "store_const", 24) && off + sz <= len) {
			memcpy(dst, buf + off, sz);
			*size = sz;
			return 0;
		}
	}
	return -1;
}

/*
 * G5's store through the UAPI: a compute context, jobs whose CDM stream runs
 * a DMA task (the buffer address into the common store) and store_const.
 * Code BO at the PDS and the USC heap bases (offsets as G5 / the self-test);
 * data BO in the general heap: streams +0x100 i, argument entries +0x1000 +
 * 0x20 i, outputs +0x2000 + 0x40 i, the CDM context state +0x8000.
 */
#define CODE_HALT	0x2000
#define CODE_DMA_PDS	0x2100
#define CODE_KER_PDS	0x2200
#define CODE_KER_DATA	0x2280
#define CODE_USC	0x2400
#define CODE_DMA_DATA	0x3000

static void compute(int fd, uint32_t vm, const struct drm_pvr_heap *heaps)
{
	static const uint32_t dma_pds[] = { 0xf0001000, 0xf4001808, 0xd3000000 };
	static const uint32_t ker_pds[] = { 0xc2c00003, 0xf0000a01, 0xd3000000, 0xf0000e01, 0xfc000002, 0xd3000000 };
	struct drm_pvr_ioctl_create_context_args ctx = { 0 };
	uint64_t pds = heaps[DRM_PVR_HEAP_PDS_CODE_DATA].base, usc = heaps[DRM_PVR_HEAP_USC_CODE].base;
	uint64_t data_va = heaps[DRM_PVR_HEAP_GENERAL].base + 0x200000;
	uint64_t pds_halt = ((uint64_t)(CODE_HALT + 0x10) << 32) | CODE_HALT;
	uint64_t state[1 + 7] = { 0 };	/* the u32 length + u32 0, then 7 x u64 */
	uint32_t code_h, data_h, usc_size = 0, *code, *data, i, k, ok = 0;
	struct timespec t0, t1;
	int r;

	code = new_bo(fd, 0x10000, &code_h);
	data = new_bo(fd, 0x10000, &data_h);
	CHECK(code && data, "compute: code and data BOs");
	if (!code || !data)
		return;
	r = vm_map(fd, vm, code_h, pds, 0x10000) || vm_map(fd, vm, code_h, usc, 0x10000) ||
	    vm_map(fd, vm, data_h, data_va, 0x10000);
	CHECK(!r, "compute: code at %#llx / %#llx, data at %#llx", (unsigned long long)pds,
	      (unsigned long long)usc, (unsigned long long)data_va);
	r = load_store_const((uint8_t *)code + CODE_USC, &usc_size);
	CHECK(!r, "compute: store_const, %u bytes", usc_size);
	if (r)
		return;
	for (i = 0; i < 0x10000 / 4; i++)
		code[i] = i < CODE_USC / 4 ? 0xd3000000 : code[i];
	memset(code + CODE_HALT / 4 + 1, 0, 0x1c);
	memcpy(code + CODE_DMA_PDS / 4, dma_pds, sizeof(dma_pds));
	memcpy(code + CODE_KER_PDS / 4, ker_pds, sizeof(ker_pds));
	code[CODE_KER_DATA / 4 + 0] = CODE_USC;		/* DOUTU: EXE_OFF, TEMPS 2 */
	code[CODE_KER_DATA / 4 + 1] = 0x10;
	code[CODE_KER_DATA / 4 + 2] = 0x90000000;
	code[CODE_KER_DATA / 4 + 3] = 0x80000000;

	/* static context state: context PDS0, PDS1, terminate PDS, terminate PDS1, 3 resume */
	state[0] = sizeof(state);	/* u32 length, u32 0 */
	state[1] = pds_halt;		/* cdmreg_cdm_context_pds0 */
	state[3] = pds_halt;		/* cdmreg_cdm_terminate_pds */
	ctx.type = DRM_PVR_CTX_TYPE_COMPUTE;
	ctx.vm_context_handle = vm;
	ctx.static_context_state = (uintptr_t)state;
	ctx.static_context_state_len = sizeof(state);
	r = ioctl(fd, DRM_IOCTL_PVR_CREATE_CONTEXT, &ctx);
	CHECK(!r, "CREATE_CONTEXT (compute): handle %u%s%s", ctx.handle, r ? " -- " : "", r ? strerror(errno) : "");
	if (r)
		return;

	for (int batch = 0; batch < 2; batch++) {
		uint32_t n = batch ? 16 : 100, inflight = batch ? 16 : 1;

		ok = 0;
		clock_gettime(CLOCK_MONOTONIC, &t0);
		for (k = 0; k < n; k += inflight) {
			struct drm_pvr_job jobs[16];
			struct drm_pvr_sync_op ops[16];
			struct drm_pvr_ioctl_submit_jobs_args sub = { 0 };
			struct drm_syncobj_create sc = { 0 };
			struct drm_syncobj_wait sw = { 0 };
			uint64_t cmds[16][5];

			if (ioctl(fd, DRM_IOCTL_SYNCOBJ_CREATE, &sc))
				break;
			for (i = 0; i < inflight; i++) {
				uint32_t s = i, *st = data + 0x100 / 4 * s;
				uint64_t arg = data_va + 0x1000 + 0x20 * s, out = data_va + 0x2000 + 0x40 * s;
				const uint32_t rec[] = {
					0x4, (CODE_DMA_DATA + 0x40 * s) | 0x2, CODE_DMA_PDS, 0, 0, 0, 0x40000001, 0,
					0x2103, CODE_KER_DATA, CODE_KER_PDS, 0, 0, 0, 0x40000001, 0,
					0x80000000,
				};
				uint64_t dd[4] = { arg, arg + 0x10, 0x10000002, 0x90004003 };
				uint64_t ae[3] = { out, 0, 0x00010001 };

				memcpy(st, rec, sizeof(rec));
				memcpy((uint8_t *)code + CODE_DMA_DATA + 0x40 * s, dd, sizeof(dd));
				memcpy((uint8_t *)data + 0x1000 + 0x20 * s, ae, 20);
				for (uint32_t w = 0; w < 16; w++)
					data[(0x2000 + 0x40 * s) / 4 + w] = 0xabadbeef;
				/* the command stream: len, pad, tpu border, stream base, state base, resume pds1 */
				cmds[i][0] = 36;			/* u32 length 36, u32 0 */
				cmds[i][1] = 0;				/* tpu_border_colour_table */
				cmds[i][2] = data_va + 0x100 * s;	/* cdm_ctrl_stream_base */
				cmds[i][3] = data_va + 0x8000;		/* cdm_context_state_base_addr */
				cmds[i][4] = 0;				/* cdm_resume_pds1 (u32) */
				memset(&jobs[i], 0, sizeof(jobs[i]));
				jobs[i].type = DRM_PVR_JOB_TYPE_COMPUTE;
				jobs[i].context_handle = ctx.handle;
				jobs[i].cmd_stream = (uintptr_t)cmds[i];
				jobs[i].cmd_stream_len = 36;
				if (i == inflight - 1) {	/* the last job signals the syncobj */
					ops[i] = (struct drm_pvr_sync_op){ .handle = sc.handle,
						.flags = DRM_PVR_SYNC_OP_FLAG_SIGNAL };
					jobs[i].sync_ops = (struct drm_pvr_obj_array){ .stride = sizeof(ops[0]),
						.count = 1, .array = (uintptr_t)&ops[i] };
				}
			}
			sub.jobs = (struct drm_pvr_obj_array){ .stride = sizeof(jobs[0]), .count = inflight,
							       .array = (uintptr_t)jobs };
			r = ioctl(fd, DRM_IOCTL_PVR_SUBMIT_JOBS, &sub);
			if (r) {
				printf("  FAIL  SUBMIT_JOBS: %s\n", strerror(errno));
				fails++;
				break;
			}
			clock_gettime(CLOCK_MONOTONIC, &t1);
			sw.handles = (uintptr_t)&sc.handle;
			sw.count_handles = 1;
			sw.timeout_nsec = t1.tv_sec * 1000000000LL + t1.tv_nsec + 3000000000LL;
			sw.flags = DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT;
			r = ioctl(fd, DRM_IOCTL_SYNCOBJ_WAIT, &sw);
			for (i = 0; i < inflight; i++) {
				uint32_t *o = data + (0x2000 + 0x40 * i) / 4;

				ok += !r && o[0] == 0xcafe0000 && o[1] == 0xabadbeef;
			}
			struct drm_syncobj_destroy sd = { .handle = sc.handle };

			ioctl(fd, DRM_IOCTL_SYNCOBJ_DESTROY, &sd);
			if (r) {
				printf("  FAIL  SYNCOBJ_WAIT: %s\n", strerror(errno));
				fails++;
				break;
			}
		}
		clock_gettime(CLOCK_MONOTONIC, &t1);
		CHECK(ok == n, "compute store x%u (%u in flight): %u wrote cafe0000; %.0f us per job", n, inflight,
		      ok, ((t1.tv_sec - t0.tv_sec) * 1e9 + (t1.tv_nsec - t0.tv_nsec)) / 1e3 / n);
	}
}

static int open_powervr(const char *path)
{
	char name[32];
	int i;

	for (i = 128; i < 136; i++) {
		char p[32];
		struct drm_version v = { .name = name, .name_len = sizeof(name) - 1 };
		int fd;

		if (path)
			snprintf(p, sizeof(p), "%s", path);
		else
			snprintf(p, sizeof(p), "/dev/dri/renderD%d", i);
		fd = open(p, O_RDWR | O_CLOEXEC);
		if (fd < 0) {
			if (path)
				return -1;
			continue;
		}
		memset(name, 0, sizeof(name));
		if (!ioctl(fd, DRM_IOCTL_VERSION, &v) && !strcmp(name, "powervr")) {
			printf("%s: %s\n", p, name);
			return fd;
		}
		close(fd);
		if (path)
			return -1;
	}
	return -1;
}

int main(int argc, char **argv)
{
	struct drm_pvr_dev_query_gpu_info gpu = { 0 };
	struct drm_pvr_dev_query_runtime_info rt = { 0 };
	struct drm_pvr_heap heaps[DRM_PVR_HEAP_COUNT] = { 0 };
	struct drm_pvr_dev_query_heap_info hi = { 0 };
	struct drm_pvr_ioctl_create_vm_context_args vm = { 0 };
	struct drm_pvr_ioctl_create_bo_args bo = { .size = 0x4000, .flags = DRM_PVR_BO_ALLOW_CPU_USERSPACE_ACCESS };
	struct drm_pvr_ioctl_get_bo_mmap_offset_args off = { 0 };
	struct drm_pvr_ioctl_vm_map_args map = { 0 };
	struct drm_pvr_ioctl_vm_unmap_args unmap = { 0 };
	uint64_t gen_base, gen_size;
	uint32_t *cpu;
	int fd, r, i;

	fd = open_powervr(argc > 1 ? argv[1] : NULL);
	if (fd < 0) {
		printf("no powervr render node\n");
		return 1;
	}

	r = query(fd, DRM_PVR_DEV_QUERY_GPU_INFO_GET, &gpu, sizeof(gpu));
	CHECK(r > 0, "GPU_INFO: BVNC %llu.%llu.%llu.%llu, %u phantom(s)",
	      (unsigned long long)(gpu.gpu_id >> 48), (unsigned long long)((gpu.gpu_id >> 32) & 0xffff),
	      (unsigned long long)((gpu.gpu_id >> 16) & 0xffff), (unsigned long long)(gpu.gpu_id & 0xffff),
	      gpu.num_phantoms);
	r = query(fd, DRM_PVR_DEV_QUERY_RUNTIME_INFO_GET, &rt, sizeof(rt));
	CHECK(r > 0, "RUNTIME_INFO: free list %llu..%llu pages, common store alloc region %u",
	      (unsigned long long)rt.free_list_min_pages, (unsigned long long)rt.free_list_max_pages,
	      rt.common_store_alloc_region_size);
	hi.heaps.count = DRM_PVR_HEAP_COUNT;
	hi.heaps.stride = sizeof(heaps[0]);
	hi.heaps.array = (uintptr_t)heaps;
	r = query(fd, DRM_PVR_DEV_QUERY_HEAP_INFO_GET, &hi, sizeof(hi));
	CHECK(r > 0, "HEAP_INFO: %u heaps", hi.heaps.count);
	for (i = 0; i < (int)hi.heaps.count && i < DRM_PVR_HEAP_COUNT; i++)
		printf("        heap %d: %#014llx + %#llx, page 2^%u\n", i, (unsigned long long)heaps[i].base,
		       (unsigned long long)heaps[i].size, heaps[i].page_size_log2);
	gen_base = heaps[DRM_PVR_HEAP_GENERAL].base;
	gen_size = heaps[DRM_PVR_HEAP_GENERAL].size;

	r = ioctl(fd, DRM_IOCTL_PVR_CREATE_VM_CONTEXT, &vm);
	CHECK(!r, "CREATE_VM_CONTEXT: handle %u", vm.handle);
	r = ioctl(fd, DRM_IOCTL_PVR_CREATE_BO, &bo);
	CHECK(!r, "CREATE_BO 16 KiB: handle %u", bo.handle);
	off.handle = bo.handle;
	r = ioctl(fd, DRM_IOCTL_PVR_GET_BO_MMAP_OFFSET, &off);
	CHECK(!r, "GET_BO_MMAP_OFFSET: %#llx", (unsigned long long)off.offset);
	cpu = mmap(NULL, bo.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, off.offset);
	CHECK(cpu != MAP_FAILED, "mmap");
	if (cpu != MAP_FAILED) {
		for (i = 0; i < 0x1000; i++)
			cpu[i] = 0x10000 + i;
		for (i = 0; i < 0x1000 && cpu[i] == 0x10000u + i; i++)
			;
		CHECK(i == 0x1000, "CPU writes and reads the BO back");
	}

	map.vm_context_handle = vm.handle;
	map.handle = bo.handle;
	map.device_addr = gen_base + 0x100000;
	map.size = bo.size;
	r = ioctl(fd, DRM_IOCTL_PVR_VM_MAP, &map);
	CHECK(!r, "VM_MAP at %#llx", (unsigned long long)map.device_addr);
	unmap.vm_context_handle = vm.handle;
	unmap.device_addr = map.device_addr;
	unmap.size = map.size;
	r = ioctl(fd, DRM_IOCTL_PVR_VM_UNMAP, &unmap);
	CHECK(!r, "VM_UNMAP");

	/* right after the general heap the size reports: the kernel's page */
	map.device_addr = gen_base + gen_size;
	r = ioctl(fd, DRM_IOCTL_PVR_VM_MAP, &map);
	CHECK(r && errno == EINVAL, "VM_MAP at the reserved %#llx refused (%s)",
	      (unsigned long long)map.device_addr, r ? strerror(errno) : "mapped!");

	compute(fd, vm.handle, heaps);

	printf("%s: %d failure(s)\n", fails ? "FAIL" : "PASS", fails);
	return fails != 0;
}
