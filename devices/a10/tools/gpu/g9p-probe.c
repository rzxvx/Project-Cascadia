// SPDX-License-Identifier: MIT
/*
 * g9p-probe: one compute dispatch on the iPad 7's GPU through the powervr
 * UAPI, every word of it settable from the command line -- for finding the
 * compute ABI of Apple's offline compiler (thread / threadgroup IDs, several
 * buffers). Kernels from build/gpu/g9p-probe.bin (g9p-probe-shaders.py).
 *
 *   g9p-probe KERNEL [key=value ...]
 *     x= y= z=        CDM K3 / K4 / K5 (default 0)
 *     w6= w7=         the record's words 6 / 7 (default 0x40000001 / 0)
 *     k0=             the kernel record's K0 (default 0x2103)
 *     temps=          DOUTU word 1 (default 0x10)
 *     kd=I:V          the kernel PDS data dword I (4..31) = V
 *     kd2= kd3=       the kernel PDS data words 2 / 3 (default 0x90000000 / 0x80000000)
 *     tab=I:V,...     common-store table dword I = V (the DMA loads 64 dwords;
 *                     default: dword 2i / 2i+1 = buffer i % 16's VA, + 0x2000 for i >= 16)
 *     in=B:I:V        buffer B word I = V before the run
 *     dump=N          print buffer words that changed (default all, up to N per buffer)
 *     multi=N         one DMA record, then N kernel records whose threadgroup
 *                     position is their index (from their own PDS data)
 *     tail=1          end the stream (not with multi=) with a DOUTC fence record
 *     cs=N            dwords the DMA loads (default 32, up to 64)
 *     n=N             N DMA + kernel record pairs; pair i's table is the base
 *                     table with dwords 0 / 1 (buffer 0) moved by 0x800 i
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

#define DIE(...) do { fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); exit(1); } while (0)

static int fd;

static uint32_t *new_bo(uint64_t size, uint32_t *handle)
{
	struct drm_pvr_ioctl_create_bo_args c = { .size = size, .flags = DRM_PVR_BO_ALLOW_CPU_USERSPACE_ACCESS };
	struct drm_pvr_ioctl_get_bo_mmap_offset_args o = { 0 };
	void *p;

	if (ioctl(fd, DRM_IOCTL_PVR_CREATE_BO, &c))
		DIE("CREATE_BO: %s", strerror(errno));
	o.handle = c.handle;
	if (ioctl(fd, DRM_IOCTL_PVR_GET_BO_MMAP_OFFSET, &o))
		DIE("MMAP_OFFSET: %s", strerror(errno));
	p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, o.offset);
	if (p == MAP_FAILED)
		DIE("mmap: %s", strerror(errno));
	*handle = c.handle;
	return p;
}

static void vm_map(uint32_t vm, uint32_t bo, uint64_t va, uint64_t size)
{
	struct drm_pvr_ioctl_vm_map_args m = { .vm_context_handle = vm, .handle = bo,
					       .device_addr = va, .size = size };

	if (ioctl(fd, DRM_IOCTL_PVR_VM_MAP, &m))
		DIE("VM_MAP %#llx: %s", (unsigned long long)va, strerror(errno));
}

static uint32_t load_usc(const char *file, const char *name, uint8_t *dst)
{
	static uint8_t buf[65536];
	FILE *f = fopen(file, "rb");
	uint32_t n, i;
	size_t len;

	if (!f)
		DIE("%s: %s", file, strerror(errno));
	len = fread(buf, 1, sizeof(buf), f);
	fclose(f);
	if (len < 8 || memcmp(buf, "G9PT", 4))
		DIE("%s: not a G9PT file", file);
	memcpy(&n, buf + 4, 4);
	for (i = 0; i < n && 8 + 32 * (i + 1) <= len; i++) {
		uint32_t off, sz;

		memcpy(&off, buf + 8 + 32 * i + 24, 4);
		memcpy(&sz, buf + 8 + 32 * i + 28, 4);
		if (!strncmp((char *)buf + 8 + 32 * i, name, 24) && off + sz <= len) {
			memcpy(dst, buf + off, sz);
			return sz;
		}
	}
	DIE("%s: no kernel %s", file, name);
}

#define CODE_HALT	0x2000
#define CODE_DMA_PDS	0x2100
#define CODE_KER_PDS	0x2200
#define CODE_KER_DATA	0x2280
#define CODE_USC	0x2400
#define CODE_DMA_DATA	0x4000		/* pair i's DMA data at +0x20 i */
#define TAB_OFF		0x80000		/* pair i's table at data + 0x80000 + 0x100 i */
#define NBUF		16
#define BUF_OFF		0x10000		/* buffer i at data + 0x10000 + 0x4000 i */
#define BUF_SIZE	0x4000
#define SENTINEL	0xabadbeef

