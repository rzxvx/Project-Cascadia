/* tuxorbit -- the GPU demo (tux-gpudemo): Tux on the GPU, the camera going
 * round him.  teutekeune's scene, made ready by make_tux.py (assets/tux.glb,
 * assets/tux-baked.png: every lit object's colour baked on its second UV
 * map, display-referred), drawn unlit through OpenGL ES 2 on KMS,
 * fullscreen -- the logo white, with a bloom: the logo alone (the rest
 * black, for the depth) into a small target, blurred, added onto the
 * screen.  A half-transparent EXIT button in the top right corner ends it,
 * touched and let go.  The frames a second every two seconds, and on the
 * whole run at the end.
 *
 *   tux-gpudemo [SECONDS [PERIOD]]   (tux-gpudemo.sh: the console and
 *                                     fbkeyboard off the screen meanwhile;
 *                                     default until EXIT, a turn in 12 s)
 *   TUX_DIR=dir                      where tux.glb and tux-baked.png are
 *                                    (../share/tux-gpudemo beside this
 *                                    program)
 *   TUX_TOUCH=/dev/input/event0      the touchscreen (0..its range on both
 *                                    axes, portrait); TUX_TOUCH_FLIP=x, y
 *                                    or xy if they run the other way
 *   TUX_BLOOM=0                      no bloom
 *   TUX_BLUR=2                       the bloom blurred once each way, not
 *                                    twice: two renders a frame fewer
 *   TUX_NOFLIP=1                     frames drawn but not shown: as fast as
 *                                    the GPU goes, no page flip to wait for
 *   TUX_SHOT=FILE                    the screen's last frame (glReadPixels)
 *                                    as a PPM
 */
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <gbm.h>
#include <drm_fourcc.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include <linux/input.h>
#include <sys/ioctl.h>

#include <fcntl.h>
#include <libgen.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define CGLTF_IMPLEMENTATION
#include "cgltf.h"
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include "stb_image.h"

/* the bloom's target, GLOW x GLOW, blurred across and up twice, the taps
 * 1.5 texels apart -- or with TUX_BLUR=2 once, 2.25 apart: each pass is a
 * render of its own, and a render costs the SGX some 2 ms whatever its
 * size (drawn unshown: 69 frames a second, 85 with TUX_BLUR=2, 202 without
 * the bloom; once at 64 x 64, the halo came out in steps, and once at 128
 * x 128 has a faint ring) */
#define GLOW 128

static int fd;
static uint32_t crtc_id, conn_id;
static drmModeModeInfo mode;
static int flip_pending;

/* ---- the scene ---------------------------------------------------------- */

struct vtx {
	float p[3], t[2];
};

static struct vtx *verts;
static uint16_t *idx;
static unsigned nverts, nidx, nlit;     /* indices: the lit objects', then the logo's */
/* the bounds of everything but the base (the primitive widest across: the
 * platform), the orbit's middle */
static float bbmin[3] = { 1e30f, 1e30f, 1e30f }, bbmax[3] = { -1e30f, -1e30f, -1e30f };
static float cam_world[16], yfov = 0.5566f;

static int is_logo(const cgltf_material *m)
{
	return m && (m->emissive_factor[0] > 0 || m->emissive_factor[1] > 0 ||
		     m->emissive_factor[2] > 0);
}

/* every primitive's vertices in world space, its TEXCOORD_1; the logo's
 * (emissive) after the rest */
