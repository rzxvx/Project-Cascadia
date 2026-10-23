/* kmsrot -- the screen turned by the KMS plane's rotation (apple-sgx: the
 * 2D engine's copy onto the screen turned, docs/research/p105-mesa.md,
 * M41).  For each of rotate-0, -90, -180, -270: a dumb buffer of the turned
 * size whose every pixel is its own (x and y in its bytes), committed
 * atomically with the rotation, and the screen read back through /dev/fb0
 * (simplefb shows the same memory) against where DRM's rotation puts each
 * pixel (drm_rect_rotate); then a box of it changed and shown with a
 * DIRTYFB, and the screen checked again -- the damaged copy turned.  With
 * nothing else on the screen (no X: it is the DRM master); the text
 * console is unbound while it runs (fbcon blinks its cursor into the same
 * memory).
 *
 *   sgx-gl kmsrot [HOLD]     (HOLD: seconds each rotation stays)
 *
 * Exit status 0 when every pixel was right.
 */
#include <drm_fourcc.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static int fd;

static void
console(int on)
{
	FILE *f = fopen("/sys/class/vtconsole/vtcon1/bind", "w");

	if (f) {
		fprintf(f, "%d\n", on);
		fclose(f);
	}
}

static void
console_back(void)
{
	console(1);
}

static uint32_t
prop_id(uint32_t obj, uint32_t type, const char *name, drmModePropertyRes **out)
{
	drmModeObjectProperties *props = drmModeObjectGetProperties(fd, obj, type);
	uint32_t id = 0;

	for (uint32_t i = 0; props && i < props->count_props && !id; i++) {
		drmModePropertyRes *p = drmModeGetProperty(fd, props->props[i]);

		if (p && !strcmp(p->name, name)) {
			id = p->prop_id;
			if (out) {
				*out = p;
				continue;
			}
		}
		drmModeFreeProperty(p);
	}
	drmModeFreeObjectProperties(props);
	return id;
}

/* the pattern: every pixel its own -- x's low byte in blue, y's in green,
 * their high bits in red (to 4096 x 4096) */
static uint32_t
pattern(int x, int y, int box)
{
	uint32_t v = (uint32_t)(((x >> 8) & 0xf) | (y >> 8) << 4) << 16 | (y & 0xff) << 8 | (x & 0xff);

	return 0xff000000 | (box ? v ^ 0x00ffffff : v);
}

/* the screen against the buffer, turned: screen pixel xs, ys shows buffer
 * pixel x, y (DRM's sense: anticlockwise quarter turns q) */
static int
check(int q, int fw, int fh, int sw, int sh, int bx, int by, int bw, int bh)
{
	int fb = open("/dev/fb0", O_RDONLY), wrong = 0, first = -1;
	int x0 = sw, y0 = sh, x1 = -1, y1 = -1;
	size_t len = (size_t)sw * sh * 4;
	uint32_t *px = malloc(len), got = 0, want = 0;

	if (fb < 0 || read(fb, px, len) != (ssize_t)len) {
		perror("kmsrot: /dev/fb0");
		return -1;
	}
	close(fb);
	for (int ys = 0; ys < sh; ys++)
		for (int xs = 0; xs < sw; xs++) {
			int x, y;
			uint32_t e;

			switch (q) {
			case 0: x = xs, y = ys; break;
			case 1: x = fw - 1 - ys, y = xs; break;
			case 2: x = fw - 1 - xs, y = fh - 1 - ys; break;
			default: x = ys, y = fh - 1 - xs; break;
			}
			e = pattern(x, y, x >= bx && x < bx + bw && y >= by && y < by + bh);
			if ((px[ys * sw + xs] ^ e) & 0xffffff) {
				x0 = xs < x0 ? xs : x0;
				y0 = ys < y0 ? ys : y0;
				x1 = xs > x1 ? xs : x1;
				y1 = ys > y1 ? ys : y1;
				if (!wrong++) {
					first = ys * sw + xs;
					got = px[first];
					want = e;
				}
			}
		}
	free(px);
	if (wrong)
		printf("  WRONG: %d of %d pixels, within %d,%d..%d,%d; the first at %d,%d: %06x, "
		       "not %06x\n", wrong, sw * sh, x0, y0, x1, y1, first % sw, first / sw,
		       got & 0xffffff, want & 0xffffff);
	return wrong;
}