int main(int argc, char **argv)
{
	static const uint32_t dma_pds[] = { 0xf0001000, 0xf4001808, 0xd3000000 };
	static const uint32_t ker_pds[] = { 0xc2c00003, 0xf0000a01, 0xd3000000, 0xf0000e01, 0xfc000002, 0xd3000000 };
	struct drm_pvr_ioctl_create_vm_context_args vm = { 0 };
	struct drm_pvr_ioctl_dev_query_args q = { 0 };
	struct drm_pvr_dev_query_heap_info hi = { 0 };
	struct drm_pvr_heap heaps[DRM_PVR_HEAP_COUNT];
	struct drm_pvr_ioctl_create_context_args ctx = { 0 };
	uint32_t x = 0, y = 0, z = 0, w6 = 0x40000001, w7 = 0, k0 = 0x2103, temps = 0x10;
	uint32_t kd2 = 0x90000000, kd3 = 0x80000000, dump = 64, n = 1, cs = 32, multi = 0, tail = 0, code_h, data_h, *code, *data, *tab, i, b;
	uint64_t pds, usc, data_va, state[1 + 7] = { 0 }, cmd[5];
	const char *file = "/run/g9p-probe.bin";
	struct timespec t0, t1;
	int r;

	if (argc < 2)
		DIE("usage: g9p-probe KERNEL [x= y= z= w6= w7= k0= temps= kd2= kd3= tab=I:V,.. in=B:I:V dump=N file=]");
	fd = open("/dev/dri/renderD128", O_RDWR);
	if (fd < 0)
		DIE("renderD128: %s", strerror(errno));
	q.type = DRM_PVR_DEV_QUERY_HEAP_INFO_GET;
	q.pointer = (uintptr_t)&hi;
	q.size = sizeof(hi);
	hi.heaps.stride = sizeof(heaps[0]);
	hi.heaps.count = DRM_PVR_HEAP_COUNT;
	hi.heaps.array = (uintptr_t)heaps;
	if (ioctl(fd, DRM_IOCTL_PVR_DEV_QUERY, &q))
		DIE("HEAP_INFO: %s", strerror(errno));
	pds = heaps[DRM_PVR_HEAP_PDS_CODE_DATA].base;
	usc = heaps[DRM_PVR_HEAP_USC_CODE].base;
	data_va = heaps[DRM_PVR_HEAP_GENERAL].base + 0x200000;

	if (ioctl(fd, DRM_IOCTL_PVR_CREATE_VM_CONTEXT, &vm))
		DIE("CREATE_VM_CONTEXT: %s", strerror(errno));
	code = new_bo(0x10000, &code_h);
	data = new_bo(0x100000, &data_h);
	vm_map(vm.handle, code_h, pds, 0x10000);
	vm_map(vm.handle, code_h, usc, 0x10000);
	vm_map(vm.handle, data_h, data_va, 0x100000);

	for (i = 0; i < CODE_USC / 4; i++)
		code[i] = 0xd3000000;
	memset(code + CODE_HALT / 4 + 1, 0, 0x1c);
	for (i = 2; i < argc; i++)
		if (!strncmp(argv[i], "file=", 5))
			file = argv[i] + 5;
	printf("%s: %u bytes of USC code\n", argv[1], load_usc(file, argv[1], (uint8_t *)code + CODE_USC));

	tab = data + TAB_OFF / 4;
	for (b = 0; b < 32; b++) {
		/* 64-bit slot b -> buffer b % 16, + 0x2000 for slots 16-31 */
		uint64_t va = data_va + BUF_OFF + (uint64_t)BUF_SIZE * (b % 16) + 0x2000 * (b / 16);

		tab[2 * b] = (uint32_t)va;
		tab[2 * b + 1] = (uint32_t)(va >> 32);
	}
	for (b = 0; b < NBUF; b++) {
		for (i = 0; i < BUF_SIZE / 4; i++)
			data[(BUF_OFF + BUF_SIZE * b) / 4 + i] = SENTINEL;
	}
	for (i = 2; i < argc; i++) {
		char *a = argv[i], *v = strchr(a, '=');

		if (!v)
			DIE("bad arg %s", a);
		v++;
#define U(s) strtoul(s, NULL, 0)
		if (!strncmp(a, "x=", 2)) x = U(v);
		else if (!strncmp(a, "y=", 2)) y = U(v);
		else if (!strncmp(a, "z=", 2)) z = U(v);
		else if (!strncmp(a, "w6=", 3)) w6 = U(v);
		else if (!strncmp(a, "w7=", 3)) w7 = U(v);
		else if (!strncmp(a, "k0=", 3)) k0 = U(v);
		else if (!strncmp(a, "temps=", 6)) temps = U(v);
		else if (!strncmp(a, "kd2=", 4)) kd2 = U(v);
		else if (!strncmp(a, "kd=", 3)) {
			char *e1;
			uint32_t ii = strtoul(v, &e1, 0);

			if (ii < 4 || ii >= 32)
				DIE("kd=I:V, I 4..31");
			code[CODE_KER_DATA / 4 + ii] = strtoul(e1 + 1, NULL, 0);
		}
		else if (!strncmp(a, "kd3=", 4)) kd3 = U(v);
		else if (!strncmp(a, "dump=", 5)) dump = U(v);
		else if (!strncmp(a, "n=", 2)) n = U(v);
		else if (!strncmp(a, "multi=", 6)) multi = U(v);
		else if (!strncmp(a, "tail=", 5)) tail = U(v);
		else if (!strncmp(a, "cs=", 3)) cs = U(v);
		else if (!strncmp(a, "file=", 5) || !strncmp(a, "kp=", 3)) ;
		else if (!strncmp(a, "tab=", 4)) {
			for (char *p = v; p && *p; p = strchr(p, ',') ? strchr(p, ',') + 1 : NULL) {
				char *e;
				uint32_t idx = strtoul(p, &e, 0);

				if (*e != ':' || idx >= 64)
					DIE("bad tab entry %s", p);
				tab[idx] = strtoul(e + 1, NULL, 0);
			}
		} else if (!strncmp(a, "in=", 3)) {
			char *e1, *e2;
			uint32_t bb = strtoul(v, &e1, 0), ii = strtoul(e1 + 1, &e2, 0);

			data[(BUF_OFF + BUF_SIZE * bb) / 4 + ii] = strtoul(e2 + 1, NULL, 0);
		} else
			DIE("bad arg %s", a);
#undef U
	}

	memcpy(code + CODE_DMA_PDS / 4, dma_pds, sizeof(dma_pds));
	memcpy(code + CODE_KER_PDS / 4, ker_pds, sizeof(ker_pds));
	for (i = 2; i < argc; i++)
		if (!strncmp(argv[i], "kp=", 3)) {	/* the kernel PDS code dword I = V */
			char *e1;
			uint32_t ii = strtoul(argv[i] + 3, &e1, 0);

			code[CODE_KER_PDS / 4 + ii] = strtoul(e1 + 1, NULL, 0);
		}
	code[CODE_KER_DATA / 4 + 0] = CODE_USC;
	code[CODE_KER_DATA / 4 + 1] = temps;
	code[CODE_KER_DATA / 4 + 2] = kd2;
	code[CODE_KER_DATA / 4 + 3] = kd3;
	if (n < 1 || n > 500)
		DIE("n: 1..500");
	for (i = 0; i < n; i++) {
		/* DMA: table dwords 0-15 -> common store 0, 16-31 -> 16 (LAST) */
		uint64_t t = data_va + TAB_OFF + 0x100 * i, va;
		uint64_t dd[4] = { t, t + 2 * cs, 0x10000000 | cs / 2, 0x90000000 | (cs / 2 << 13) | cs / 2 };
		const uint32_t rec[] = {
			0x4, (CODE_DMA_DATA + 0x20 * i) | 0x2, CODE_DMA_PDS, 0, 0, 0, 0x40000001, 0,
			k0, CODE_KER_DATA, CODE_KER_PDS, x, y, z, w6, w7,
		};

		memcpy((uint8_t *)code + CODE_DMA_DATA + 0x20 * i, dd, sizeof(dd));
		memcpy(data + 0x100 / 4 * i + TAB_OFF / 4, tab, 0x100);
		va = ((uint64_t)tab[1] << 32 | tab[0]) + 0x800 * i;
		data[(TAB_OFF + 0x100 * i) / 4] = (uint32_t)va;
		data[(TAB_OFF + 0x100 * i) / 4 + 1] = (uint32_t)(va >> 32);
		memcpy(data + 16 * i, rec, sizeof(rec));
	}
	data[16 * n] = 0x80000000;
	if (tail) {
		/* end the stream with a fence record (DOUTC, as iPadOS's jobs end) */
		static const uint32_t fpds[] = { 0xc2c00002, 0xd3000000, 0xf0000005, 0xd3000000 };
		const uint32_t frec[] = { 0xc0000003, 0x23c0 | 0x4, 0x2380, 0, 0, 0, 0x40000000, 0 };

		memcpy(code + 0x2380 / 4, fpds, sizeof(fpds));
		memset(code + 0x23c0 / 4, 0, 16);
		memcpy(data + 16 * n, frec, sizeof(frec));
		data[16 * n + 8] = 0x80000000;
	}
	if (multi) {
		/* one DMA record, then `multi` kernel records; kernel i's PDS data
		 * {DOUTU, 0x90000000, 0x80000000, i} at code + 0x8000 + 0x20 i, its
		 * shared-phase DOUTW taking dwords 4-5 */
		code[CODE_KER_PDS / 4 + 1] = 0xf0000811;
		for (i = 0; i < multi; i++) {
			uint32_t *kd = code + (0x8000 + 0x20 * i) / 4;
			const uint32_t rec[] = { k0 == 0x2103 ? 0x2107 : k0, 0x8000 + 0x20 * i, CODE_KER_PDS,
						 x, y, z, w6, w7 };

			kd[0] = CODE_USC; kd[1] = temps; kd[2] = kd2; kd[3] = kd3; kd[4] = i;
			kd[5] = kd[6] = kd[7] = 0;
			memcpy(data + 8 + 8 * i, rec, sizeof(rec));
		}
		data[8 + 8 * multi] = 0x80000000;
	}

	state[0] = sizeof(state);
	state[1] = ((uint64_t)(CODE_HALT + 0x10) << 32) | CODE_HALT;
	state[3] = state[1];
	ctx.type = DRM_PVR_CTX_TYPE_COMPUTE;
	ctx.vm_context_handle = vm.handle;
	ctx.static_context_state = (uintptr_t)state;
	ctx.static_context_state_len = sizeof(state);
	if (ioctl(fd, DRM_IOCTL_PVR_CREATE_CONTEXT, &ctx))
		DIE("CREATE_CONTEXT: %s", strerror(errno));

	{
		struct drm_pvr_job job = { 0 };
		struct drm_pvr_sync_op op;
		struct drm_pvr_ioctl_submit_jobs_args sub = { 0 };
		struct drm_syncobj_create sc = { 0 };
		struct drm_syncobj_wait sw = { 0 };

		ioctl(fd, DRM_IOCTL_SYNCOBJ_CREATE, &sc);
		cmd[0] = 36;
		cmd[1] = 0;
		cmd[2] = data_va;
		cmd[3] = data_va + 0x8000;
		cmd[4] = 0;
		op = (struct drm_pvr_sync_op){ .handle = sc.handle, .flags = DRM_PVR_SYNC_OP_FLAG_SIGNAL };
		job.type = DRM_PVR_JOB_TYPE_COMPUTE;
		job.context_handle = ctx.handle;
		job.cmd_stream = (uintptr_t)cmd;
		job.cmd_stream_len = 36;
		job.sync_ops = (struct drm_pvr_obj_array){ .stride = sizeof(op), .count = 1, .array = (uintptr_t)&op };
		sub.jobs = (struct drm_pvr_obj_array){ .stride = sizeof(job), .count = 1, .array = (uintptr_t)&job };
		clock_gettime(CLOCK_MONOTONIC, &t0);
		if (ioctl(fd, DRM_IOCTL_PVR_SUBMIT_JOBS, &sub))
			DIE("SUBMIT_JOBS: %s", strerror(errno));
		sw.handles = (uintptr_t)&sc.handle;
		sw.count_handles = 1;
		sw.timeout_nsec = t0.tv_sec * 1000000000LL + t0.tv_nsec + 3000000000LL;
		sw.flags = DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT;
		r = ioctl(fd, DRM_IOCTL_SYNCOBJ_WAIT, &sw);
		clock_gettime(CLOCK_MONOTONIC, &t1);
		printf("wait: %s, %.0f us\n", r ? strerror(errno) : "done",
		       ((t1.tv_sec - t0.tv_sec) * 1e9 + (t1.tv_nsec - t0.tv_nsec)) / 1e3);
	}
	if (getenv("TGW")) {		/* tgw: word tg * 128 + tl == tg << 16 | tl? */
		uint32_t *p = data + BUF_OFF / 4, bad = 0, ngw = atoi(getenv("TGW"));

		for (i = 0; i < ngw * 128 && i < NBUF * BUF_SIZE / 4; i++)
			if (p[i] != ((i / 128) << 16 | (i % 128)) && bad++ < 5)
				printf("  word %u = %08x\n", i, p[i]);
		printf("tgw: %u of %u words wrong\n", bad, ngw * 128);
	}
	for (b = 0; b < NBUF; b++) {
		uint32_t *p = data + (BUF_OFF + BUF_SIZE * b) / 4, n = 0;

		for (i = 0; i < BUF_SIZE / 4; i++) {
			if (p[i] == SENTINEL)
				continue;
			if (n++ < dump)
				printf("  buf %u [%4u] = %08x\n", b, i, p[i]);
		}
		if (n > dump)
			printf("  buf %u: %u words changed in all\n", b, n);
	}
	return r ? 1 : 0;
}
