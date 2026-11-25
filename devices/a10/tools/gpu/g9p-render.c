// SPDX-License-Identifier: MIT
/*
 * g9p-render: one draw on the iPad 7's GPU through the powervr UAPI with the
 * shaders and the per-draw state supplied from outside -- for decoding the
 * fragment side of the ISA (varyings, textures) with our own assembler
 * (tools/gpu/isa/render-check.py). The frame around the draw is G6 / G7's:
 * a 64x64 BGRA8 linear target, the clear, the end-of-tile program.
 *
 *   g9p-render FILE.bin NAME [out.raw]
 *
 * FILE.bin (G9PT) holds, per test NAME: NAME.vs / NAME.fs (USC code),
 * NAME.cfg (u32: vertex output dwords, VARYING0, SIZEINFO1, SIZEINFO2,
 * varying PDS data bytes, texture w, texture h, sampler 0, sampler 1,
 * texture state word 0[, vertices (3), ISPA front (0x003e0f00), ISPCTL
 * (0x03c40000), ISPA back (0x00100f00)]), NAME.vb (the vertex buffer, its address goes to
 * common-store dwords 0-1 of the vertex shader), NAME.vpds (the varying PDS
 * program: data, then code), NAME.tex (texels, as the GPU reads them).
 * Prints the 64x64 pixels' hash and writes them to out.raw.
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
static uint8_t pack[1 << 20];
static size_t pack_len;

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

/* an entry of the G9PT pack; NULL if missing */
static const uint8_t *entry(const char *name, uint32_t *size)
{
	uint32_t n, i;

	memcpy(&n, pack + 4, 4);
	for (i = 0; i < n; i++) {
		uint32_t off, sz;

		memcpy(&off, pack + 8 + 32 * i + 24, 4);
		memcpy(&sz, pack + 8 + 32 * i + 28, 4);
		if (!strncmp((char *)pack + 8 + 32 * i, name, 24) && off + sz <= pack_len) {
			*size = sz;
			return pack + off;
		}
	}
	return NULL;
}

static const uint8_t *need(const char *test, const char *what, uint32_t *size)
{
	char name[64];
	const uint8_t *p;

	snprintf(name, sizeof(name), "%s.%s", test, what);
	if (!(p = entry(name, size)))
		DIE("no %s in the pack", name);
	return p;
}

static uint32_t f32b(float f)
{
	uint32_t u;

	memcpy(&u, &f, 4);
	return u;
}

/* code BO layout (at the PDS and the USC heap bases) */
#define C_VS		0x2000	/* vertex shader USC code */
#define C_FS		0x3000	/* fragment shader USC code */
#define C_BG		0x0700	/* the clear (bg_fs, from the self-test's file) */
#define C_EOT		0x0900	/* end-of-tile USC */
#define C_470		0x0b00	/* RGX+0x470's USC task */
#define C_VPDS		0x1600	/* varying PDS program: data, then code */
#define C_TPDS		0x1700	/* texture-state DMA: data at +0, code at +0x20 */

static int load_bg_fs(uint8_t *dst)
{
	static uint8_t buf[4096];
	FILE *f = fopen("/lib/firmware/apple/g9p-selftest.bin", "rb");
	uint32_t n, i;
	size_t len;

	if (!f)
		return -1;
	len = fread(buf, 1, sizeof(buf), f);
	fclose(f);
	memcpy(&n, buf + 4, 4);
	for (i = 0; i < n && 8 + 32 * (i + 1) <= len; i++) {
		uint32_t off, sz;

		memcpy(&off, buf + 8 + 32 * i + 24, 4);
		memcpy(&sz, buf + 8 + 32 * i + 28, 4);
		if (!strcmp((char *)buf + 8 + 32 * i, "bg_fs")) {
			memcpy(dst, buf + off, sz);
			return 0;
		}
	}
	return -1;
}

