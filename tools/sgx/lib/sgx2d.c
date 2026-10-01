/* sgx2d: see sgx2d.h.  How each block works is in tools/sgx/rpack.py,
 * rbatch.py and docs/research/p105-gpu.md; this is the frame assembly. */
#include "sgx2d.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define DBG "/sys/kernel/debug/apple-sgx/"

/* EXT window layout: textures and their blocks, the vertex buffer, and the
 * blocks rebuilt every frame */
#define EXT_TEX_END	0x100000
#define EXT_VB		0x100000
#define EXT_VB_END	0x1c0000
#define EXT_FRAME	0x1c0000
#define VTX_FLOATS	8		/* r g b a u v x y */
#define QUAD_BYTES	(6 * VTX_FLOATS * 4)
#define MAX_QUADS	((EXT_VB_END - EXT_VB) / QUAD_BYTES - 1)
#define MAX_TEX		512

enum { T_FULL, T_FULLPROG, T_DELTA, T_DELTAPROG, T_FETCH, T_TEX, T_NUM };
static const char *tmpl_names[T_NUM] = {
	"tmpl_full", "tmpl_fullprog", "tmpl_delta", "tmpl_deltaprog", "tmpl_fetch", "tmpl_tex"
};

struct quad {
	int tex, mode;
	float xy[8], uv[8], rgba[4];
};

static struct {
	int mem, cmd;
	uint32_t kick[3], consts0, consts, idx, idx_count, vdm, vdm_size, ext, ext_size;
	uint32_t fetch_tag, fetch_word, blendprog[SGX2D_NMODES];
	uint32_t tail[8];
	int ntail, w, h;
	uint8_t *tmpl;
	int toff[T_NUM], tsize[T_NUM];
	uint32_t ext_top;
	struct {
		uint32_t data, block[SGX2D_NMODES], deltaprog[SGX2D_NMODES];
		float us, vs;
		int w, h, s;
	} tex[MAX_TEX];
	int ntex, white;
	/* the frame being built */
	int bg, nq, mode;
	float rgba[4];
	struct quad *q;
	float *vb;
	uint8_t *fbuf;		/* EXT_FRAME .. end, staged */
	uint32_t ftop;
	uint32_t *vdmbuf;
} S;

static int put(uint32_t va, const void *p, size_t n)
{
	return pwrite(S.mem, p, n, (off_t)va) == (ssize_t)n ? 0 : -errno;
}

static int cmd(const char *fmt, ...)
{
	char b[128];
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vsnprintf(b, sizeof(b), fmt, ap);
	va_end(ap);
	return write(S.cmd, b, n) == n ? 0 : -errno;
}

static uint32_t p27(uint32_t a)		/* PDS data pointer form */
{
	return 0x10000000 | ((a >> 4) & 0x07ffffff);
}

static uint32_t vdm4(uint32_t tag, uint32_t a)
{
	return tag << 28 | a >> 4;
}

static int load_file(const char *dir, const char *name, uint8_t **buf, size_t *len)
{
	char path[512];
	FILE *f;
	long n;

	snprintf(path, sizeof(path), "%s/%s", dir, name);
	if (!(f = fopen(path, "rb")))
		return -errno;
	fseek(f, 0, SEEK_END);
	n = ftell(f);
	fseek(f, 0, SEEK_SET);
	*buf = malloc(n);
	*len = fread(*buf, 1, n, f);
	fclose(f);
	return *len == (size_t)n ? 0 : -EIO;
}

static int parse_words(const char *p, uint32_t *out, int max)
{
	int n = 0;

	while (n < max && *p) {
		char *e;
		uint32_t v = strtoul(p, &e, 0);

		if (e == p)
			break;
		out[n++] = v;
		p = e;
	}
	return n;
}