static int load_glb(const char *path)
{
	cgltf_options opt = { 0 };
	cgltf_data *d = NULL;
	unsigned cap = 0, icap = 0, nlogo_idx = 0, nprim = 0;
	uint16_t *logo_idx = NULL;
	float pmin[64][3], pmax[64][3], widest = -1;
	int base = -1;

	if (cgltf_parse_file(&opt, path, &d) != cgltf_result_success ||
	    cgltf_load_buffers(&opt, d, path) != cgltf_result_success) {
		fprintf(stderr, "tuxorbit: %s: not a glTF it can read\n", path);
		return -1;
	}
	for (cgltf_size n = 0; n < d->nodes_count; n++) {
		cgltf_node *node = &d->nodes[n];
		float w[16];

		cgltf_node_transform_world(node, w);
		if (node->camera && node->camera->type == cgltf_camera_type_perspective) {
			memcpy(cam_world, w, sizeof(w));
			yfov = node->camera->data.perspective.yfov;
		}
		if (!node->mesh)
			continue;
		for (cgltf_size k = 0; k < node->mesh->primitives_count; k++) {
			cgltf_primitive *pr = &node->mesh->primitives[k];
			cgltf_accessor *pos = NULL, *uv = NULL;
			unsigned first = nverts, logo = is_logo(pr->material);

			for (cgltf_size a = 0; a < pr->attributes_count; a++) {
				if (pr->attributes[a].type == cgltf_attribute_type_position)
					pos = pr->attributes[a].data;
				if (pr->attributes[a].type == cgltf_attribute_type_texcoord &&
				    pr->attributes[a].index == 1)
					uv = pr->attributes[a].data;
			}
			if (!pos || !pr->indices || (!uv && !logo))
				continue;
			if (nverts + pos->count > 65535) {
				fprintf(stderr, "tuxorbit: more than 65535 vertices\n");
				return -1;
			}
			if (nprim == 64) {
				fprintf(stderr, "tuxorbit: more than 64 primitives\n");
				return -1;
			}
			for (int c = 0; c < 3; c++)
				pmin[nprim][c] = 1e30f, pmax[nprim][c] = -1e30f;
			if (nverts + pos->count > cap) {
				cap = (nverts + pos->count) * 2;
				verts = realloc(verts, cap * sizeof(*verts));
			}
			for (cgltf_size i = 0; i < pos->count; i++) {
				float p[3], t[2] = { 0, 0 };
				struct vtx *v = &verts[nverts + i];

				cgltf_accessor_read_float(pos, i, p, 3);
				if (uv)
					cgltf_accessor_read_float(uv, i, t, 2);
				for (int c = 0; c < 3; c++) {
					v->p[c] = w[c] * p[0] + w[4 + c] * p[1] + w[8 + c] * p[2] + w[12 + c];
					pmin[nprim][c] = fminf(pmin[nprim][c], v->p[c]);
					pmax[nprim][c] = fmaxf(pmax[nprim][c], v->p[c]);
				}
				v->t[0] = t[0];
				v->t[1] = t[1];
			}
			nverts += pos->count;
			{
				float across = (pmax[nprim][0] - pmin[nprim][0]) *
					       (pmax[nprim][2] - pmin[nprim][2]);

				if (across > widest)
					widest = across, base = nprim;
				nprim++;
			}
			for (cgltf_size i = 0; i < pr->indices->count; i++) {
				uint16_t x = (uint16_t)(first + cgltf_accessor_read_index(pr->indices, i));

				if (logo) {
					logo_idx = realloc(logo_idx, (nlogo_idx + 1) * sizeof(*logo_idx));
					logo_idx[nlogo_idx++] = x;
				} else {
					if (nidx == icap)
						idx = realloc(idx, (icap = icap * 2 + 1024) * sizeof(*idx));
					idx[nidx++] = x;
				}
			}
		}
	}
	for (unsigned k = 0; k < nprim; k++)
		for (int c = 0; k != (unsigned)base && c < 3; c++) {
			bbmin[c] = fminf(bbmin[c], pmin[k][c]);
			bbmax[c] = fmaxf(bbmax[c], pmax[k][c]);
		}
	nlit = nidx;
	idx = realloc(idx, (nidx + nlogo_idx) * sizeof(*idx));
	memcpy(idx + nidx, logo_idx, nlogo_idx * sizeof(*idx));
	nidx += nlogo_idx;
	free(logo_idx);
	cgltf_free(d);
	printf("scene: %u vertices, %u triangles (%u the logo's)\n", nverts, nidx / 3,
	       (nidx - nlit) / 3);
	return nlit && nidx > nlit ? 0 : -1;
}

/* ---- matrices (column-major, as GL takes them) ---------------------------- */

static void mul(float *r, const float *a, const float *b)
{
	float t[16];

	for (int c = 0; c < 4; c++)
		for (int i = 0; i < 4; i++)
			t[c * 4 + i] = a[i] * b[c * 4] + a[4 + i] * b[c * 4 + 1] +
				       a[8 + i] * b[c * 4 + 2] + a[12 + i] * b[c * 4 + 3];
	memcpy(r, t, sizeof(t));
}

static void perspective(float *m, float fovy, float aspect, float n, float f)
{
	float t = 1.0f / tanf(fovy / 2);

	memset(m, 0, 16 * sizeof(float));
	m[0] = t / aspect;
	m[5] = t;
	m[10] = (f + n) / (n - f);
	m[11] = -1;
	m[14] = 2 * f * n / (n - f);
}

