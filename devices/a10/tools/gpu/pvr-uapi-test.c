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
	struct drm_pvr_ioctl_create_context_args ctx = { 0 };
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

	ctx.type = DRM_PVR_CTX_TYPE_COMPUTE;
	ctx.vm_context_handle = vm.handle;
	r = ioctl(fd, DRM_IOCTL_PVR_CREATE_CONTEXT, &ctx);
	printf("  info  CREATE_CONTEXT (compute): %s\n", r ? strerror(errno) : "created");

	printf("%s: %d failure(s)\n", fails ? "FAIL" : "PASS", fails);
	return fails != 0;
}