int sgx2d_open(const char *dir)
{
	char path[512], line[512], key[64], a[64], b[64], c[64];
	FILE *f;
	size_t len;
	int boot, ret, i;

	if (access(DBG "mem", F_OK))
		system("mount -t debugfs none /sys/kernel/debug 2>/dev/null");
	S.mem = open(DBG "mem", O_RDWR);
	S.cmd = open(DBG "cmd", O_WRONLY);
	if (S.mem < 0 || S.cmd < 0)
		return -errno;
	snprintf(path, sizeof(path), "%s/pack.txt", dir);
	if (!(f = fopen(path, "r")))
		return -errno;
	/* pass 1: mappings (before the boot) and parameters */
	while (fgets(line, sizeof(line), f)) {
		int n = sscanf(line, "%63s %63s %63s %63s", key, a, b, c);

		if (n < 2)
			continue;
		if (!strcmp(key, "map"))
			cmd("map %s %s", a, b);	/* EEXIST on a second open is fine */
		else if (!strcmp(key, "kick"))
			parse_words(line + 4, S.kick, 3);
		else if (!strcmp(key, "screen")) {
			S.w = atoi(a); S.h = atoi(b);
		} else if (!strcmp(key, "consts0"))
			S.consts0 = strtoul(a, 0, 0);
		else if (!strcmp(key, "consts"))
			S.consts = strtoul(a, 0, 0);
		else if (!strcmp(key, "idx")) {
			S.idx = strtoul(a, 0, 0); S.idx_count = strtoul(b, 0, 0);
		} else if (!strcmp(key, "vdm")) {
			S.vdm = strtoul(a, 0, 0); S.vdm_size = strtoul(b, 0, 0);
		} else if (!strcmp(key, "ext")) {
			S.ext = strtoul(a, 0, 0); S.ext_size = strtoul(b, 0, 0);
		} else if (!strcmp(key, "fetch")) {
			S.fetch_tag = strtoul(a, 0, 0); S.fetch_word = strtoul(b, 0, 0);
		} else if (!strcmp(key, "blendprogs"))
			parse_words(line + 10, S.blendprog, SGX2D_NMODES);
		else if (!strcmp(key, "tail"))
			S.ntail = parse_words(line + 4, S.tail, 8);
		else
			for (i = 0; i < T_NUM; i++)
				if (!strcmp(key, tmpl_names[i])) {
					S.toff[i] = atoi(a); S.tsize[i] = atoi(b);
				}
	}
	boot = open(DBG "boot", O_WRONLY);
	if (boot < 0 || write(boot, "1", 1) != 1)
		return -EIO;
	close(boot);
	usleep(200000);
	/* pass 2: images and register pokes (after the boot) */
	rewind(f);
	while (fgets(line, sizeof(line), f)) {
		uint8_t *img;

		if (sscanf(line, "%63s %63s %63s", key, a, b) < 3)
			continue;
		if (!strcmp(key, "img")) {
			if ((ret = load_file(dir, b, &img, &len)))
				return ret;
			ret = put(strtoul(a, 0, 0), img, len);
			free(img);
			if (ret)
				return ret;
		} else if (!strcmp(key, "poke")) {
			snprintf(line, sizeof(line), "peek w %s %s > /dev/null",
				 a + (a[0] == '0' && a[1] == 'x' ? 2 : 0),
				 b + (b[0] == '0' && b[1] == 'x' ? 2 : 0));
			system(line);
		}
	}
	fclose(f);
	if ((ret = load_file(dir, "tmpl.bin", &S.tmpl, &len)))
		return ret;
	S.q = calloc(MAX_QUADS, sizeof(*S.q));
	S.vb = malloc((MAX_QUADS + 1) * QUAD_BYTES);
	S.fbuf = malloc(S.ext_size - EXT_FRAME);
	S.vdmbuf = malloc(S.vdm_size);
	if (!S.q || !S.vb || !S.fbuf || !S.vdmbuf)
		return -ENOMEM;
	{
		uint32_t px[16];

		for (i = 0; i < 16; i++)
			px[i] = 0xffffffff;
		S.white = sgx2d_texture(px, 4, 4);
	}
	return S.white < 0 ? S.white : 0;
}

void sgx2d_close(void)
{
	close(S.mem);
	close(S.cmd);
}

int sgx2d_width(void) { return S.w; }
int sgx2d_height(void) { return S.h; }

static uint32_t ext_alloc(const void *p, uint32_t n, uint32_t align)
{
	uint32_t off = (S.ext_top + align - 1) & ~(align - 1);

	if (off + n > EXT_TEX_END)
		return 0;
	S.ext_top = off + n;
	return put(S.ext + off, p, n) ? 0 : S.ext + off;
}

/* Morton order, y in the even bits (the layout the GL driver uploads) */
static uint32_t twiddle(uint32_t x, uint32_t y)
{
	uint32_t i = 0;
	int b;

	for (b = 0; b < 11; b++)
		i |= ((y >> b) & 1) << (2 * b) | ((x >> b) & 1) << (2 * b + 1);
	return i;
}

static int upload(int id, const uint32_t *src, uint32_t va)
{
	int s = S.tex[id].s, x, y, ret;
	uint32_t *t = calloc(s * s, 4);

	if (!t)
		return -ENOMEM;
	for (y = 0; y < S.tex[id].h; y++)
		for (x = 0; x < S.tex[id].w; x++)
			t[twiddle(x, y)] = src[y * S.tex[id].w + x];
	ret = put(va, t, s * s * 4);
	free(t);
	return ret;
}