static void look_at(float *m, const float *eye, const float *at)
{
	float f[3] = { at[0] - eye[0], at[1] - eye[1], at[2] - eye[2] }, s[3], u[3];
	float l = sqrtf(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);

	for (int i = 0; i < 3; i++)
		f[i] /= l;
	s[0] = -f[2], s[1] = 0, s[2] = f[0];       /* f x (0, 1, 0) */
	l = sqrtf(s[0] * s[0] + s[2] * s[2]);
	s[0] /= l, s[2] /= l;
	u[0] = s[1] * f[2] - s[2] * f[1];
	u[1] = s[2] * f[0] - s[0] * f[2];
	u[2] = s[0] * f[1] - s[1] * f[0];
	memset(m, 0, 16 * sizeof(float));
	for (int i = 0; i < 3; i++) {
		m[i * 4] = s[i];
		m[i * 4 + 1] = u[i];
		m[i * 4 + 2] = -f[i];
	}
	m[12] = -(s[0] * eye[0] + s[1] * eye[1] + s[2] * eye[2]);
	m[13] = -(u[0] * eye[0] + u[1] * eye[1] + u[2] * eye[2]);
	m[14] = f[0] * eye[0] + f[1] * eye[1] + f[2] * eye[2];
	m[15] = 1;
}

/* ---- GL ----------------------------------------------------------------- */

static const char *scene_vs =
	"attribute vec3 p;\n"
	"attribute vec2 t;\n"
	"uniform mat4 m;\n"
	"varying vec2 v;\n"
	"void main() { v = t; gl_Position = m * vec4(p, 1.0); }\n";
static const char *baked_fs =
	"precision mediump float;\n"
	"uniform sampler2D s;\n"
	"varying vec2 v;\n"
	"void main() { gl_FragColor = texture2D(s, v); }\n";
static const char *flat_fs =
	"precision mediump float;\n"
	"uniform vec4 c;\n"
	"void main() { gl_FragColor = c; }\n";
static const char *quad_vs =
	"attribute vec2 p;\n"
	"varying vec2 v;\n"
	"void main() { v = p * 0.5 + 0.5; gl_Position = vec4(p, 0.0, 1.0); }\n";
/* a 9-tap Gaussian in 5 lookups, each between two texels (linear filtering) */
static const char *blur_fs =
	"precision mediump float;\n"
	"uniform sampler2D s;\n"
	"uniform vec2 d;\n"
	"varying vec2 v;\n"
	"void main() {\n"
	"  vec4 c = texture2D(s, v) * 0.2270270;\n"
	"  c += (texture2D(s, v + d * 1.3846154) + texture2D(s, v - d * 1.3846154)) * 0.3162162;\n"
	"  c += (texture2D(s, v + d * 3.2307692) + texture2D(s, v - d * 3.2307692)) * 0.0702703;\n"
	"  gl_FragColor = c;\n"
	"}\n";
static const char *add_fs =
	"precision mediump float;\n"
	"uniform sampler2D s;\n"
	"uniform float k;\n"
	"varying vec2 v;\n"
	"void main() { gl_FragColor = texture2D(s, v) * k; }\n";

static GLuint program(const char *vs, const char *fs)
{
	GLuint p = glCreateProgram();
	char log[1024];
	GLint ok;

	for (int i = 0; i < 2; i++) {
		GLuint s = glCreateShader(i ? GL_FRAGMENT_SHADER : GL_VERTEX_SHADER);

		glShaderSource(s, 1, i ? &fs : &vs, NULL);
		glCompileShader(s);
		glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
		if (!ok) {
			glGetShaderInfoLog(s, sizeof(log), NULL, log);
			fprintf(stderr, "tuxorbit: a shader: %s\n", log);
			exit(2);
		}
		glAttachShader(p, s);
	}
	glBindAttribLocation(p, 0, "p");
	glBindAttribLocation(p, 1, "t");
	glLinkProgram(p);
	glGetProgramiv(p, GL_LINK_STATUS, &ok);
	if (!ok) {
		glGetProgramInfoLog(p, sizeof(log), NULL, log);
		fprintf(stderr, "tuxorbit: a program: %s\n", log);
		exit(2);
	}
	return p;
}

static GLuint texture_of(const char *path)
{
	int w, h, n;
	unsigned char *px = stbi_load(path, &w, &h, &n, 4);
	GLuint t;

	if (!px) {
		fprintf(stderr, "tuxorbit: %s: %s\n", path, stbi_failure_reason());
		exit(2);
	}
	/* (glTF's texture coordinates have row 0 at v = 0: the rows as they
	 * come) */
	glGenTextures(1, &t);
	glBindTexture(GL_TEXTURE_2D, t);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
	glGenerateMipmap(GL_TEXTURE_2D);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	stbi_image_free(px);
	printf("texture: %s, %d x %d\n", path, w, h);
	return t;
}

