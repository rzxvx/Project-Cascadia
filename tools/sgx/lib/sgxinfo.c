/* sgxinfo -- the render node's first check: what the kernel says about the
 * GPU, and whether buffers come out where they should.
 *
 *   sgxinfo          parameters, then three buffers (one the kernel places,
 *                    one in the code zone, one at a fixed address), each
 *                    written and read back through its mapping
 *
 * No GPU work is submitted: sgx2d (sprites, demo2, supertux-gpu) does that.
 * Exit status 0 when everything checked out. */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <drm.h>
#include "apple_sgx_drm.h"

static int fd;

static uint64_t param(uint32_t p, const char *name)
{
	struct drm_apple_sgx_get_param g = { .param = p };

	if (ioctl(fd, DRM_IOCTL_APPLE_SGX_GET_PARAM, &g)) {
		printf("  %-20s error %d\n", name, errno);
		return 0;
	}
	printf("  %-20s 0x%08llx  %llu\n", name, (unsigned long long)g.value,
	       (unsigned long long)g.value);
	return g.value;
}

/* create, map, write a pattern, read it back */
static int bo_test(const char *what, uint32_t flags, uint32_t va, uint32_t size,
		   uint32_t lo, uint32_t hi)
{
	struct drm_apple_sgx_gem_create c = { .size = size, .flags = flags, .va = va };
	struct drm_apple_sgx_gem_mmap_offset m = { 0 };
	struct drm_gem_close cl = { 0 };
	uint32_t *p, i, bad = 0;

	if (ioctl(fd, DRM_IOCTL_APPLE_SGX_GEM_CREATE, &c)) {
		printf("  %-12s create: %s\n", what, strerror(errno));
		return 1;
	}
	m.handle = c.handle;
	if (ioctl(fd, DRM_IOCTL_APPLE_SGX_GEM_MMAP_OFFSET, &m)) {
		printf("  %-12s mmap offset: %s\n", what, strerror(errno));
		return 1;
	}
	p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, (off_t)m.offset);
	if (p == MAP_FAILED) {
		printf("  %-12s mmap: %s\n", what, strerror(errno));
		return 1;
	}
	for (i = 0; i < size / 4; i++)
		p[i] = c.va + 4 * i;
	for (i = 0; i < size / 4; i++)
		bad += p[i] != c.va + 4 * i;
	munmap(p, size);
	cl.handle = c.handle;
	ioctl(fd, DRM_IOCTL_GEM_CLOSE, &cl);
	printf("  %-12s handle %u at GPU 0x%08x, %u KiB: %s%s\n", what, c.handle, c.va,
	       size >> 10, bad ? "READ BACK WRONG" : "read back",
	       c.va < lo || c.va + size > hi ? ", OUTSIDE ITS RANGE" : "");
	return bad || c.va < lo || c.va + size > hi;
}

int main(void)
{
	char path[32], name[32];
	uint64_t va0, va1, cb0, cb1;
	int i, fails = 0;

	for (i = 128, fd = -1; i < 192 && fd < 0; i++) {
		struct drm_version v = { 0 };

		snprintf(path, sizeof(path), "/dev/dri/renderD%d", i);
		if ((fd = open(path, O_RDWR | O_CLOEXEC)) < 0)
			continue;
		memset(name, 0, sizeof(name));
		v.name = name;
		v.name_len = sizeof(name) - 1;
		if (ioctl(fd, DRM_IOCTL_VERSION, &v) || strcmp(name, "apple_sgx")) {
			close(fd);
			fd = -1;
		}
	}
	if (fd < 0) {
		fprintf(stderr, "sgxinfo: no apple_sgx render node in /dev/dri\n");
		return 1;
	}
	printf("%s:\n", path);
	param(APPLE_SGX_PARAM_UAPI_VERSION, "uapi version");
	param(APPLE_SGX_PARAM_CORE_ID, "core id");
	param(APPLE_SGX_PARAM_CORE_REVISION, "core revision");
	param(APPLE_SGX_PARAM_NUM_CORES, "cores");
	param(APPLE_SGX_PARAM_CLOCK_KHZ, "clock kHz");
	fails += !param(APPLE_SGX_PARAM_UKERNEL_UP, "microkernel up");
	param(APPLE_SGX_PARAM_UKERNEL_BOOTS, "microkernel boots");
	va0 = param(APPLE_SGX_PARAM_VA_START, "va start");
	va1 = param(APPLE_SGX_PARAM_VA_END, "va end");
	param(APPLE_SGX_PARAM_CODE_BASE, "code base");
	cb0 = param(APPLE_SGX_PARAM_CODE_VA_START, "code va start");
	cb1 = param(APPLE_SGX_PARAM_CODE_VA_END, "code va end");
	param(APPLE_SGX_PARAM_FB_VA, "framebuffer va");
	param(APPLE_SGX_PARAM_FB_WIDTH, "framebuffer width");
	param(APPLE_SGX_PARAM_FB_HEIGHT, "framebuffer height");
	param(APPLE_SGX_PARAM_FB_STRIDE, "framebuffer stride");
	param(APPLE_SGX_PARAM_RENDERS_SUBMITTED, "renders submitted");
	param(APPLE_SGX_PARAM_RENDERS_DONE, "renders done");
	param(APPLE_SGX_PARAM_RENDERS_TIMED_OUT, "renders timed out");

	printf("buffers:\n");
	fails += bo_test("anywhere", 0, 0, 0x10000, va0, va1);
	fails += bo_test("code", APPLE_SGX_BO_USSE_CODE, 0, 0x4000, cb0, cb1);
	fails += bo_test("fixed", APPLE_SGX_BO_FIXED_VA, 0xc0000000, 0x3000,
			 0xc0000000, 0xc0003000);
	printf("%s\n", fails ? "FAILED" : "all good");
	return !!fails;
}