int sgx2d_texture(const void *rgba, int w, int h)
{
	uint32_t va, data;
	uint8_t blk[0x40], d[0x20], prog[0x40];
	int s = 4, id = S.ntex, m;

	if (id >= MAX_TEX || w < 1 || h < 1 || w > 1024 || h > 1024)
		return -EINVAL;
	while (s < w || s < h)
		s <<= 1;		/* square, power of two (padded) */
	S.tex[id].w = w; S.tex[id].h = h; S.tex[id].s = s;
	S.tex[id].us = (float)w / s;
	S.tex[id].vs = (float)h / s;
	S.ext_top = (S.ext_top + 0xfff) & ~0xfff;
	va = S.ext + S.ext_top;
	if (S.ext_top + s * s * 4 > EXT_TEX_END)
		return -ENOSPC;
	S.ext_top += s * s * 4;
	if (upload(id, rgba, va))
		return -EIO;
	S.tex[id].data = va;
	for (m = 0; m < SGX2D_NMODES; m++) {	/* a block and a delta per blend mode */
		memcpy(blk, S.tmpl + S.toff[T_TEX], S.tsize[T_TEX]);
		((uint32_t *)blk)[0] = S.blendprog[m];
		((uint32_t *)blk)[5] = 0x0c000000 | (__builtin_ctz(s) << 16) | __builtin_ctz(s);
		((uint32_t *)blk)[6] = va;
		S.tex[id].block[m] = ext_alloc(blk, S.tsize[T_TEX], 0x40);
		memcpy(d, S.tmpl + S.toff[T_DELTA], S.tsize[T_DELTA]);
		((uint32_t *)d)[3] = p27(S.tex[id].block[m]);
		data = ext_alloc(d, S.tsize[T_DELTA], 0x20);
		memcpy(prog, S.tmpl + S.toff[T_DELTAPROG], S.tsize[T_DELTAPROG]);
		((uint32_t *)prog)[0] = data;
		S.tex[id].deltaprog[m] = ext_alloc(prog, S.tsize[T_DELTAPROG], 0x40);
		if (!S.tex[id].block[m] || !data || !S.tex[id].deltaprog[m])
			return -ENOSPC;
	}
	return S.ntex++;
}

int sgx2d_texture_update(int id, const void *rgba)
{
	if (id < 0 || id >= S.ntex)
		return -EINVAL;
	return upload(id, rgba, S.tex[id].data);
}

void sgx2d_begin(int bg)
{
	S.bg = bg;
	S.nq = 0;
	S.mode = SGX2D_BLEND;
	S.rgba[0] = S.rgba[1] = S.rgba[2] = S.rgba[3] = 1;
}

void sgx2d_color(float r, float g, float b, float a)
{
	S.rgba[0] = r; S.rgba[1] = g; S.rgba[2] = b; S.rgba[3] = a;
}

void sgx2d_blend(int mode)
{
	if (mode >= 0 && mode < SGX2D_NMODES)
		S.mode = mode;
}

void sgx2d_quad4(int tex, const float xy[8], const float uv[8])
{
	struct quad *q;

	if (S.nq >= MAX_QUADS || tex < 0 || tex >= S.ntex)
		return;
	q = &S.q[S.nq++];
	q->tex = tex;
	q->mode = S.mode;
	memcpy(q->xy, xy, sizeof(q->xy));
	memcpy(q->uv, uv, sizeof(q->uv));
	memcpy(q->rgba, S.rgba, sizeof(q->rgba));
}

void sgx2d_quad_uv(int tex, float x, float y, float w, float h,
		   float u0, float v0, float u1, float v1)
{
	float xy[8] = { x, y, x + w, y, x + w, y + h, x, y + h };
	float uv[8] = { u0, v0, u1, v0, u1, v1, u0, v1 };

	sgx2d_quad4(tex, xy, uv);
}

void sgx2d_quad(int tex, float x, float y, float w, float h)
{
	sgx2d_quad_uv(tex, x, y, w, h, 0, 0, 1, 1);
}

void sgx2d_fill(float x, float y, float w, float h)
{
	sgx2d_quad(S.white, x, y, w, h);
}

/* two triangles (corners 0 1 2, 0 2 3) as r g b a u v x y */
static void vb_quad(float *v, const struct quad *q)
{
	static const int order[6] = { 0, 1, 2, 0, 2, 3 };
	float us = S.tex[q->tex].us, vs = S.tex[q->tex].vs;
	int i, k;

	for (i = 0; i < 6; i++, v += VTX_FLOATS) {
		k = order[i];
		memcpy(v, q->rgba, 4 * sizeof(float));
		v[4] = q->uv[2 * k] * us;
		v[5] = q->uv[2 * k + 1] * vs;
		v[6] = q->xy[2 * k] / S.w * 2 - 1;
		v[7] = q->xy[2 * k + 1] / S.h * 2 - 1;
	}
}

static uint32_t frame_alloc(const void *p, uint32_t n, uint32_t align)
{
	uint32_t off = (S.ftop + align - 1) & ~(align - 1);

	memcpy(S.fbuf + off, p, n);
	S.ftop = off + n;
	return S.ext + EXT_FRAME + off;
}