int main(int argc, char **argv)
{
	struct drm_pvr_ioctl_create_vm_context_args vm = { 0 };
	struct drm_pvr_ioctl_create_free_list_args fla = { 0 };
	struct drm_pvr_ioctl_create_hwrt_dataset_args hw = { 0 };
	struct drm_pvr_ioctl_create_context_args ctx = { 0 };
	struct drm_pvr_ioctl_dev_query_args q = { 0 };
	struct drm_pvr_dev_query_heap_info hi = { 0 };
	struct drm_pvr_heap heaps[DRM_PVR_HEAP_COUNT];
	uint64_t pds, usc, gen, g3va, pmbva, flva, tva;
	uint32_t code_h, g3_h, pmb_h, fl_h, *code, *g3, *pmb, *fl, sz, i, k;
	const uint32_t *cfg;
	const uint8_t *p;
	int tex, r;
	FILE *f;

	if (argc < 3)
		DIE("usage: g9p-render FILE.bin NAME [out.raw]");
	if (!(f = fopen(argv[1], "rb")))
		DIE("%s: %s", argv[1], strerror(errno));
	pack_len = fread(pack, 1, sizeof(pack), f);
	fclose(f);
	if (pack_len < 8 || memcmp(pack, "G9PT", 4))
		DIE("%s: not G9PT", argv[1]);
	cfg = (const uint32_t *)need(argv[2], "cfg", &sz);
	if (sz < 40)
		DIE("cfg too short");
	tex = cfg[5] != 0;
	{
		static uint32_t c2[14];

		memset(c2, 0, sizeof(c2));
		memcpy(c2, cfg, sz < sizeof(c2) ? sz : sizeof(c2));
		if (!c2[10]) c2[10] = 3;
		if (!c2[11]) c2[11] = 0x003e0f00;
		if (!c2[12]) c2[12] = 0x03c40000;
		if (!c2[13]) c2[13] = 0x00100f00;
		cfg = c2;
	}

	fd = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
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
	gen = heaps[DRM_PVR_HEAP_GENERAL].base;
	g3va = gen + 0x400000;
	pmbva = gen + 0x300000;
	flva = gen + 0x600000;
	tva = g3va + 0x6c000;

	if (ioctl(fd, DRM_IOCTL_PVR_CREATE_VM_CONTEXT, &vm))
		DIE("CREATE_VM_CONTEXT: %s", strerror(errno));
	code = new_bo(0x10000, &code_h);
	g3 = new_bo(0x100000, &g3_h);
	pmb = new_bo(0x100000, &pmb_h);
	fl = new_bo(0x4000, &fl_h);
	vm_map(vm.handle, code_h, pds, 0x10000);
	vm_map(vm.handle, code_h, usc, 0x10000);
	vm_map(vm.handle, g3_h, g3va, 0x100000);
	vm_map(vm.handle, pmb_h, pmbva, 0x100000);
	vm_map(vm.handle, fl_h, flva, 0x4000);
	(void)pmb;
	(void)fl;

	/* PDS HALT everywhere; the programs at their places */
	for (i = 0; i < 0x10000 / 4; i++)
		code[i] = 0xd3000000;
	p = need(argv[2], "vs", &sz);
	if (sz > 0x1000)
		DIE("vs too big");
	memcpy((uint8_t *)code + C_VS, p, sz);
	p = need(argv[2], "fs", &sz);
	if (sz > 0x1000)
		DIE("fs too big");
	memcpy((uint8_t *)code + C_FS, p, sz);
	if (load_bg_fs((uint8_t *)code + C_BG))
		DIE("bg_fs: /lib/firmware/apple/g9p-selftest.bin");
	{
		static const uint16_t eot[] = { 0x0198, 0x0100, 0x0101, 0x0120, 0x0154, 0x0038,
						0x011c, 0x4100, 0x0800, 0x8000 };
		static const uint8_t end470[] = { 0x5e, 0x01, 0x00, 0xa0 };

		memcpy((uint8_t *)code + C_EOT, eot, sizeof(eot));
		memcpy((uint8_t *)code + C_470, end470, sizeof(end470));
	}
	/* VDM sync area: RGX+0x470's program; EOT area: DOUTD image state, DOUTU, HALT */
	code[0x40 / 4] = C_470; code[0x44 / 4] = 0x10;
	for (i = 0x48; i < 0x60; i += 4)
		code[i / 4] = 0;
	code[0x60 / 4] = 0xf4000002; code[0x64 / 4] = 0xd3000000;
	code[0x80 / 4] = 0xf0000800; code[0x84 / 4] = 0xf4000012; code[0x88 / 4] = 0xd3000000;
	{
		uint32_t v[] = { 0xf0000a01, 0xfc000002, 0xd3000000 };
		uint64_t qv;

		/* vertex kick: DOUTW the vertex index -> r0, DOUTU (8 temps: VDM_STATE5 0x1081's 8 registers) */
		memcpy(code + 0x1100 / 4, v, sizeof(v));
		code[0x1180 / 4] = C_VS; code[0x1184 / 4] = 0x10; code[0x1188 / 4] = 0x80000000; code[0x118c / 4] = 0;
		/* the vertex shader's common store: dwords 0-1 = the vertex buffer's address */
		code[0x1200 / 4] = 0xf4000800; code[0x1204 / 4] = 0xd3000000;
		qv = tva + 0x700; memcpy(code + 0x1280 / 4, &qv, 8); qv = 0x90000002; memcpy(code + 0x1288 / 4, &qv, 8);
		/* fragment kick */
		code[0x1300 / 4] = C_FS; code[0x1304 / 4] = 0x10;
		for (i = 0x1308; i < 0x1320; i += 4)
			code[i / 4] = 0;
		code[0x1320 / 4] = 0xf4000002;
		/* background */
		code[0x1400 / 4] = C_BG; code[0x1404 / 4] = 0x10;
		for (i = 0x1408; i < 0x1420; i += 4)
			code[i / 4] = 0;
		code[0x1420 / 4] = 0xf4000002;
		code[0x1480 / 4] = 0xf4000800; code[0x1484 / 4] = 0xd3000000;
		qv = tva + 0x900; memcpy(code + 0x14c0 / 4, &qv, 8); qv = 0x90004004; memcpy(code + 0x14c8 / 4, &qv, 8);
		qv = tva + 0x920; memcpy(code + 0x1540 / 4, &qv, 8); qv = 0x90000007; memcpy(code + 0x1548 / 4, &qv, 8);
		code[0x1550 / 4] = C_EOT; code[0x1554 / 4] = 0x10;
	}
	/* the varying PDS program, as supplied */
	if (cfg[4]) {
		p = need(argv[2], "vpds", &sz);
		if (sz > 0x100)
			DIE("vpds too big");
		memcpy((uint8_t *)code + C_VPDS, p, sz);
	}
	/* the vertex buffer, its address at tva + 0x700 */
	p = need(argv[2], "vb", &sz);
	if (sz > 0x1000)
		DIE("vb too big");
	memcpy(g3 + 0x60000 / 4, p, sz);
	{
		uint64_t vb = g3va + 0x60000;

		memcpy(g3 + (0x6c000 + 0x700) / 4, &vb, 8);
	}
	/* the texture: texels at g3 + 0x40000, state (8 dwords) + sampler (2) at tva + 0xa00 */
	if (tex) {
		uint64_t ta = g3va + 0x40000, q2;
		uint32_t *ts = g3 + (0x6c000 + 0xa00) / 4;

		p = need(argv[2], "tex", &sz);
		if (sz > 0x10000)
			DIE("tex too big");
		memcpy(g3 + 0x40000 / 4, p, sz);
		memset(ts, 0, 0x40);
		ts[0] = cfg[9] | (cfg[5] - 1) << 27;
		ts[1] = (cfg[5] - 1) >> 5 | (cfg[6] - 1) << 9;
		ts[2] = (uint32_t)(ta >> 1);
		ts[3] = (uint32_t)(ta >> 33);
		ts[8] = cfg[7];
		ts[9] = cfg[8];
		/* PDS: DOUTD state -> common store 0-7, DOUTD END sampler -> 8-9, HALT */
		q2 = tva + 0xa00; memcpy(code + C_TPDS / 4, &q2, 8);
		q2 = tva + 0xa20; memcpy(code + C_TPDS / 4 + 2, &q2, 8);
		code[C_TPDS / 4 + 4] = 0x10000008;
		code[C_TPDS / 4 + 5] = 0x90010002;
		code[C_TPDS / 4 + 6] = code[C_TPDS / 4 + 7] = 0;
		code[(C_TPDS + 0x20) / 4] = 0xf0001000;
		code[(C_TPDS + 0x20) / 4 + 1] = 0xf4001408;
		code[(C_TPDS + 0x20) / 4 + 2] = 0xd3000000;
	}
	/* clear colour, image state (BGRA8 linear 64x64 at g3va + 0x50000) */
	{
		float colour[4] = { 0.25f, 0.5f, 0.75f, 1 };
		uint32_t img[7] = { 0x03050502 | (31u << 27), 1 | (63 << 9), (uint32_t)((g3va + 0x50000) >> 1),
				    0x80000000 | (3 << 15) | (uint32_t)((g3va + 0x50000) >> 33), 0, 0xc0000000, 0 };

		memcpy(g3 + (0x6c000 + 0x900) / 4, colour, sizeof(colour));
		memcpy(g3 + (0x6c000 + 0x920) / 4, img, sizeof(img));
	}
	/* PPP state blocks and the VDM stream */
	{
		uint32_t blk[10][12], len[10], nb = 0, st[128], m = 0;
		uint32_t ptrs = tex ? 0x000002c0 : 0x000000c0;

#define B(...) do { uint32_t t_[] = { __VA_ARGS__ }; memcpy(blk[nb], t_, sizeof(t_)); \
		    len[nb++] = sizeof(t_) / 4; } while (0)
		B(0x001c0000, cfg[1], 0, 0);					/* varying words 0-2 */
		if (tex)
			B(ptrs, 0x1300, C_TPDS + 0x20, cfg[2], cfg[3], cfg[4] ? C_VPDS : 0xc0, C_TPDS);
		else
			B(ptrs, 0x1300, 0xc0, cfg[2], cfg[3], cfg[4] ? C_VPDS : 0xc0);
		B(0x00020000, cfg[0] << 24 | 0x00010000);			/* OUTPUT_SEL: vertex size, rhw */
		B(0x0000001f, cfg[12], cfg[11], 0x0e000000, cfg[13], 0);	/* ISPCTL, front A / B, back A / B */
		B(0x00000c00, 0x80000001, 1, f32b(32), f32b(32), f32b(32), f32b(-32), 0, 0x3f800000);
		B(0x00210000, 0x1e3ce508, 0x000004a0);
		B(0x00000020, 0);
		B(0x02000000, (1 << 18) | 1, 0);
#undef B
		for (k = 0; k < nb; k++)
			memcpy(g3 + (0x6c000 + 0x100 + 0x40 * k) / 4, blk[k], 4 * len[k]);
#define PPP(b) do { uint64_t at = tva + 0x100 + 0x40 * (b); st[m++] = (len[b] << 8) | (uint32_t)(at >> 32); \
		    st[m++] = (uint32_t)at & 0xfffffffc; } while (0)
		st[m++] = 0x44002f80; st[m++] = 0x1100; st[m++] = cfg[0]; st[m++] = 0x1081;
		PPP(0);
		st[m++] = 0x20020001; st[m++] = 0x1280; st[m++] = 0x1200; st[m++] = 0x48002f80; st[m++] = 0x1180;
		for (k = 1; k + 1 < nb; k++)
			PPP(k);
		st[m++] = 0x6e440600; st[m++] = cfg[10]; st[m++] = 0; st[m++] = 0; st[m++] = 0xaa;
		PPP(nb - 1);
		st[m++] = 0xc0000000;