/* a GLOW x GLOW colour target, filtered linear; depth with it if asked */
static GLuint target(GLuint *tex, int depth)
{
	GLuint fbo, rb;

	glGenTextures(1, tex);
	glBindTexture(GL_TEXTURE_2D, *tex);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, GLOW, GLOW, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glGenFramebuffers(1, &fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, *tex, 0);
	if (depth) {
		glGenRenderbuffers(1, &rb);
		glBindRenderbuffer(GL_RENDERBUFFER, rb);
		glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, GLOW, GLOW);
		glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, rb);
	}
	if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
		fprintf(stderr, "tuxorbit: the bloom's target is not complete\n");
		exit(2);
	}
	return fbo;
}

/* ---- the EXIT button and the touchscreen --------------------------------- */

#define BTN_W 192               /* the button, in screen pixels, top right */
#define BTN_H 80
#define BTN_MARGIN 24
#define BTN_TEX_W 256           /* its texture (power of two sizes) */
#define BTN_TEX_H 128

/* E X I T, 5 x 7, a row a byte (bit 4 the left) */
static const uint8_t glyph[4][7] = {
	{ 0x1f, 0x10, 0x10, 0x1e, 0x10, 0x10, 0x1f },
	{ 0x11, 0x11, 0x0a, 0x04, 0x0a, 0x11, 0x11 },
	{ 0x0e, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0e },
	{ 0x1f, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04 },
};

/* the button as a texture, premultiplied alpha: a rounded rectangle of
 * white at fill (its rim brighter), the letters on it */
static GLuint button_texture(float fill)
{
	static uint8_t px[BTN_TEX_H][BTN_TEX_W][4];
	const int scale = 6, tw = (4 * 6 - 1) * scale, th = 7 * scale;
	const int tx0 = (BTN_W - tw) / 2, ty0 = (BTN_H - th) / 2;
	const float r = 18;
	GLuint t;

	memset(px, 0, sizeof(px));
	for (int y = 0; y < BTN_H; y++)
		for (int x = 0; x < BTN_W; x++) {
			/* distance outside the rounded rectangle, for its edge */
			float cx = fmaxf(fmaxf(r - (x + 0.5f), x + 0.5f - (BTN_W - r)), 0);
			float cy = fmaxf(fmaxf(r - (y + 0.5f), y + 0.5f - (BTN_H - r)), 0);
			float d = sqrtf(cx * cx + cy * cy) - r;           /* < 0 inside */
			float cover = fminf(fmaxf(0.5f - d, 0), 1);
			float rim = fminf(fmaxf(d + 3.0f, 0), 1);         /* the last 3 px */
			float a = cover * (fill + rim * 0.35f);
			int gx = x - tx0, gy = y - ty0;

			if (gx >= 0 && gy >= 0 && gx < tw && gy < th) {
				int g = gx / scale / 6, col = gx / scale % 6, row = gy / scale;

				if (col < 5 && (glyph[g][row] >> (4 - col) & 1))
					a = 0.92f;
			}
			px[y][x][0] = px[y][x][1] = px[y][x][2] = px[y][x][3] = (uint8_t)(a * 255 + 0.5f);
		}
	glGenTextures(1, &t);
	glBindTexture(GL_TEXTURE_2D, t);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, BTN_TEX_W, BTN_TEX_H, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	return t;
}

static struct {
	int fd, flipx, flipy, xmax, ymax, cur;
	struct { int id, x, y; } slot[10];
} touch = { .fd = -1 };

static void touch_open(void)
{
	const char *dev = getenv("TUX_TOUCH"), *flip = getenv("TUX_TOUCH_FLIP");
	struct input_absinfo ai;

	touch.fd = open(dev ? dev : "/dev/input/event0", O_RDONLY | O_NONBLOCK | O_CLOEXEC);
	touch.flipx = flip && strchr(flip, 'x');
	touch.flipy = flip && strchr(flip, 'y');
	touch.xmax = touch.ymax = 4096;
	if (touch.fd >= 0 && !ioctl(touch.fd, EVIOCGABS(ABS_MT_POSITION_X), &ai) && ai.maximum > 0)
		touch.xmax = ai.maximum;
	if (touch.fd >= 0 && !ioctl(touch.fd, EVIOCGABS(ABS_MT_POSITION_Y), &ai) && ai.maximum > 0)
		touch.ymax = ai.maximum;
	for (int i = 0; i < 10; i++)
		touch.slot[i].id = -1;
	if (touch.fd < 0)
		fprintf(stderr, "tuxorbit: no touchscreen: no EXIT button\n");
}