int
main(int argc, char **argv)
{
	static const char *const names[4] = { "rotate-0", "rotate-90", "rotate-180", "rotate-270" };
	int hold = argc > 1 ? atoi(argv[1]) : 0, failed = 0;
	drmModeRes *res;
	drmModeConnector *conn = NULL;
	drmModePlaneRes *pres;
	drmModePropertyRes *rot = NULL;
	drmModeModeInfo mode;
	uint32_t crtc, plane = 0, blob, bits[4] = { 0 }, old_fb = 0;
	uint32_t p_fb, p_crtc, p_sx, p_sy, p_sw, p_sh, p_cx, p_cy, p_cw, p_ch, p_rot;
	uint32_t c_mode, c_active, k_crtc;

	/* fbcon draws its cursor into the same memory: off it while this runs */
	console(0);
	atexit(console_back);
	fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
	if (fd < 0 || drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1) ||
	    drmSetClientCap(fd, DRM_CLIENT_CAP_ATOMIC, 1))
		return fprintf(stderr, "kmsrot: no atomic KMS on /dev/dri/card0\n"), 2;
	res = drmModeGetResources(fd);
	for (int i = 0; res && i < res->count_connectors && !conn; i++) {
		conn = drmModeGetConnector(fd, res->connectors[i]);
		if (conn && (conn->connection != DRM_MODE_CONNECTED || !conn->count_modes)) {
			drmModeFreeConnector(conn);
			conn = NULL;
		}
	}
	if (!conn || !res->count_crtcs)
		return fprintf(stderr, "kmsrot: no screen\n"), 2;
	mode = conn->modes[0];
	crtc = res->crtcs[0];
	pres = drmModeGetPlaneResources(fd);
	for (uint32_t i = 0; pres && i < pres->count_planes && !plane; i++) {
		drmModePropertyRes *type = NULL;
		drmModeObjectProperties *props = drmModeObjectGetProperties(fd, pres->planes[i],
									    DRM_MODE_OBJECT_PLANE);

		prop_id(pres->planes[i], DRM_MODE_OBJECT_PLANE, "type", &type);
		for (uint32_t j = 0; type && props && j < props->count_props; j++)
			if (props->props[j] == type->prop_id &&
			    props->prop_values[j] == DRM_PLANE_TYPE_PRIMARY)
				plane = pres->planes[i];
		drmModeFreeProperty(type);
		drmModeFreeObjectProperties(props);
	}
	p_rot = plane ? prop_id(plane, DRM_MODE_OBJECT_PLANE, "rotation", &rot) : 0;
	if (!p_rot)
		return fprintf(stderr, "kmsrot: the primary plane has no rotation\n"), 1;
	for (int j = 0; j < rot->count_enums; j++)
		for (int k = 0; k < 4; k++)
			if (!strcmp(rot->enums[j].name, names[k]))
				bits[k] = 1u << rot->enums[j].value;
	p_fb = prop_id(plane, DRM_MODE_OBJECT_PLANE, "FB_ID", NULL);
	p_crtc = prop_id(plane, DRM_MODE_OBJECT_PLANE, "CRTC_ID", NULL);
	p_sx = prop_id(plane, DRM_MODE_OBJECT_PLANE, "SRC_X", NULL);
	p_sy = prop_id(plane, DRM_MODE_OBJECT_PLANE, "SRC_Y", NULL);
	p_sw = prop_id(plane, DRM_MODE_OBJECT_PLANE, "SRC_W", NULL);
	p_sh = prop_id(plane, DRM_MODE_OBJECT_PLANE, "SRC_H", NULL);
	p_cx = prop_id(plane, DRM_MODE_OBJECT_PLANE, "CRTC_X", NULL);
	p_cy = prop_id(plane, DRM_MODE_OBJECT_PLANE, "CRTC_Y", NULL);
	p_cw = prop_id(plane, DRM_MODE_OBJECT_PLANE, "CRTC_W", NULL);
	p_ch = prop_id(plane, DRM_MODE_OBJECT_PLANE, "CRTC_H", NULL);
	c_mode = prop_id(crtc, DRM_MODE_OBJECT_CRTC, "MODE_ID", NULL);
	c_active = prop_id(crtc, DRM_MODE_OBJECT_CRTC, "ACTIVE", NULL);
	k_crtc = prop_id(conn->connector_id, DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID", NULL);
	if (drmModeCreatePropertyBlob(fd, &mode, sizeof(mode), &blob))
		return perror("kmsrot: mode blob"), 2;
	printf("screen %ux%u, plane %u, rotation bits %x %x %x %x\n", mode.hdisplay,
	       mode.vdisplay, plane, bits[0], bits[1], bits[2], bits[3]);

	for (int q = 0; q < 5; q++) {
		int r = q & 3;     /* (and back to rotate-0 at the end) */
		int fw = r & 1 ? mode.vdisplay : mode.hdisplay, fh = r & 1 ? mode.hdisplay : mode.vdisplay;
		struct drm_mode_create_dumb cd = { .width = fw, .height = fh, .bpp = 32 };
		struct drm_mode_map_dumb md = { 0 };
		uint32_t handles[4] = { 0 }, pitches[4] = { 0 }, offsets[4] = { 0 }, fb_id;
		drmModeAtomicReq *req;
		drmModeClip clip;
		uint32_t *map;
		int bx, by, bw = 48, bh = 32, wrong;

		if (!bits[r]) {
			printf("%-10s  not offered\n", names[r]);
			continue;
		}
		if (drmIoctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &cd))
			return perror("kmsrot: dumb buffer"), 2;
		md.handle = cd.handle;
		if (drmIoctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &md))
			return perror("kmsrot: map"), 2;
		map = mmap(NULL, cd.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, md.offset);
		if (map == MAP_FAILED)
			return perror("kmsrot: mmap"), 2;
		for (int y = 0; y < fh; y++)
			for (int x = 0; x < fw; x++)
				map[y * (cd.pitch / 4) + x] = pattern(x, y, 0);
		handles[0] = cd.handle;
		pitches[0] = cd.pitch;
		if (drmModeAddFB2(fd, fw, fh, DRM_FORMAT_XRGB8888, handles, pitches, offsets, &fb_id, 0)) {
			printf("%-10s  no %dx%d framebuffer: %s\n", names[r], fw, fh, strerror(errno));
			failed++;
			continue;
		}

		req = drmModeAtomicAlloc();
		drmModeAtomicAddProperty(req, conn->connector_id, k_crtc, crtc);
		drmModeAtomicAddProperty(req, crtc, c_mode, blob);
		drmModeAtomicAddProperty(req, crtc, c_active, 1);
		drmModeAtomicAddProperty(req, plane, p_fb, fb_id);
		drmModeAtomicAddProperty(req, plane, p_crtc, crtc);
		drmModeAtomicAddProperty(req, plane, p_sx, 0);
		drmModeAtomicAddProperty(req, plane, p_sy, 0);
		drmModeAtomicAddProperty(req, plane, p_sw, (uint64_t)fw << 16);
		drmModeAtomicAddProperty(req, plane, p_sh, (uint64_t)fh << 16);
		drmModeAtomicAddProperty(req, plane, p_cx, 0);
		drmModeAtomicAddProperty(req, plane, p_cy, 0);
		drmModeAtomicAddProperty(req, plane, p_cw, mode.hdisplay);
		drmModeAtomicAddProperty(req, plane, p_ch, mode.vdisplay);
		drmModeAtomicAddProperty(req, plane, p_rot, bits[r]);
		if (drmModeAtomicCommit(fd, req, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL)) {
			printf("%-10s  the commit failed: %s\n", names[r], strerror(errno));
			failed++;
			drmModeAtomicFree(req);
			continue;
		}
		drmModeAtomicFree(req);
		if (old_fb)
			drmModeRmFB(fd, old_fb);
		old_fb = fb_id;

		printf("%-10s  %dx%d turned onto %ux%u\n", names[r], fw, fh, mode.hdisplay, mode.vdisplay);
		wrong = check(r, fw, fh, mode.hdisplay, mode.vdisplay, 0, 0, 0, 0);
		printf("            the whole plane: %s\n", wrong ? "WRONG" : "right");
		/* a box changed, shown by a DIRTYFB alone: the damaged copy turned
		 * (not on 16-pixel edges, to be widened) */
		bx = fw / 3 + 5;
		by = fh / 4 + 3;
		for (int y = by; y < by + bh; y++)
			for (int x = bx; x < bx + bw; x++)
				map[y * (cd.pitch / 4) + x] = pattern(x, y, 1);
		clip = (drmModeClip){ bx, by, bx + bw, by + bh };
		if (drmModeDirtyFB(fd, fb_id, &clip, 1)) {
			printf("  DIRTYFB failed: %s\n", strerror(errno));
			wrong++;
		} else {
			wrong += check(r, fw, fh, mode.hdisplay, mode.vdisplay, bx, by, bw, bh);
		}
		printf("            %s\n", wrong ? "WRONG" : "every pixel right, and the damaged box");
		failed += !!wrong;
		munmap(map, cd.size);
		if (hold)
			sleep(hold);
	}
	return failed ? 1 : 0;
}