static uint32_t fetch_block(uint32_t vb)
{
	uint8_t b[0x80];

	memcpy(b, S.tmpl + S.toff[T_FETCH], S.tsize[T_FETCH]);
	((uint32_t *)b)[0] = vb;		/* r g b a */
	((uint32_t *)b)[4] = vb + 16;		/* u v */
	((uint32_t *)b)[8] = vb + 24;		/* x y */
	return frame_alloc(b, S.tsize[T_FETCH], 0x40);
}

/* The microkernel stores the 3D block's address in the render details
 * (+0x24, entry 0 word 1) when the TA starts and clears it when the 3D pass
 * has ended: that is the frame's fence. */
static int wait_3d(void)
{
	uint32_t v = 1;
	int i;

	for (i = 0; i < 200000; i++) {
		if (pread(S.mem, &v, 4, (off_t)S.kick[1] + 0x24) != 4)
			return -EIO;
		if (!v)
			return 0;
		if (i > 100)
			usleep(50);
	}
	return -ETIMEDOUT;
}

int sgx2d_finish(void)
{
	return wait_3d();
}

int sgx2d_end(void)
{
	uint32_t *v = S.vdmbuf, vbva = S.ext + EXT_VB, d0, p0, limit;
	uint8_t full[0x80], prog[0x40];
	struct quad bgq = { .tex = S.bg, .mode = SGX2D_BLEND,
			    .uv = { 0, 0, 1, 0, 1, 1, 0, 1 }, .rgba = { 1, 1, 1, 1 } };
	/* the VDM reads ahead: keep 512 bytes clear of the window's end */
	int i, n, ret, maxv = S.vdm_size / 4 - 128 - 10 - S.ntail;

	if (S.bg < 0 || S.bg >= S.ntex)
		return -EINVAL;
	if ((ret = wait_3d()))		/* the previous frame still reads our buffers */
		return ret;
	S.ftop = 0;
	/* draw 0: the background, with the whole state */
	bgq.xy[2] = bgq.xy[4] = S.w;
	bgq.xy[5] = bgq.xy[7] = S.h;
	vb_quad(S.vb, &bgq);
	memcpy(full, S.tmpl + S.toff[T_FULL], S.tsize[T_FULL]);
	((uint32_t *)full)[6] = p27(S.tex[S.bg].block[SGX2D_BLEND]);
	d0 = frame_alloc(full, S.tsize[T_FULL], 0x20);
	memcpy(prog, S.tmpl + S.toff[T_FULLPROG], S.tsize[T_FULLPROG]);
	((uint32_t *)prog)[0] = d0;
	p0 = frame_alloc(prog, S.tsize[T_FULLPROG], 0x40);
	*v++ = vdm4(4, S.consts0); *v++ = 0x1000e102;
	*v++ = vdm4(4, p0); *v++ = 0x12022206;
	*v++ = 0x81c00006; *v++ = S.idx; *v++ = 0x70000000; *v++ = 0x003fffff;
	*v++ = vdm4(S.fetch_tag, fetch_block(vbva)); *v++ = S.fetch_word;
	/* one draw per run of quads with the same texture and blend mode */
	limit = S.idx_count / 6;
	for (i = 0; i < S.nq; i += n) {
		for (n = 1; i + n < S.nq && S.q[i + n].tex == S.q[i].tex &&
		     S.q[i + n].mode == S.q[i].mode && n < (int)limit; n++)
			;
		if (v - S.vdmbuf > maxv || S.ftop + 0x100 > S.ext_size - EXT_FRAME)
			break;		/* too many draws: the rest is dropped */
		*v++ = vdm4(4, S.consts); *v++ = 0x1000e102;
		*v++ = vdm4(4, S.tex[S.q[i].tex].deltaprog[S.q[i].mode]); *v++ = 0x12022201;
		*v++ = 0x81c00000 | 6 * n; *v++ = S.idx; *v++ = 0x70000000; *v++ = 0x003fffff;
		*v++ = vdm4(S.fetch_tag, fetch_block(vbva + QUAD_BYTES * (1 + i)));
		*v++ = S.fetch_word;
	}
	for (n = 0; n < S.nq; n++)
		vb_quad(S.vb + (1 + n) * 6 * VTX_FLOATS, &S.q[n]);
	for (n = 0; n < S.ntail; n++)
		*v++ = S.tail[n];
	if ((ret = put(vbva, S.vb, QUAD_BYTES * (1 + S.nq))) ||
	    (ret = put(S.ext + EXT_FRAME, S.fbuf, S.ftop)) ||
	    (ret = put(S.vdm, S.vdmbuf, (v - S.vdmbuf) * 4)))
		return ret;
	return cmd("rkick 0x%x 0x%x 0x%x", S.kick[0], S.kick[1], S.kick[2]);
}
