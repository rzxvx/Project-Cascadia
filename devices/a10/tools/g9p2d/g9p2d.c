// SPDX-License-Identifier: MIT
/*
 * g9p2d -- see g9p2d.h. The GPU side, as docs/gpu.md "2D on the GPU" has it:
 *
 * - one compute job per frame; its CDM stream: a DMA record (a PDS program
 *   loading the two pointers, tile records and arena, into the USC common
 *   store), then one kernel record per tile (one workgroup of 128 threads
 *   running "comp" over one 64 x 32 tile; the tile's index reaches the
 *   kernel as its threadgroup position, written by the kernel's PDS program
 *   from the record's own PDS data), then the terminate word;
 * - the code BO sits at the PDS and the USC heap bases: PDS programs, the
 *   kernel's PDS data, the USC code;
 * - the arena: 1 GiB of GPU VA right below the display, which the kernel
 *   driver maps at the end of the general heap in every VM; frame data and
 *   textures live in BOs mapped into it, so the kernel addresses all of it
 *   with 32-bit offsets from one pointer;
 * - each frame slot's BO is mapped twice: in the arena (what the kernel
 *   reads) and in the PDS heap (the CDM stream and the DMA programs' data,
 *   whose addresses the CDM takes as PDS-heap offsets).
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
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

#include "g9p2d.h"

#define TILE_W		64
#define TILE_H		32
#define WG_THREADS	128	/* g9p2d-shaders.py's WG */

#define ARENA_SIZE	(1ull << 30)	/* below the display */
#define SLOTS		2
#define FRAME_SIZE	(8u << 20)
#define CHUNK_SIZE	(32u << 20)
#define TEX_ALIGN	256u

/* the code BO, at the PDS and the USC heap bases */
#define CODE_SIZE	0x10000
#define CODE_HALT	0x2000
#define CODE_DMA_PDS	0x2100
#define CODE_KER_PDS	0x2200
#define CODE_FENCE_PDS	0x2380		/* the end-of-job fence: DOUTC */
#define CODE_FENCE_DATA	0x23c0
#define CODE_USC	0x4000

/* a frame slot's BO */
#define F_ENV		0x000000	/* struct env per pass */
#define F_STREAM	0x001000	/* CDM stream: 8 words per tile */
#define F_DMA		0x090000	/* PDS data: the DMA's, then 32 bytes per tile */
#define F_TAB		0x0d0000	/* {u64 -> tile records, u64 arena} */
#define F_TILE		0x0f0000	/* tile records, uint4 */
#define F_CDMSTATE	0x110000	/* the CDM context state buffer */
#define F_OPS		0x120000	/* struct op, 128 bytes */
#define F_LIST		0x320000	/* u32 op indices */
#define MAX_PASS	(0x1000 / 32)
/* tiles per frame: the stream also holds the DMA record, a fence per pass, the tail */
#define MAX_REC		((F_DMA - F_STREAM) / 32 - 3 * MAX_PASS)
#define MAX_OPS		((F_LIST - F_OPS) / 128)
#define MAX_LIST	((FRAME_SIZE - F_LIST) / 4)

struct op {
	float s[4], t[4], uv[4], color[4];
	uint32_t tex, tstride, tw, th;
	uint32_t mode, pad[11];		/* pad[0..1]: the bounding box (CPU only) */
};

_Static_assert(sizeof(struct op) == 128, "struct op: the kernel reads 128-byte ops");

struct env {
	uint32_t ops, list, target, stride, width, height, rgba, pad;
};

struct tex {
	uint32_t aoff, size, w, h;	/* aoff 0: free slot */
	uint32_t *cpu;
	int chunk, dead;		/* dead: freed, waiting for the GPU */
};

struct chunk {
	uint32_t aoff, size, handle;
	uint8_t *cpu;
	struct { uint32_t off, size; } *free;	/* sorted, coalesced */
	int nfree, capfree;
};

/* a pass: one target, the quads drawn into it */
struct pass {
	int tex;			/* 0: the display */
	int first_op, nops;
};

