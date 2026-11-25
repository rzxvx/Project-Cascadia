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

/* a USC program from the self-test's firmware file (G9PT: name[24], off, size) */
static int load_usc(const char *name, uint8_t *dst, uint32_t *size)
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
		if (!strncmp((char *)buf + 8 + 32 * i, name, 24) && off + sz <= len) {
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
	r = load_usc("store_const", (uint8_t *)code + CODE_USC, &usc_size);
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

/*
 * G6's clear + triangle through the UAPI, in a VM of its own: a free list,
 * an HWRT dataset (64x64), a render context, a geometry and a fragment job
 * each time, the pixels checked. The programs and state are those of
 * docs/gpu.md "G6 legalization", at the places the kernel's translation
 * expects: RGX+0x470's PDS program in the VDM sync area (PDS heap +0x40,
 * code +0x60), the end-of-tile PDS code in the EOT area (PDS heap +0x80).
 */
static uint32_t f32b(float f)
{
	uint32_t u;

	memcpy(&u, &f, 4);
	return u;
}

static void render(int fd, const struct drm_pvr_heap *heaps, int n)
{
	struct drm_pvr_ioctl_create_vm_context_args vm = { 0 };
	struct drm_pvr_ioctl_create_free_list_args fla = { 0 };
	struct drm_pvr_ioctl_create_hwrt_dataset_args hw = { 0 };
	struct drm_pvr_ioctl_create_context_args ctx = { 0 };
	uint64_t pds = heaps[DRM_PVR_HEAP_PDS_CODE_DATA].base, usc = heaps[DRM_PVR_HEAP_USC_CODE].base;
	uint64_t gen = heaps[DRM_PVR_HEAP_GENERAL].base;
	uint64_t g3va = gen + 0x400000, pmbva = gen + 0x300000, flva = gen + 0x600000;
	uint32_t code_h, g3_h, pmb_h, fl_h, *code, *g3, *pmb, *fl, sz, i, k, ok = 0, last_bg = 0, last_tri = 0;
	uint64_t tva = g3va + 0x6c000;
	struct timespec t0, t1;
	int r;

	r = ioctl(fd, DRM_IOCTL_PVR_CREATE_VM_CONTEXT, &vm);
	code = new_bo(fd, 0x10000, &code_h);
	g3 = new_bo(fd, 0x100000, &g3_h);
	pmb = new_bo(fd, 0x100000, &pmb_h);
	fl = new_bo(fd, 0x4000, &fl_h);
	CHECK(!r && code && g3 && pmb && fl, "render: VM and BOs");
	if (r || !code || !g3 || !pmb || !fl)
		return;
	r = vm_map(fd, vm.handle, code_h, pds, 0x10000) || vm_map(fd, vm.handle, code_h, usc, 0x10000) ||
	    vm_map(fd, vm.handle, g3_h, g3va, 0x100000) || vm_map(fd, vm.handle, pmb_h, pmbva, 0x100000) ||
	    vm_map(fd, vm.handle, fl_h, flva, 0x4000);
	CHECK(!r, "render: mapped (code at the PDS / USC bases, data in the general heap)");

	/* USC code; the rest of the code BO PDS HALT (the PPP blocks point at 0xc0) */
	for (i = 0; i < 0x10000 / 4; i++)
		code[i] = 0xd3000000;
	r = load_usc("tri_vs", (uint8_t *)code + 0x400, &sz) || load_usc("tri_fs", (uint8_t *)code + 0x600, &sz) ||
	    load_usc("bg_fs", (uint8_t *)code + 0x700, &sz);
	CHECK(!r, "render: tri_vs, tri_fs, bg_fs");
	{
		static const uint16_t eot[] = { 0x0198, 0x0100, 0x0101, 0x0120, 0x0154, 0x0038,
						0x011c, 0x4100, 0x0800, 0x8000 };
		static const uint8_t end470[] = { 0x5e, 0x01, 0x00, 0xa0 };

		memcpy((uint8_t *)code + 0x900, eot, sizeof(eot));
		memcpy((uint8_t *)code + 0xb00, end470, sizeof(end470));
	}
	/* VDM sync area: RGX+0x470's program (data {DOUTU -> end470}, code at +0x20) */
	code[0x40 / 4] = 0xb00; code[0x44 / 4] = 0x10;
	for (i = 0x48; i < 0x60; i += 4)
		code[i / 4] = 0;
	code[0x60 / 4] = 0xf4000002; code[0x64 / 4] = 0xd3000000;
	/* EOT area: DOUTD of the image state, DOUTU END, HALT; its data at 0x1540 */
	code[0x80 / 4] = 0xf0000800; code[0x84 / 4] = 0xf4000012; code[0x88 / 4] = 0xd3000000;
	/* vertex 0x1100 / 0x1180, shared 0x1200 / 0x1280, fragment 0x1300, background 0x1400 / 0x1480 / 0x14c0 */
	{
		uint32_t v[] = { 0xf0000a01, 0xfc000002, 0xd3000000 };
		uint64_t q;

		memcpy(code + 0x1100 / 4, v, sizeof(v));
		code[0x1180 / 4] = 0x400; code[0x1184 / 4] = 0x10; code[0x1188 / 4] = 0x80000000; code[0x118c / 4] = 0;
		code[0x1200 / 4] = 0xf4000800; code[0x1204 / 4] = 0xd3000000;
		q = tva + 0x800; memcpy(code + 0x1280 / 4, &q, 8); q = 0x90000006; memcpy(code + 0x1288 / 4, &q, 8);
		code[0x1300 / 4] = 0x600; code[0x1304 / 4] = 0x10;
		for (i = 0x1308; i < 0x1320; i += 4)
			code[i / 4] = 0;
		code[0x1320 / 4] = 0xf4000002;
		code[0x1400 / 4] = 0x700; code[0x1404 / 4] = 0x10;
		for (i = 0x1408; i < 0x1420; i += 4)
			code[i / 4] = 0;
		code[0x1420 / 4] = 0xf4000002;
		code[0x1480 / 4] = 0xf4000800; code[0x1484 / 4] = 0xd3000000;
		q = tva + 0x900; memcpy(code + 0x14c0 / 4, &q, 8); q = 0x90004004; memcpy(code + 0x14c8 / 4, &q, 8);
		q = tva + 0x920; memcpy(code + 0x1540 / 4, &q, 8); q = 0x90000007; memcpy(code + 0x1548 / 4, &q, 8);
		code[0x1550 / 4] = 0x900; code[0x1554 / 4] = 0x10;
	}
	/* constants, colour, image state (BGRA8 linear 64x64 at g3va + 0x50000) */
	{
		float corners[6] = { -1, -1, 0, -1, -1, 1 }, colour[4] = { 0.25f, 0.5f, 0.75f, 1 };
		/* word 2: address bits 32:1; word 3 bits 0..: address bits 33.. (to check) */
		uint32_t img[7] = { 0x03050502 | (31u << 27), 1 | (63 << 9), (uint32_t)((g3va + 0x50000) >> 1),
				    0x80000000 | (3 << 15) | (uint32_t)((g3va + 0x50000) >> 33), 0, 0xc0000000, 0 };

		memcpy(g3 + (0x6c000 + 0x800) / 4, corners, sizeof(corners));
		memcpy(g3 + (0x6c000 + 0x900) / 4, colour, sizeof(colour));
		memcpy(g3 + (0x6c000 + 0x920) / 4, img, sizeof(img));
	}
	/* PPP state blocks and the VDM stream (docs "G6 legalization") */
	{
		const uint32_t blk[8][9] = {
			{ 0x001c0000, 0, 0, 0 },
			{ 0x000000c0, 0x1300, 0xc0, 0x00800000, 0, 0xc0 },
			{ 0x00020000, 0x04010000 },
			{ 0x0000001f, 0x03c40000, 0x003e0f00, 0x0e000000, 0x00100f00, 0 },
			{ 0x00000c00, 0x80000001, 1, f32b(32), f32b(32), f32b(32), f32b(-32), 0, 0x3f800000 },
			{ 0x00210000, 0x1e3ce508, 0x000004a0 },
			{ 0x00000020, 0 },
			{ 0x02000000, (1 << 18) | 1, 0 },
		};
		const int len[8] = { 4, 6, 2, 6, 9, 3, 2, 3 };
		uint32_t st[64], m = 0;

		for (k = 0; k < 8; k++)
			memcpy(g3 + (0x6c000 + 0x100 + 0x40 * k) / 4, blk[k], 4 * len[k]);
#define PPP(b) do { uint64_t at = tva + 0x100 + 0x40 * (b); st[m++] = (len[b] << 8) | (uint32_t)(at >> 32); \
		    st[m++] = (uint32_t)at & 0xfffffffc; } while (0)
		st[m++] = 0x44002f80; st[m++] = 0x1100; st[m++] = 4; st[m++] = 0x1081;
		PPP(0);
		st[m++] = 0x20020001; st[m++] = 0x1280; st[m++] = 0x1200; st[m++] = 0x48002f80; st[m++] = 0x1180;
		for (k = 1; k <= 6; k++)
			PPP(k);
		st[m++] = 0x6e440600; st[m++] = 3; st[m++] = 0; st[m++] = 0; st[m++] = 0xaa;
		PPP(7);
		st[m++] = 0xc0000000;
#undef PPP
		memcpy(g3 + 0x6c000 / 4, st, 4 * m);
	}

	fla.free_list_gpu_addr = flva;
	fla.initial_num_pages = fla.max_num_pages = 128;
	fla.vm_context_handle = vm.handle;
	r = ioctl(fd, DRM_IOCTL_PVR_CREATE_FREE_LIST, &fla);
	CHECK(!r, "CREATE_FREE_LIST: 128 pages, handle %u%s%s", fla.handle, r ? " -- " : "", r ? strerror(errno) : "");
	hw.geom_data_args.tpc_dev_addr = pmbva + 0x40000;
	hw.geom_data_args.tpc_size = 0x40000;
	hw.geom_data_args.vheap_table_dev_addr = g3va + 0x59000;
	for (i = 0; i < 2; i++) {
		hw.rt_data_args[i].pm_mlist_dev_addr = g3va + 0x57000;
		hw.rt_data_args[i].macrotile_array_dev_addr = g3va + 0x56000;
		hw.rt_data_args[i].region_header_dev_addr = pmbva;
	}
	hw.free_list_handles[0] = fla.handle;
	hw.width = hw.height = 64;
	hw.samples = hw.layers = 1;
	r = r ?: ioctl(fd, DRM_IOCTL_PVR_CREATE_HWRT_DATASET, &hw);
	CHECK(!r, "CREATE_HWRT_DATASET: 64x64, handle %u%s%s", hw.handle, r ? " -- " : "", r ? strerror(errno) : "");
	ctx.type = DRM_PVR_CTX_TYPE_RENDER;
	ctx.vm_context_handle = vm.handle;
	r = r ?: ioctl(fd, DRM_IOCTL_PVR_CREATE_CONTEXT, &ctx);
	CHECK(!r, "CREATE_CONTEXT (render): handle %u%s%s", ctx.handle, r ? " -- " : "", r ? strerror(errno) : "");
	if (r)
		return;

	for (int round = 0; round < 2; round++) {
	if (round) {
		/* past the driver's idle time: the firmware goes off; the same objects again */
		sleep(7);
		n = 5;
		ok = 0;
	}
	clock_gettime(CLOCK_MONOTONIC, &t0);
	for (k = 0; k < (uint32_t)n; k++) {
		uint64_t geom[5] = { 40, tva, 0, 2 | (0x20000ULL << 32), 0 };
		uint8_t frag[376] = { 0 };
		uint32_t *f32p = (uint32_t *)(frag + 336);
		uint64_t *bg = (uint64_t *)(frag + 256);
		struct drm_pvr_job jobs[2] = { 0 };
		struct drm_pvr_sync_op op;
		struct drm_pvr_ioctl_submit_jobs_args sub = { 0 };
		struct drm_syncobj_create sc = { 0 };
		struct drm_syncobj_wait sw = { 0 };
		struct drm_syncobj_destroy sd;
		uint32_t nbg = 0, ntri = 0;

		for (i = 0; i < 64 * 64; i++)
			g3[0x50000 / 4 + i] = 0x5a5a5a5a;
		*(uint32_t *)frag = 376;
		bg[0] = (0x1480ULL << 32) | 0x1400;	/* pds_bgnd[0..2] */
		bg[1] = 0x14c0;
		bg[2] = 0x0080000000800000ULL;
		f32p[1] = 0x3f800000;			/* isp_bgobjdepth */
		f32p[2] = 0x300;			/* isp_bgobjvals */
		f32p[3] = 0xf;				/* isp_aa */
		f32p[4] = 0x0023000f;			/* isp_ctl */
		f32p[5] = 0x402;			/* event_pixel_pds_info */
		f32p[7] = 0x1540;			/* event_pixel_pds_data */
		ioctl(fd, DRM_IOCTL_SYNCOBJ_CREATE, &sc);
		op = (struct drm_pvr_sync_op){ .handle = sc.handle, .flags = DRM_PVR_SYNC_OP_FLAG_SIGNAL };
		for (i = 0; i < 2; i++) {
			jobs[i].type = i ? DRM_PVR_JOB_TYPE_FRAGMENT : DRM_PVR_JOB_TYPE_GEOMETRY;
			jobs[i].context_handle = ctx.handle;
			jobs[i].cmd_stream = i ? (uintptr_t)frag : (uintptr_t)geom;
			jobs[i].cmd_stream_len = i ? 376 : 40;
			jobs[i].hwrt.set_handle = hw.handle;
			jobs[i].hwrt.data_index = k & 1;
		}
		jobs[1].sync_ops = (struct drm_pvr_obj_array){ .stride = sizeof(op), .count = 1, .array = (uintptr_t)&op };
		sub.jobs = (struct drm_pvr_obj_array){ .stride = sizeof(jobs[0]), .count = 2, .array = (uintptr_t)jobs };
		r = ioctl(fd, DRM_IOCTL_PVR_SUBMIT_JOBS, &sub);
		if (r) {
			printf("  FAIL  SUBMIT_JOBS (geometry + fragment): %s\n", strerror(errno));
			fails++;
			break;
		}
		clock_gettime(CLOCK_MONOTONIC, &t1);
		sw.handles = (uintptr_t)&sc.handle;
		sw.count_handles = 1;
		sw.timeout_nsec = t1.tv_sec * 1000000000LL + t1.tv_nsec + 3000000000LL;
		sw.flags = DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT;
		r = ioctl(fd, DRM_IOCTL_SYNCOBJ_WAIT, &sw);
		sd.handle = sc.handle;
		sd.pad = 0;
		ioctl(fd, DRM_IOCTL_SYNCOBJ_DESTROY, &sd);
		for (i = 0; i < 64 * 64; i++) {
			nbg += g3[0x50000 / 4 + i] == 0xff4080bf;
			ntri += g3[0x50000 / 4 + i] == 0xffff0000;
		}
		last_bg = nbg;
		last_tri = ntri;
		if (r) {
			printf("  FAIL  SYNCOBJ_WAIT (render %u): %s\n", k, strerror(errno));
			fails++;
			break;
		}
		ok += nbg == 3072 && ntri == 1024;
	}
	clock_gettime(CLOCK_MONOTONIC, &t1);
	CHECK(ok == (uint32_t)n, "render x%d (clear + triangle, 64x64)%s: %u right; last %u background, %u triangle; %.0f us per render",
	      n, round ? " after an idle power-off, same objects" : "", ok, last_bg, last_tri,
	      ((t1.tv_sec - t0.tv_sec) * 1e9 + (t1.tv_nsec - t0.tv_nsec)) / 1e3 / n);
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
	render(fd, heaps, 20);

	printf("%s: %d failure(s)\n", fails ? "FAIL" : "PASS", fails);
	return fails != 0;
}