static int on_button(int s, int W, int H)
{
	float x = (touch.flipx ? touch.xmax - touch.slot[s].x : touch.slot[s].x) * (float)W / touch.xmax;
	float y = (touch.flipy ? touch.ymax - touch.slot[s].y : touch.slot[s].y) * (float)H / touch.ymax;

	return x >= W - BTN_MARGIN - BTN_W && x < W - BTN_MARGIN && y >= BTN_MARGIN &&
	       y < BTN_MARGIN + BTN_H;
}

/* the touches since the last frame: 1 when one let go on the button;
 * *held, whether one is on it now */
static int touch_poll(int W, int H, int *held)
{
	struct input_event ev[64];
	int n, left = 0;

	while (touch.fd >= 0 && (n = read(touch.fd, ev, sizeof(ev))) > 0)
		for (int i = 0; i < n / (int)sizeof(ev[0]); i++) {
			if (ev[i].type != EV_ABS)
				continue;
			switch (ev[i].code) {
			case ABS_MT_SLOT:
				touch.cur = ev[i].value >= 0 && ev[i].value < 10 ? ev[i].value : 0;
				break;
			case ABS_MT_TRACKING_ID:
				if (ev[i].value < 0 && touch.slot[touch.cur].id >= 0 &&
				    on_button(touch.cur, W, H))
					left = 1;
				touch.slot[touch.cur].id = ev[i].value;
				break;
			case ABS_MT_POSITION_X:
				touch.slot[touch.cur].x = ev[i].value;
				break;
			case ABS_MT_POSITION_Y:
				touch.slot[touch.cur].y = ev[i].value;
				break;
			}
		}
	*held = 0;
	for (int s = 0; s < 10; s++)
		*held |= touch.slot[s].id >= 0 && on_button(s, W, H);
	return left;
}

/* ---- KMS (glkms.c's) ------------------------------------------------------ */