static struct {
	int fd, on;
	uint32_t vm, ctx;
	uint64_t pds, usc, arena, screen_va;
	int width, height, stride;	/* the display, stride in pixels */
	uint32_t temps;
	int nstream;			/* CDM records written (8 words each) */
	uint32_t code_h, *code;
	struct {
		uint32_t handle, syncobj;
		uint8_t *cpu;
		uint64_t pds_va;	/* its PDS-heap mapping */
		uint32_t aoff;		/* its arena offset */
		int busy;
		struct timespec submitted;
	} slot[SLOTS];
	int cur;
	struct chunk *chunks;
	int nchunks;
	uint32_t next_chunk_aoff;
	struct tex *tex;
	int ntex;
	/* the frame being built */
	struct op *ops;
	int nops, dropped;
	struct pass pass[MAX_PASS];
	int npass;
	float color[4];
	int mode;
	uint32_t *count;		/* per tile, while binning */
	size_t ncount;
	int stat_quads, stat_tiles, stat_entries, stat_dropped;
	double gpu_us;
	int *pending_free[SLOTS];
	int npending[SLOTS], cappending[SLOTS];
} G;

static uint64_t ts_us(const struct timespec *t)
{
	return t->tv_sec * 1000000ull + t->tv_nsec / 1000;
}

static int bo_new(uint64_t size, uint32_t *handle, void **cpu)
{
	struct drm_pvr_ioctl_create_bo_args c = { .size = size, .flags = DRM_PVR_BO_ALLOW_CPU_USERSPACE_ACCESS };
	struct drm_pvr_ioctl_get_bo_mmap_offset_args o = { 0 };
	void *p;

	if (ioctl(G.fd, DRM_IOCTL_PVR_CREATE_BO, &c))
		return -errno;
	o.handle = c.handle;
	if (ioctl(G.fd, DRM_IOCTL_PVR_GET_BO_MMAP_OFFSET, &o))
		return -errno;
	p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, G.fd, o.offset);
	if (p == MAP_FAILED)
		return -errno;
	*handle = c.handle;
	*cpu = p;
	return 0;
}

static int vm_map(uint32_t bo, uint64_t va, uint64_t size)
{
	struct drm_pvr_ioctl_vm_map_args m = { .vm_context_handle = G.vm, .handle = bo,
					       .device_addr = va, .size = size };

	return ioctl(G.fd, DRM_IOCTL_PVR_VM_MAP, &m) ? -errno : 0;
}

static uint32_t be32(const char *path)
{
	unsigned char b[4] = { 0 };
	FILE *f = fopen(path, "rb");

	if (f) {
		if (fread(b, 1, 4, f) != 4)
			b[0] = b[1] = b[2] = b[3] = 0;
		fclose(f);
	}
	return (uint32_t)b[0] << 24 | b[1] << 16 | b[2] << 8 | b[3];
}

/* the display's geometry: /chosen/framebuffer (simpledrm's) */
static int screen_geometry(void)
{
	DIR *d = opendir("/proc/device-tree/chosen");
	struct dirent *e;
	char p[512];

	if (!d)
		return -ENOENT;
	while ((e = readdir(d)))
		if (!strncmp(e->d_name, "framebuffer", 11)) {
			snprintf(p, sizeof(p), "/proc/device-tree/chosen/%s/width", e->d_name);
			G.width = be32(p);
			snprintf(p, sizeof(p), "/proc/device-tree/chosen/%s/height", e->d_name);
			G.height = be32(p);
			snprintf(p, sizeof(p), "/proc/device-tree/chosen/%s/stride", e->d_name);
			G.stride = be32(p) / 4;
			break;
		}
	closedir(d);
	return G.width && G.height && G.stride ? 0 : -ENOENT;
}

/* a kernel of the G9PT file: "G9PT", n, n x {name[24], off, size}, code */
static int load_code(const char *file)
{
	static uint8_t buf[1 << 16];
	FILE *f = fopen(file, "rb");
	uint32_t n, i, got = 0;
	size_t len;

	if (!f)
		return -errno;
	len = fread(buf, 1, sizeof(buf), f);
	fclose(f);
	if (len < 8 || memcmp(buf, "G9PT", 4))
		return -EINVAL;
	memcpy(&n, buf + 4, 4);
	for (i = 0; i < n && 8 + 32 * (i + 1) <= len; i++) {
		const char *name = (const char *)buf + 8 + 32 * i;
		uint32_t off, sz;

		memcpy(&off, buf + 8 + 32 * i + 24, 4);
		memcpy(&sz, buf + 8 + 32 * i + 28, 4);
		if (off + sz > len)
			return -EINVAL;
		if (!strncmp(name, "comp", 24) && CODE_USC + sz <= CODE_SIZE) {
			memcpy((uint8_t *)G.code + CODE_USC, buf + off, sz);
			got |= 1;
		} else if (!strncmp(name, "comp.temps", 24) && sz == 4) {
			memcpy(&G.temps, buf + off, 4);
			got |= 2;
		}
	}
	return got == 3 ? 0 : -EINVAL;
}