#undef PPP
		memcpy(g3 + 0x6c000 / 4, st, 4 * m);
	}

	fla.free_list_gpu_addr = flva;
	fla.initial_num_pages = fla.max_num_pages = 128;
	fla.vm_context_handle = vm.handle;
	if (ioctl(fd, DRM_IOCTL_PVR_CREATE_FREE_LIST, &fla))
		DIE("CREATE_FREE_LIST: %s", strerror(errno));
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
	if (ioctl(fd, DRM_IOCTL_PVR_CREATE_HWRT_DATASET, &hw))
		DIE("CREATE_HWRT_DATASET: %s", strerror(errno));
	ctx.type = DRM_PVR_CTX_TYPE_RENDER;
	ctx.vm_context_handle = vm.handle;
	if (ioctl(fd, DRM_IOCTL_PVR_CREATE_CONTEXT, &ctx))
		DIE("CREATE_CONTEXT: %s", strerror(errno));

	for (i = 0; i < 64 * 64; i++)
		g3[0x50000 / 4 + i] = 0x5a5a5a5a;
	{
		uint64_t geom[5] = { 40, tva, 0, 2 | (0x20000ULL << 32), 0 };
		uint8_t frag[376] = { 0 };
		uint32_t *f32p = (uint32_t *)(frag + 336);
		uint64_t *bg = (uint64_t *)(frag + 256);
		struct drm_pvr_job jobs[2] = { 0 };
		struct drm_pvr_sync_op op;
		struct drm_pvr_ioctl_submit_jobs_args sub = { 0 };
		struct drm_syncobj_create sc = { 0 };
		struct drm_syncobj_wait sw = { 0 };
		struct timespec t1;

		*(uint32_t *)frag = 376;
		bg[0] = (0x1480ULL << 32) | 0x1400;
		bg[1] = 0x14c0;
		bg[2] = 0x0080000000800000ULL;
		f32p[1] = 0x3f800000;
		f32p[2] = 0x300;
		f32p[3] = 0xf;
		f32p[4] = 0x0023000f;
		f32p[5] = 0x402;
		f32p[7] = 0x1540;
		ioctl(fd, DRM_IOCTL_SYNCOBJ_CREATE, &sc);
		op = (struct drm_pvr_sync_op){ .handle = sc.handle, .flags = DRM_PVR_SYNC_OP_FLAG_SIGNAL };
		for (i = 0; i < 2; i++) {
			jobs[i].type = i ? DRM_PVR_JOB_TYPE_FRAGMENT : DRM_PVR_JOB_TYPE_GEOMETRY;
			jobs[i].context_handle = ctx.handle;
			jobs[i].cmd_stream = i ? (uintptr_t)frag : (uintptr_t)geom;
			jobs[i].cmd_stream_len = i ? 376 : 40;
			jobs[i].hwrt.set_handle = hw.handle;
			jobs[i].hwrt.data_index = 0;
		}
		jobs[1].sync_ops = (struct drm_pvr_obj_array){ .stride = sizeof(op), .count = 1, .array = (uintptr_t)&op };
		sub.jobs = (struct drm_pvr_obj_array){ .stride = sizeof(jobs[0]), .count = 2, .array = (uintptr_t)jobs };
		if (ioctl(fd, DRM_IOCTL_PVR_SUBMIT_JOBS, &sub))
			DIE("SUBMIT_JOBS: %s", strerror(errno));
		clock_gettime(CLOCK_MONOTONIC, &t1);
		sw.handles = (uintptr_t)&sc.handle;
		sw.count_handles = 1;
		sw.timeout_nsec = t1.tv_sec * 1000000000LL + t1.tv_nsec + 3000000000LL;
		sw.flags = DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT;
		r = ioctl(fd, DRM_IOCTL_SYNCOBJ_WAIT, &sw);
		if (r)
			DIE("SYNCOBJ_WAIT: %s", strerror(errno));
	}
	{
		uint32_t h = 2166136261u, nclear = 0, n5a = 0;

		for (i = 0; i < 64 * 64; i++) {
			h = (h ^ g3[0x50000 / 4 + i]) * 16777619u;
			nclear += g3[0x50000 / 4 + i] == 0xff4080bf;
			n5a += g3[0x50000 / 4 + i] == 0x5a5a5a5a;
		}
		printf("%s: hash %08x, %u clear, %u untouched, pixel (5,58) %08x (32,40) %08x (5,5) %08x\n", argv[2], h,
		       nclear, n5a, g3[0x50000 / 4 + 58 * 64 + 5], g3[0x50000 / 4 + 40 * 64 + 32],
		       g3[0x50000 / 4 + 5 * 64 + 5]);
		if (argc > 3) {
			if (!(f = fopen(argv[3], "wb")) || fwrite(g3 + 0x50000 / 4, 4, 64 * 64, f) != 64 * 64)
				DIE("%s: %s", argv[3], strerror(errno));
			fclose(f);
		}
	}
	return 0;
}