static double now(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void drop_fb(struct gbm_bo *bo, void *data)
{
	uint32_t id = (uint32_t)(uintptr_t)data;

	if (id)
		drmModeRmFB(fd, id);
}

static uint32_t fb_for(struct gbm_bo *bo)
{
	uint32_t id = (uint32_t)(uintptr_t)gbm_bo_get_user_data(bo);
	uint32_t handles[4] = { gbm_bo_get_handle(bo).u32 }, pitches[4] = { gbm_bo_get_stride(bo) };
	uint32_t offsets[4] = { 0 };

	if (id)
		return id;
	if (drmModeAddFB2(fd, gbm_bo_get_width(bo), gbm_bo_get_height(bo), DRM_FORMAT_XRGB8888,
			  handles, pitches, offsets, &id, 0)) {
		perror("tuxorbit: drmModeAddFB2");
		exit(1);
	}
	gbm_bo_set_user_data(bo, (void *)(uintptr_t)id, drop_fb);
	return id;
}

static void flipped(int fd, unsigned int seq, unsigned int s, unsigned int us, void *data)
{
	flip_pending = 0;
}

static int find_output(void)
{
	drmModeRes *res = drmModeGetResources(fd);

	if (!res)
		return -1;
	for (int i = 0; i < res->count_connectors; i++) {
		drmModeConnector *c = drmModeGetConnector(fd, res->connectors[i]);

		if (c && c->connection == DRM_MODE_CONNECTED && c->count_modes) {
			conn_id = c->connector_id;
			mode = c->modes[0];
			crtc_id = res->crtcs[0];
			drmModeFreeConnector(c);
			drmModeFreeResources(res);
			return 0;
		}
		drmModeFreeConnector(c);
	}
	drmModeFreeResources(res);
	return -1;
}

static void shot(const char *path, int w, int h)
{
	unsigned char *px = malloc((size_t)w * h * 4);
	FILE *f = fopen(path, "wb");

	glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
	fprintf(f, "P6\n%d %d\n255\n", w, h);
	for (int y = h - 1; y >= 0; y--)
		for (int x = 0; x < w; x++)
			fwrite(px + ((size_t)y * w + x) * 4, 1, 3, f);
	fclose(f);
	free(px);
	printf("the last frame: %s\n", path);
}

int main(int argc, char **argv)
{
	double seconds = argc > 1 ? atof(argv[1]) : 0, period = argc > 2 ? atof(argv[2]) : 12;
	const char *dir = getenv("TUX_DIR");
	char exe[512], share[600];
	int bloom = !getenv("TUX_BLOOM") || atoi(getenv("TUX_BLOOM"));
	int noflip = getenv("TUX_NOFLIP") && atoi(getenv("TUX_NOFLIP"));
	int passes = getenv("TUX_BLUR") && atoi(getenv("TUX_BLUR")) == 2 ? 2 : 4;
	float step = passes == 2 ? 2.25f : 1.5f;
	static const EGLint config_attrs[] = {
		EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8,
		EGL_BLUE_SIZE, 8, EGL_DEPTH_SIZE, 16, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
		EGL_NONE,
	};
	static const EGLint ctx_attrs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
	static const float quad[] = { -1, -1, 1, -1, -1, 1, 1, 1 };
	drmEventContext ev = { .version = 2, .page_flip_handler = flipped };
	PFNEGLGETPLATFORMDISPLAYEXTPROC get_display;
	struct gbm_bo *shown = NULL, *bo;
	struct gbm_surface *gs;
	struct gbm_device *gbm;
	EGLConfig configs[64], config = NULL;
	EGLint n = 0;
	EGLDisplay dpy;
	EGLSurface surf;
	EGLContext ctx;
	GLuint p_baked, p_flat, p_blur, p_add, tex, vbo, ibo, glow_fbo[2], glow_tex[2];
	GLint m_baked, m_flat, c_flat, d_blur, k_add;
	float eye0[3], at[3], radius, angle0, proj[16];
	char path[700];
	double t0, tlast, waited = 0, worked = 0;
	unsigned frames = 0, last_frames = 0;
	GLuint btn_tex[2];
	int W, H, held = 0;

	/* (the assets beside this program: bin/../share/tux-gpudemo) */
	if (!dir) {
		ssize_t k = readlink("/proc/self/exe", exe, sizeof(exe) - 1);

		exe[k > 0 ? k : 0] = 0;
		snprintf(share, sizeof(share), "%s/../share/tux-gpudemo", dirname(exe));
		dir = share;
	}
	snprintf(path, sizeof(path), "%s/tux.glb", dir);
	if (load_glb(path))
		return 2;
	if ((fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC)) < 0)
		return perror("tuxorbit: /dev/dri/card0"), 2;
	if (find_output())
		return fprintf(stderr, "tuxorbit: no connected output\n"), 2;
	W = mode.hdisplay;
	H = mode.vdisplay;
	gbm = gbm_create_device(fd);
	gs = gbm ? gbm_surface_create(gbm, W, H, GBM_FORMAT_XRGB8888,
				      GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING) : NULL;
	if (!gs)
		return fprintf(stderr, "tuxorbit: no GBM surface\n"), 2;
	get_display = (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
	dpy = get_display ? get_display(EGL_PLATFORM_GBM_KHR, gbm, NULL) : NULL;
	if (!dpy || !eglInitialize(dpy, NULL, NULL) || !eglBindAPI(EGL_OPENGL_ES_API))
		return fprintf(stderr, "tuxorbit: no EGL on GBM\n"), 2;
	eglChooseConfig(dpy, config_attrs, configs, 64, &n);
	for (int i = 0; i < n && !config; i++) {
		EGLint id;

		if (eglGetConfigAttrib(dpy, configs[i], EGL_NATIVE_VISUAL_ID, &id) &&
		    id == GBM_FORMAT_XRGB8888)
			config = configs[i];
	}
	if (!config)
		return fprintf(stderr, "tuxorbit: no XRGB8888 config with depth\n"), 2;
	ctx = eglCreateContext(dpy, config, EGL_NO_CONTEXT, ctx_attrs);
	surf = eglCreateWindowSurface(dpy, config, (EGLNativeWindowType)gs, NULL);
	if (ctx == EGL_NO_CONTEXT || surf == EGL_NO_SURFACE || !eglMakeCurrent(dpy, surf, surf, ctx))
		return fprintf(stderr, "tuxorbit: no context or window surface (0x%x)\n",
			       eglGetError()), 2;
	printf("output: %s (%dx%d), %s; bloom %s (%d passes)%s\n", mode.name, W, H,
	       glGetString(GL_RENDERER), bloom ? "on" : "off", passes, noflip ? ", frames not shown" : "");

	p_baked = program(scene_vs, baked_fs);
	p_flat = program(scene_vs, flat_fs);
	p_blur = program(quad_vs, blur_fs);
	p_add = program(quad_vs, add_fs);
	m_baked = glGetUniformLocation(p_baked, "m");
	m_flat = glGetUniformLocation(p_flat, "m");
	c_flat = glGetUniformLocation(p_flat, "c");
	d_blur = glGetUniformLocation(p_blur, "d");
	k_add = glGetUniformLocation(p_add, "k");
	snprintf(path, sizeof(path), "%s/tux-baked.png", dir);
	tex = texture_of(path);
	glGenBuffers(1, &vbo);
	glBindBuffer(GL_ARRAY_BUFFER, vbo);
	glBufferData(GL_ARRAY_BUFFER, nverts * sizeof(*verts), verts, GL_STATIC_DRAW);
	glGenBuffers(1, &ibo);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo);
	glBufferData(GL_ELEMENT_ARRAY_BUFFER, nidx * sizeof(*idx), idx, GL_STATIC_DRAW);
	glow_fbo[0] = target(&glow_tex[0], 1);
	glow_fbo[1] = target(&glow_tex[1], 0);
	btn_tex[0] = button_texture(0.16f);
	btn_tex[1] = button_texture(0.36f);
	touch_open();

	/* the orbit: the glb's camera, at its own height, going round the
	 * middle of everything but the base, looking at it level */
	for (int i = 0; i < 3; i++)
		eye0[i] = cam_world[12 + i];
	at[0] = (bbmin[0] + bbmax[0]) / 2;
	at[1] = eye0[1];
	at[2] = (bbmin[2] + bbmax[2]) / 2;
	radius = hypotf(eye0[0] - at[0], eye0[2] - at[2]);
	angle0 = atan2f(eye0[0] - at[0], eye0[2] - at[2]);
	perspective(proj, yfov, (float)W / H, 0.1f, 100.0f);
	printf("orbit: round (%.2f, %.2f, %.2f), radius %.2f, a turn in %.0f s; fov %.1f degrees\n",
	       at[0], at[1], at[2], radius, period, yfov * 180 / M_PI);

	glEnable(GL_CULL_FACE);
	glClearColor(0, 0, 0, 1);
	t0 = tlast = now();
	for (;;) {
		double t = now() - t0, tw;
		float a = angle0 + 2 * (float)M_PI * (float)(t / period), view[16], mvp[16];
		float eye[3] = { at[0] + radius * sinf(a), eye0[1], at[2] + radius * cosf(a) };

		if ((seconds > 0 && t >= seconds) || touch_poll(W, H, &held))
			break;
		look_at(view, eye, at);
		mul(mvp, proj, view);
		glBindBuffer(GL_ARRAY_BUFFER, vbo);
		glEnableVertexAttribArray(0);
		glEnableVertexAttribArray(1);
		glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(struct vtx), (void *)0);
		glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(struct vtx),
				      (void *)(3 * sizeof(float)));
		glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo);

		if (bloom) {
			/* the glow's source: the logo white, the rest black, depth tested */
			glBindFramebuffer(GL_FRAMEBUFFER, glow_fbo[0]);
			glViewport(0, 0, GLOW, GLOW);
			glEnable(GL_DEPTH_TEST);
			glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
			glUseProgram(p_flat);
			glUniformMatrix4fv(m_flat, 1, GL_FALSE, mvp);
			glUniform4f(c_flat, 0, 0, 0, 1);
			glDrawElements(GL_TRIANGLES, nlit, GL_UNSIGNED_SHORT, (void *)0);
			glUniform4f(c_flat, 1, 1, 1, 1);
			glDrawElements(GL_TRIANGLES, nidx - nlit, GL_UNSIGNED_SHORT,
				       (void *)(nlit * sizeof(*idx)));
			glDisable(GL_DEPTH_TEST);
			/* blurred: across, then up (twice) */
			glUseProgram(p_blur);
			glDisableVertexAttribArray(1);
			glBindBuffer(GL_ARRAY_BUFFER, 0);
			glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, quad);
			for (int pass = 0; pass < passes; pass++) {
				glBindFramebuffer(GL_FRAMEBUFFER, glow_fbo[!(pass & 1)]);
				glBindTexture(GL_TEXTURE_2D, glow_tex[pass & 1]);
				glUniform2f(d_blur, pass & 1 ? 0 : step / GLOW, pass & 1 ? step / GLOW : 0);
				glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
			}
			glBindBuffer(GL_ARRAY_BUFFER, vbo);
			glEnableVertexAttribArray(1);
			glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(struct vtx), (void *)0);
		}

		/* the screen: the baked colours, the logo white */
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
		glViewport(0, 0, W, H);
		glEnable(GL_DEPTH_TEST);
		glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
		glUseProgram(p_baked);
		glUniformMatrix4fv(m_baked, 1, GL_FALSE, mvp);
		glBindTexture(GL_TEXTURE_2D, tex);
		glDrawElements(GL_TRIANGLES, nlit, GL_UNSIGNED_SHORT, (void *)0);
		glUseProgram(p_flat);
		glUniformMatrix4fv(m_flat, 1, GL_FALSE, mvp);
		glUniform4f(c_flat, 1, 1, 1, 1);
		glDrawElements(GL_TRIANGLES, nidx - nlit, GL_UNSIGNED_SHORT, (void *)(nlit * sizeof(*idx)));
		glDisable(GL_DEPTH_TEST);
		if (bloom) {
			/* the glow added on */
			glUseProgram(p_add);
			glDisableVertexAttribArray(1);
			glBindBuffer(GL_ARRAY_BUFFER, 0);
			glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, quad);
			glBindTexture(GL_TEXTURE_2D, glow_tex[0]);
			glUniform1f(k_add, 1.25f);
			glEnable(GL_BLEND);
			glBlendFunc(GL_ONE, GL_ONE);
			glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
			glDisable(GL_BLEND);
		}
		{
			/* the button, over everything, its texels to the screen's
			 * pixels one to one */
			static const float id[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
			float x0 = 2.0f * (W - BTN_MARGIN - BTN_W) / W - 1, x1 = 2.0f * (W - BTN_MARGIN) / W - 1;
			float y0 = 1 - 2.0f * (BTN_MARGIN + BTN_H) / H, y1 = 1 - 2.0f * BTN_MARGIN / H;
			float u1 = (float)BTN_W / BTN_TEX_W, v1 = (float)BTN_H / BTN_TEX_H;
			/* (the texture's row 0 the button's top) */
			const struct vtx q[4] = {
				{ { x0, y0, 0 }, { 0, v1 } }, { { x1, y0, 0 }, { u1, v1 } },
				{ { x0, y1, 0 }, { 0, 0 } }, { { x1, y1, 0 }, { u1, 0 } },
			};

			glUseProgram(p_baked);
			glUniformMatrix4fv(m_baked, 1, GL_FALSE, id);
			glBindTexture(GL_TEXTURE_2D, btn_tex[held]);
			glBindBuffer(GL_ARRAY_BUFFER, 0);
			glEnableVertexAttribArray(1);
			glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(struct vtx), q[0].p);
			glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(struct vtx), q[0].t);
			glEnable(GL_BLEND);
			glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
			glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
			glDisable(GL_BLEND);
		}
		if (getenv("TUX_SHOT") && seconds > 0 && now() - t0 + 0.2 >= seconds)
			shot(getenv("TUX_SHOT"), W, H);

		eglSwapBuffers(dpy, surf);
		bo = gbm_surface_lock_front_buffer(gs);
		tw = now();
		worked += tw - t0 - t;
		if (noflip) {
			gbm_surface_release_buffer(gs, bo);
		} else if (!shown) {
			if (drmModeSetCrtc(fd, crtc_id, fb_for(bo), 0, 0, &conn_id, 1, &mode))
				return perror("tuxorbit: drmModeSetCrtc"), 1;
			shown = bo;
		} else {
			flip_pending = 1;
			if (drmModePageFlip(fd, crtc_id, fb_for(bo), DRM_MODE_PAGE_FLIP_EVENT, NULL))
				return perror("tuxorbit: drmModePageFlip"), 1;
			while (flip_pending)
				drmHandleEvent(fd, &ev);
			gbm_surface_release_buffer(gs, shown);
			shown = bo;
		}
		waited += now() - tw;
		frames++;
		if (now() - tlast >= 2) {
			printf("%6.1f s: %.1f frames a second\n", now() - t0,
			       (frames - last_frames) / (now() - tlast));
			fflush(stdout);
			tlast = now();
			last_frames = frames;
		}
	}
	printf("%u frames in %.1f s: %.1f frames a second; a frame %.1f ms drawing and swapping, "
	       "%.1f ms waiting for its flip\n", frames, now() - t0, frames / (now() - t0),
	       1000 * worked / frames, 1000 * waited / frames);
	eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	eglTerminate(dpy);
	return 0;
}