int g9p2d_open(void)
{
	static const uint32_t dma_pds[] = { 0xf0001000, 0xf4001808, 0xd3000000 };
	/*
	 * Apple's kernel PDS program (G5), but the shared phase's DOUTW takes
	 * data dwords 4-5 (the tile index) instead of the hardware's workgroup
	 * ID: 0xf0000811 = DOUTW, control word dword 2, source const64 #2
	 */
	static const uint32_t ker_pds[] = { 0xc2c00003, 0xf0000811, 0xd3000000,
					    0xf0000e01, 0xfc000002, 0xd3000000 };
	struct drm_pvr_ioctl_create_vm_context_args vm = { 0 };
	struct drm_pvr_ioctl_dev_query_args q = { 0 };
	struct drm_pvr_dev_query_heap_info hi = { 0 };
	struct drm_pvr_heap heaps[DRM_PVR_HEAP_COUNT];
	struct drm_pvr_ioctl_create_context_args ctx = { 0 };
	uint64_t state[1 + 7] = { 0 }, gen_end;
	const char *file = getenv("G9P2D_SHADERS");
	int i, err;

	if (G.on)
		return 0;
	if ((err = screen_geometry()))
		return err;
	G.fd = open(getenv("G9P2D_DEVICE") ? getenv("G9P2D_DEVICE") : "/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
	if (G.fd < 0)
		return -errno;
	q.type = DRM_PVR_DEV_QUERY_HEAP_INFO_GET;
	q.pointer = (uintptr_t)&hi;
	q.size = sizeof(hi);
	hi.heaps.stride = sizeof(heaps[0]);
	hi.heaps.count = DRM_PVR_HEAP_COUNT;
	hi.heaps.array = (uintptr_t)heaps;
	if (ioctl(G.fd, DRM_IOCTL_PVR_DEV_QUERY, &q))
		return -errno;
	G.pds = heaps[DRM_PVR_HEAP_PDS_CODE_DATA].base;
	G.usc = heaps[DRM_PVR_HEAP_USC_CODE].base;
	/* the display sits right after the general heap userspace is given */
	gen_end = heaps[DRM_PVR_HEAP_GENERAL].base + heaps[DRM_PVR_HEAP_GENERAL].size;
	G.screen_va = gen_end;
	G.arena = gen_end - ARENA_SIZE;
	if (ioctl(G.fd, DRM_IOCTL_PVR_CREATE_VM_CONTEXT, &vm))
		return -errno;
	G.vm = vm.handle;

	if ((err = bo_new(CODE_SIZE, &G.code_h, (void **)&G.code)))
		return err;
	for (i = 0; i < CODE_USC / 4; i++)
		G.code[i] = 0xd3000000;		/* PDS HALT */
	memset(G.code + CODE_HALT / 4 + 1, 0, 0x1c);
	memcpy(G.code + CODE_DMA_PDS / 4, dma_pds, sizeof(dma_pds));
	memcpy(G.code + CODE_KER_PDS / 4, ker_pds, sizeof(ker_pds));
	if ((err = load_code(file ? file : "/usr/local/lib/g9p2d/g9p2d.bin")))
		return err;
	{
		/* Apple's end of a compute job (iPadOS capture): fence records whose
		 * PDS program, in its instance phase, issues DOUTC (a fence back to
		 * the CDM) */
		static const uint32_t fence_pds[] = { 0xc2c00002, 0xd3000000, 0xf0000005, 0xd3000000 };

		memcpy(G.code + CODE_FENCE_PDS / 4, fence_pds, sizeof(fence_pds));
		memset(G.code + CODE_FENCE_DATA / 4, 0, 16);
	}
	G.temps = (G.temps + 3) & ~3u;
	if ((err = vm_map(G.code_h, G.pds, CODE_SIZE)) || (err = vm_map(G.code_h, G.usc, CODE_SIZE)))
		return err;

	for (i = 0; i < SLOTS; i++) {
		struct drm_syncobj_create sc = { .flags = DRM_SYNCOBJ_CREATE_SIGNALED };

		if ((err = bo_new(FRAME_SIZE, &G.slot[i].handle, (void **)&G.slot[i].cpu)))
			return err;
		G.slot[i].aoff = i * FRAME_SIZE;
		G.slot[i].pds_va = G.pds + 0x100000 + (uint64_t)i * FRAME_SIZE;
		if ((err = vm_map(G.slot[i].handle, G.arena + G.slot[i].aoff, FRAME_SIZE)) ||
		    (err = vm_map(G.slot[i].handle, G.slot[i].pds_va, FRAME_SIZE)))
			return err;
		if (ioctl(G.fd, DRM_IOCTL_SYNCOBJ_CREATE, &sc))
			return -errno;
		G.slot[i].syncobj = sc.handle;
	}
	G.next_chunk_aoff = SLOTS * FRAME_SIZE;

	state[0] = sizeof(state);
	state[1] = ((uint64_t)(CODE_HALT + 0x10) << 32) | CODE_HALT;	/* context PDS0 */
	state[3] = state[1];						/* terminate PDS */
	ctx.type = DRM_PVR_CTX_TYPE_COMPUTE;
	ctx.vm_context_handle = G.vm;
	ctx.static_context_state = (uintptr_t)state;
	ctx.static_context_state_len = sizeof(state);
	if (ioctl(G.fd, DRM_IOCTL_PVR_CREATE_CONTEXT, &ctx))
		return -errno;
	G.ctx = ctx.handle;

	G.ntex = 1;			/* id 0: no texture */
	G.tex = calloc(64, sizeof(*G.tex));
	G.on = 1;
	g9p2d_begin();
	return 0;
}

void g9p2d_close(void)
{
	if (!G.on)
		return;
	g9p2d_finish();
	close(G.fd);
	G.on = 0;
}

int g9p2d_width(void)
{
	return G.width;
}

int g9p2d_height(void)
{
	return G.height;
}

/* ---- textures: first fit in 32 MiB chunks of the arena ------------------ */

static int chunk_new(uint32_t size)
{
	struct chunk *c;
	int err;

	size = size < CHUNK_SIZE ? CHUNK_SIZE : (size + 0xffff) & ~0xffffu;
	if ((uint64_t)G.next_chunk_aoff + size > ARENA_SIZE)
		return -ENOMEM;
	G.chunks = realloc(G.chunks, (G.nchunks + 1) * sizeof(*G.chunks));
	c = &G.chunks[G.nchunks];
	memset(c, 0, sizeof(*c));
	if ((err = bo_new(size, &c->handle, (void **)&c->cpu)))
		return err;
	c->aoff = G.next_chunk_aoff;
	c->size = size;
	if ((err = vm_map(c->handle, G.arena + c->aoff, size)))
		return err;
	G.next_chunk_aoff += size;
	c->capfree = 16;
	c->free = malloc(c->capfree * sizeof(*c->free));
	c->free[0].off = 0;
	c->free[0].size = size;
	c->nfree = 1;
	return G.nchunks++;
}

static int chunk_alloc(struct chunk *c, uint32_t size, uint32_t *off)
{
	int i;

	for (i = 0; i < c->nfree; i++)
		if (c->free[i].size >= size) {
			*off = c->free[i].off;
			c->free[i].off += size;
			c->free[i].size -= size;
			if (!c->free[i].size) {
				memmove(&c->free[i], &c->free[i + 1], (c->nfree - i - 1) * sizeof(*c->free));
				c->nfree--;
			}
			return 0;
		}
	return -ENOMEM;
}

static void chunk_release(struct chunk *c, uint32_t off, uint32_t size)
{
	int i = 0;

	while (i < c->nfree && c->free[i].off < off)
		i++;
	if (i > 0 && c->free[i - 1].off + c->free[i - 1].size == off) {
		c->free[i - 1].size += size;
		if (i < c->nfree && off + size == c->free[i].off) {
			c->free[i - 1].size += c->free[i].size;
			memmove(&c->free[i], &c->free[i + 1], (c->nfree - i - 1) * sizeof(*c->free));
			c->nfree--;
		}
		return;
	}
	if (i < c->nfree && off + size == c->free[i].off) {
		c->free[i].off = off;
		c->free[i].size += size;
		return;
	}
	if (c->nfree == c->capfree) {
		c->capfree *= 2;
		c->free = realloc(c->free, c->capfree * sizeof(*c->free));
	}
	memmove(&c->free[i + 1], &c->free[i], (c->nfree - i) * sizeof(*c->free));
	c->free[i].off = off;
	c->free[i].size = size;
	c->nfree++;
}

int g9p2d_texture(const void *rgba, int w, int h)
{
	uint32_t size, off = 0;
	struct tex *t;
	int id, ch, err;

	if (!G.on || w <= 0 || h <= 0 || w > 8192 || h > 8192)
		return -EINVAL;
	size = ((uint32_t)w * h * 4 + TEX_ALIGN - 1) & ~(TEX_ALIGN - 1);
	for (ch = 0; ch < G.nchunks; ch++)
		if (!chunk_alloc(&G.chunks[ch], size, &off))
			break;
	if (ch == G.nchunks) {
		if ((ch = chunk_new(size)) < 0)
			return ch;
		if ((err = chunk_alloc(&G.chunks[ch], size, &off)))
			return err;
	}
	for (id = 1; id < G.ntex; id++)
		if (!G.tex[id].aoff)
			break;
	if (id == G.ntex) {
		if ((G.ntex & (G.ntex - 1)) == 0 && G.ntex >= 64)
			G.tex = realloc(G.tex, 2 * G.ntex * sizeof(*G.tex));
		G.ntex++;
	}
	t = &G.tex[id];
	t->aoff = G.chunks[ch].aoff + off;
	t->size = size;
	t->w = w;
	t->h = h;
	t->cpu = (uint32_t *)(G.chunks[ch].cpu + off);
	t->chunk = ch;
	t->dead = 0;
	if (rgba)
		memcpy(t->cpu, rgba, (size_t)w * h * 4);
	else
		memset(t->cpu, 0, (size_t)w * h * 4);
	return id;
}

int g9p2d_texture_update(int id, const void *rgba)
{
	if (id <= 0 || id >= G.ntex || !G.tex[id].aoff || G.tex[id].dead)
		return -EINVAL;
	memcpy(G.tex[id].cpu, rgba, (size_t)G.tex[id].w * G.tex[id].h * 4);
	return 0;
}

uint32_t *g9p2d_texture_pixels(int id)
{
	return id > 0 && id < G.ntex && G.tex[id].aoff ? G.tex[id].cpu : NULL;
}

static void tex_release(int id)
{
	struct tex *t = &G.tex[id];

	chunk_release(&G.chunks[t->chunk], t->aoff - G.chunks[t->chunk].aoff, t->size);
	t->aoff = 0;
}

void g9p2d_texture_free(int id)
{
	int s = G.cur;

	if (id <= 0 || id >= G.ntex || !G.tex[id].aoff || G.tex[id].dead)
		return;
	/* the frame being built and the one in flight may use it: release it
	 * when this slot comes round again (both are done by then) */
	G.tex[id].dead = 1;
	if (G.npending[s] == G.cappending[s]) {
		G.cappending[s] = G.cappending[s] ? 2 * G.cappending[s] : 64;
		G.pending_free[s] = realloc(G.pending_free[s], G.cappending[s] * sizeof(int));
	}
	G.pending_free[s][G.npending[s]++] = id;
}

/* ---- frames ------------------------------------------------------------- */

static int slot_wait(int s)
{
	struct drm_syncobj_wait w = { 0 };
	struct timespec now;
	int r;

	if (!G.slot[s].busy)
		return 0;
	clock_gettime(CLOCK_MONOTONIC, &now);
	w.handles = (uintptr_t)&G.slot[s].syncobj;
	w.count_handles = 1;
	w.timeout_nsec = now.tv_sec * 1000000000ll + now.tv_nsec + 2000000000ll;
	w.flags = DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT;
	r = ioctl(G.fd, DRM_IOCTL_SYNCOBJ_WAIT, &w) ? -errno : 0;
	clock_gettime(CLOCK_MONOTONIC, &now);
	G.gpu_us = ts_us(&now) - ts_us(&G.slot[s].submitted);
	G.slot[s].busy = 0;
	return r;
}

void g9p2d_begin(void)
{
	int s = G.cur, i;

	slot_wait(s);
	for (i = 0; i < G.npending[s]; i++)
		tex_release(G.pending_free[s][i]);
	G.npending[s] = 0;
	G.ops = (struct op *)(G.slot[s].cpu + F_OPS);
	G.nops = 0;
	G.dropped = 0;
	G.npass = 1;
	G.pass[0].tex = 0;
	G.pass[0].first_op = 0;
	G.pass[0].nops = 0;
	G.color[0] = G.color[1] = G.color[2] = G.color[3] = 1;
	G.mode = G9P2D_BLEND;
}

void g9p2d_target(int id)
{
	struct pass *p = &G.pass[G.npass - 1];

	if (id < 0 || id >= G.ntex || (id && !G.tex[id].aoff))
		id = 0;
	if (p->tex == id)
		return;
	if (p->nops) {
		if (G.npass == MAX_PASS)
			return;
		p = &G.pass[G.npass++];
		p->first_op = G.nops;
		p->nops = 0;
	}
	p->tex = id;
}

void g9p2d_color(float r, float g, float b, float a)
{
	G.color[0] = r;
	G.color[1] = g;
	G.color[2] = b;
	G.color[3] = a;
}

void g9p2d_blend(int mode)
{
	G.mode = mode >= G9P2D_NONE && mode <= G9P2D_MOD ? mode : G9P2D_BLEND;
}

void g9p2d_quad4(int tex, const float xy[8], const float uv[8])
{
	float e1x = xy[2] - xy[0], e1y = xy[3] - xy[1];	/* TL -> TR: s */
	float e2x = xy[6] - xy[0], e2y = xy[7] - xy[1];	/* TL -> BL: t */
	float det = e1x * e2y - e1y * e2x;
	struct op *o;

	if (fabsf(det) < 1e-6f)
		return;
	if (G.nops == MAX_OPS) {
		G.dropped++;
		return;
	}
	o = &G.ops[G.nops];
	memset(o, 0, sizeof(*o));
	o->s[0] = e2y / det;
	o->s[1] = -e2x / det;
	o->s[2] = (xy[1] * e2x - xy[0] * e2y) / det;
	o->t[0] = -e1y / det;
	o->t[1] = e1x / det;
	o->t[2] = (xy[0] * e1y - xy[1] * e1x) / det;
	memcpy(o->color, G.color, sizeof(o->color));
	o->mode = G.mode;
	if (tex > 0 && tex < G.ntex && G.tex[tex].aoff) {
		struct tex *t = &G.tex[tex];

		o->tex = t->aoff;
		o->tstride = t->w;
		o->tw = t->w;
		o->th = t->h;
		o->uv[0] = uv[0] * t->w;
		o->uv[1] = uv[1] * t->h;
		o->uv[2] = (uv[2] - uv[0]) * t->w;
		o->uv[3] = (uv[7] - uv[1]) * t->h;
	}
	{
		float x0 = xy[0], x1 = xy[0], y0 = xy[1], y1 = xy[1];
		int k;

		for (k = 1; k < 4; k++) {
			x0 = fminf(x0, xy[2 * k]);
			x1 = fmaxf(x1, xy[2 * k]);
			y0 = fminf(y0, xy[2 * k + 1]);
			y1 = fmaxf(y1, xy[2 * k + 1]);
		}
		/* kept in the pad words, the GPU ignores them */
		o->pad[0] = (uint32_t)(int16_t)floorf(fmaxf(fminf(x0, 32767), -32768)) |
			    (uint32_t)(int16_t)floorf(fmaxf(fminf(y0, 32767), -32768)) << 16;
		o->pad[1] = (uint32_t)(int16_t)ceilf(fmaxf(fminf(x1, 32767), -32768)) |
			    (uint32_t)(int16_t)ceilf(fmaxf(fminf(y1, 32767), -32768)) << 16;
	}
	G.nops++;
	G.pass[G.npass - 1].nops++;
}

void g9p2d_quad_uv(int tex, float x, float y, float w, float h, float u0, float v0, float u1, float v1)
{
	float xy[8] = { x, y, x + w, y, x + w, y + h, x, y + h };
	float uv[8] = { u0, v0, u1, v0, u1, v1, u0, v1 };

	g9p2d_quad4(tex, xy, uv);
}

void g9p2d_quad(int tex, float x, float y, float w, float h)
{
	g9p2d_quad_uv(tex, x, y, w, h, 0, 0, 1, 1);
}

void g9p2d_fill4(const float xy[8])
{
	static const float uv[8] = { 0, 0, 1, 0, 1, 1, 0, 1 };

	g9p2d_quad4(0, xy, uv);
}

void g9p2d_fill(float x, float y, float w, float h)
{
	g9p2d_quad_uv(0, x, y, w, h, 0, 0, 1, 1);
}

/* a fence record: Apple's end of a compute job (the PDS program's DOUTC) */
static void fence_rec(uint32_t *rec)
{
	rec[0] = 0xc0000003;
	rec[1] = CODE_FENCE_DATA | 0x4;	/* sd_type PDS */
	rec[2] = CODE_FENCE_PDS;
	rec[3] = rec[4] = rec[5] = 0;
	rec[6] = 0x40000000;
	rec[7] = 0;
}

/* the quads of one pass into tile lists and CDM records; returns records added */
static int build_pass(int s, int pi, int *nrec, uint32_t *nlist)
{
	struct pass *p = &G.pass[pi];
	uint8_t *f = G.slot[s].cpu;
	uint32_t aoff = G.slot[s].aoff;
	struct env *e = (struct env *)(f + F_ENV) + pi;
	int tw, th, tx, ty, i, w, h, stride, added = 0;
	uint32_t *list = (uint32_t *)(f + F_LIST), base = *nlist, total = 0;

	if (p->tex) {
		w = G.tex[p->tex].w;
		h = G.tex[p->tex].h;
		stride = w;
		e->target = G.tex[p->tex].aoff;
		e->rgba = 1;
	} else {
		w = G.width;
		h = G.height;
		stride = G.stride;
		e->target = (uint32_t)(G.screen_va - G.arena);
		e->rgba = 0;
	}
	e->ops = aoff + F_OPS;
	e->list = aoff + F_LIST;
	e->stride = stride;
	e->width = w;
	e->height = h;
	tw = (w + TILE_W - 1) / TILE_W;
	th = (h + TILE_H - 1) / TILE_H;
	if ((size_t)tw * th > G.ncount) {
		G.ncount = (size_t)tw * th;
		G.count = realloc(G.count, G.ncount * sizeof(*G.count));
	}
	memset(G.count, 0, (size_t)tw * th * sizeof(*G.count));

	/* count, then offsets, then fill (in call order) */
	for (int pass2 = 0; pass2 < 2; pass2++) {
		if (pass2) {
			uint32_t run = base;

			for (i = 0; i < tw * th; i++) {
				uint32_t c = G.count[i];

				G.count[i] = run;
				run += c;
			}
			if (run > MAX_LIST)
				return -ENOSPC;
			total = run - base;
		}
		for (i = p->first_op; i < p->first_op + p->nops; i++) {
			struct op *o = &G.ops[i];
			int x0 = (int16_t)(o->pad[0] & 0xffff), y0 = (int16_t)(o->pad[0] >> 16);
			int x1 = (int16_t)(o->pad[1] & 0xffff), y1 = (int16_t)(o->pad[1] >> 16);

			x0 = x0 < 0 ? 0 : x0;
			y0 = y0 < 0 ? 0 : y0;
			x1 = x1 > w ? w : x1;
			y1 = y1 > h ? h : y1;
			if (x0 >= x1 || y0 >= y1)
				continue;
			for (ty = y0 / TILE_H; ty <= (y1 - 1) / TILE_H; ty++)
				for (tx = x0 / TILE_W; tx <= (x1 - 1) / TILE_W; tx++) {
					uint32_t *c = &G.count[ty * tw + tx];

					if (pass2)
						list[(*c)++] = i;
					else
						(*c)++;
				}
		}
	}
	/* records: count[] now holds each tile's end */
	for (ty = 0; ty < th; ty++)
		for (tx = 0; tx < tw; tx++) {
			uint32_t end = G.count[ty * tw + tx];
			uint32_t start = ty * tw + tx ? G.count[ty * tw + tx - 1] : base;
			uint32_t *tile, *kd, *rec;
			int r = *nrec;

			if (end == start)
				continue;
			if (r == MAX_REC)
				return -ENOSPC;
			tile = (uint32_t *)(f + F_TILE) + 4 * r;
			tile[0] = (tx * TILE_W) | (ty * TILE_H) << 16;
			tile[1] = start;
			tile[2] = end - start;
			tile[3] = aoff + F_ENV + 32 * pi;
			/* the kernel's PDS data: DOUTU, the DOUTW controls, the tile index */
			kd = (uint32_t *)(f + F_DMA + 32 * (r + 1));
			kd[0] = CODE_USC;			/* DOUTU: EXE_OFF */
			kd[1] = (G.temps / 4) << 3;		/* TEMPS, 4-register units */
			kd[2] = 0x90000000;			/* shared phase: common store */
			kd[3] = 0x80000000;			/* instance phase: unified store */
			kd[4] = r;
			kd[5] = kd[6] = kd[7] = 0;
			rec = (uint32_t *)(f + F_STREAM) + 8 * G.nstream++;
			/* fence, PDS data 32 bytes, 1 PDS temp, temps */
			rec[0] = 0x107 | (G.temps / 4) << 12;
			rec[1] = (uint32_t)(G.slot[s].pds_va - G.pds + F_DMA + 32 * (r + 1));
			rec[2] = CODE_KER_PDS;
			rec[3] = rec[4] = rec[5] = 0;			/* one workgroup */
			rec[6] = 1;
			rec[7] = (WG_THREADS - 1) << 20;		/* its size */
			(*nrec)++;
			added++;
		}
	*nlist += total;
	return added;
}

/* the DMA record that starts every job: {tile records, arena} -> common store 0..3 */
static void dma_rec(int s, uint32_t *rec)
{
	uint8_t *f = G.slot[s].cpu;
	uint64_t *tab = (uint64_t *)(f + F_TAB), *dma = (uint64_t *)(f + F_DMA);

	tab[0] = G.arena + G.slot[s].aoff + F_TILE;
	tab[1] = G.arena;
	dma[0] = G.arena + G.slot[s].aoff + F_TAB;
	dma[1] = dma[0] + 8;
	dma[2] = 0x10000000 | 2;			/* 2 dwords -> common store 0 */
	dma[3] = 0x90000000 | (2 << 13) | 2;		/* 2 dwords -> 2, last */
	rec[0] = 0x4;					/* PDS data 32 bytes */
	rec[1] = (uint32_t)(G.slot[s].pds_va - G.pds + F_DMA) | 0x2;	/* usc_common_shared */
	rec[2] = CODE_DMA_PDS;
	rec[3] = rec[4] = rec[5] = 0;
	rec[6] = 0x40000001;
	rec[7] = 0;
}

/*
 * One compute job per pass: a fence between the kernels of two passes waits
 * for the first but leaves its writes in the clusters' caches (the second
 * pass read half of a render target as zeros); the end of a job writes them
 * back. The jobs of a frame go in one submit, in order; the last one signals.
 */
int g9p2d_end(void)
{
	int s = G.cur, nrec = 0, pi, r = 0, njobs = 0;
	uint32_t nlist = 0, *st;
	uint8_t *f = G.slot[s].cpu;
	struct drm_pvr_job jobs[MAX_PASS];
	uint64_t cmd[MAX_PASS][5];
	struct drm_pvr_sync_op op;
	struct drm_pvr_ioctl_submit_jobs_args sub = { 0 };

	if (!G.on)
		return -ENODEV;
	G.nstream = 0;
	for (pi = 0; pi < G.npass; pi++) {
		int first = G.nstream;

		G.nstream++;				/* the DMA record */
		if ((r = build_pass(s, pi, &nrec, &nlist)) <= 0) {
			if (r < 0)
				G.dropped += G.pass[pi].nops;
			G.nstream = first;
			continue;
		}
		st = (uint32_t *)(f + F_STREAM);
		dma_rec(s, st + 8 * first);
		fence_rec(st + 8 * G.nstream++);	/* the job ends after its tiles */
		st[8 * G.nstream++] = 0x80000000;	/* terminate */
		cmd[njobs][0] = 36;			/* u32 length, u32 0 */
		cmd[njobs][1] = 0;			/* tpu_border_colour_table */
		cmd[njobs][2] = G.slot[s].pds_va + F_STREAM + 32 * first;	/* cdm_ctrl_stream_base */
		cmd[njobs][3] = G.arena + G.slot[s].aoff + F_CDMSTATE;	/* cdm_context_state_base_addr */
		cmd[njobs][4] = 0;
		memset(&jobs[njobs], 0, sizeof(jobs[0]));
		jobs[njobs].type = DRM_PVR_JOB_TYPE_COMPUTE;
		jobs[njobs].context_handle = G.ctx;
		jobs[njobs].cmd_stream = (uintptr_t)cmd[njobs];
		jobs[njobs].cmd_stream_len = 36;
		njobs++;
	}
	G.stat_quads = G.nops;
	G.stat_tiles = nrec;
	G.stat_entries = nlist;
	G.stat_dropped = G.dropped;
	if (!njobs) {
		g9p2d_begin();
		return 0;
	}
	op = (struct drm_pvr_sync_op){ .handle = G.slot[s].syncobj, .flags = DRM_PVR_SYNC_OP_FLAG_SIGNAL };
	jobs[njobs - 1].sync_ops = (struct drm_pvr_obj_array){ .stride = sizeof(op), .count = 1,
								.array = (uintptr_t)&op };
	sub.jobs = (struct drm_pvr_obj_array){ .stride = sizeof(jobs[0]), .count = njobs, .array = (uintptr_t)jobs };
	clock_gettime(CLOCK_MONOTONIC, &G.slot[s].submitted);
	r = ioctl(G.fd, DRM_IOCTL_PVR_SUBMIT_JOBS, &sub) ? -errno : 0;
	G.slot[s].busy = !r;
	G.cur = (s + 1) % SLOTS;
	g9p2d_begin();
	return r;
}

int g9p2d_finish(void)
{
	int s, r = 0, e;

	for (s = 0; s < SLOTS; s++)
		if ((e = slot_wait(s)))
			r = e;
	return r;
}

void g9p2d_frame_stats(int *quads, int *tiles, int *entries, int *dropped, double *gpu_us)
{
	if (quads)
		*quads = G.stat_quads;
	if (tiles)
		*tiles = G.stat_tiles;
	if (entries)
		*entries = G.stat_entries;
	if (dropped)
		*dropped = G.stat_dropped;
	if (gpu_us)
		*gpu_us = G.gpu_us;
}
