/* glkms -- on the screen through KMS (docs/research/p105-mesa.md, M16): EGL
 * on GBM over /dev/dri/card0, a spinning triangle over a gradient for
 * FRAMES frames, each shown with a page flip, the frames a second; then a
 * last frame that does not move -- the gradient (red across, green up, as
 * GL has it) and a blue square in the middle -- and the screen's memory
 * (/dev/fb0) checked against it, every pixel.
 *
 *   sgx-gl glkms [FRAMES [HOLD]]     (default 300; HOLD: seconds the last
 *                                     frame stays before the program exits)
 */
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <gbm.h>
#include <drm_fourcc.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include <fcntl.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

static int fd;
static uint32_t crtc_id, conn_id;
static drmModeModeInfo mode;
static int flip_pending;

static const char *vs_src =
	"attribute vec4 p;\n"
	"attribute vec4 c;\n"
	"uniform float a;\n"
	"varying vec4 v;\n"
	"void main() {\n"
	"  v = c;\n"
	"  gl_Position = vec4(p.x * cos(a) - p.y * sin(a), p.x * sin(a) + p.y * cos(a), p.z, 1.0);\n"
	"}\n";
static const char *fs_src =
	"precision mediump float;\n"
	"varying vec4 v;\n"
	"void main() { gl_FragColor = v; }\n";

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
		perror("glkms: drmModeAddFB2");
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

/* the gradient over the screen and the square in the middle */
static void still(float *pos, float *col)
{
	static const float cx[6] = { 0, 1, 1, 0, 1, 0 }, cy[6] = { 0, 0, 1, 0, 1, 1 };

	for (int q = 0; q < 2; q++)
		for (int i = 0; i < 6; i++) {
			float s = q ? 0.5f : 1, x = (cx[i] * 2 - 1) * s, y = (cy[i] * 2 - 1) * s;
			float *p = pos + 4 * (6 * q + i), *c = col + 4 * (6 * q + i);

			p[0] = x, p[1] = y, p[2] = 0, p[3] = 1;
			c[0] = q ? 0 : x / 2 + 0.5f;
			c[1] = q ? 0 : y / 2 + 0.5f;
			c[2] = q ? 1 : 0;
			c[3] = 1;
		}
}

/* the screen against the still frame: /dev/fb0, row 0 at the top */
static int check_screen(int w, int h)
{
	int fb = open("/dev/fb0", O_RDONLY), wrong = 0, fx = -1, fy = -1;
	size_t len = (size_t)w * h * 4;
	uint8_t *px = malloc(len), got[3] = { 0 }, want[3] = { 0 };

	if (fb < 0 || read(fb, px, len) != (ssize_t)len) {
		perror("glkms: /dev/fb0");
		return -1;
	}
	close(fb);
	for (int r = 0; r < h; r++)
		for (int x = 0; x < w; x++) {
			const uint8_t *q = px + ((size_t)r * w + x) * 4;   /* B G R X */
			int y = h - 1 - r;
			float cx = (x + 0.5f) / w, cy = (y + 0.5f) / h;
			int in = cx > 0.25f && cx < 0.75f && cy > 0.25f && cy < 0.75f;
			uint8_t e[3] = { in ? 255 : 0, in ? 0 : (uint8_t)(cy * 255 + 0.5f),
					 in ? 0 : (uint8_t)(cx * 255 + 0.5f) };
			int bad = 0;

			for (int k = 0; k < 3; k++)
				bad |= abs(q[k] - e[k]) > 3;
			if (bad && !wrong++) {
				fx = x, fy = r;
				memcpy(got, q, 3);
				memcpy(want, e, 3);
			}
		}
	free(px);
	if (wrong)
		printf("the screen: WRONG, %d of %d pixels; the first at %d,%d (from the top): "
		       "BGR %02x%02x%02x, not %02x%02x%02x\n", wrong, w * h, fx, fy, got[0], got[1],
		       got[2], want[0], want[1], want[2]);
	else
		printf("the screen: every pixel right\n");
	return wrong;
}

int main(int argc, char **argv)
{
	int frames = argc > 1 ? atoi(argv[1]) : 300, wrong;
	static const EGLint config_attrs[] = {
		EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8,
		EGL_BLUE_SIZE, 8, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_NONE,
	};
	static const EGLint ctx_attrs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
	drmEventContext ev = { .version = 2, .page_flip_handler = flipped };
	PFNEGLGETPLATFORMDISPLAYEXTPROC get_display;
	struct gbm_bo *shown = NULL, *bo;
	struct gbm_surface *gs;
	struct gbm_device *gbm;
	float pos[12 * 4], col[12 * 4];
	EGLConfig configs[64], config = NULL;
	EGLint n = 0;
	EGLDisplay dpy;
	EGLSurface surf;
	EGLContext ctx;
	GLuint p;
	GLint la;
	double t0 = 0;

	if ((fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC)) < 0)
		return perror("glkms: /dev/dri/card0"), 2;
	if (find_output())
		return fprintf(stderr, "glkms: no connected output\n"), 2;
	printf("output: connector %u, CRTC %u, %s (%ux%u)\n", conn_id, crtc_id, mode.name,
	       mode.hdisplay, mode.vdisplay);

	gbm = gbm_create_device(fd);
	gs = gbm ? gbm_surface_create(gbm, mode.hdisplay, mode.vdisplay, GBM_FORMAT_XRGB8888,
				      GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING) : NULL;
	if (!gs)
		return fprintf(stderr, "glkms: no GBM surface\n"), 2;
	get_display = (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
	dpy = get_display ? get_display(EGL_PLATFORM_GBM_KHR, gbm, NULL) : NULL;
	if (!dpy || !eglInitialize(dpy, NULL, NULL) || !eglBindAPI(EGL_OPENGL_ES_API))
		return fprintf(stderr, "glkms: no EGL on GBM\n"), 2;
	eglChooseConfig(dpy, config_attrs, configs, 64, &n);
	for (int i = 0; i < n && !config; i++) {
		EGLint id;

		if (eglGetConfigAttrib(dpy, configs[i], EGL_NATIVE_VISUAL_ID, &id) &&
		    id == GBM_FORMAT_XRGB8888)
			config = configs[i];
	}
	if (!config)
		return fprintf(stderr, "glkms: no XRGB8888 config (%d configs)\n", n), 2;
	ctx = eglCreateContext(dpy, config, EGL_NO_CONTEXT, ctx_attrs);
	surf = eglCreateWindowSurface(dpy, config, (EGLNativeWindowType)gs, NULL);
	if (ctx == EGL_NO_CONTEXT || surf == EGL_NO_SURFACE || !eglMakeCurrent(dpy, surf, surf, ctx))
		return fprintf(stderr, "glkms: no context or window surface (0x%x)\n",
			       eglGetError()), 2;
	printf("GL_RENDERER %s\n", glGetString(GL_RENDERER));

	p = glCreateProgram();
	for (int i = 0; i < 2; i++) {
		GLuint s = glCreateShader(i ? GL_FRAGMENT_SHADER : GL_VERTEX_SHADER);

		glShaderSource(s, 1, i ? &fs_src : &vs_src, NULL);
		glCompileShader(s);
		glAttachShader(p, s);
	}
	glBindAttribLocation(p, 0, "p");
	glBindAttribLocation(p, 1, "c");
	glLinkProgram(p);
	glUseProgram(p);
	la = glGetUniformLocation(p, "a");
	glEnableVertexAttribArray(0);
	glEnableVertexAttribArray(1);
	glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 0, pos);
	glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, 0, col);
	glViewport(0, 0, mode.hdisplay, mode.vdisplay);

	for (int f = -1; f <= frames; f++) {
		int last = f == frames;

		if (f == 0)
			t0 = now();
		still(pos, col);
		glUniform1f(la, last ? 0 : f * 0.05f);
		glClearColor(0.1f, 0.1f, 0.3f, 1);
		glClear(GL_COLOR_BUFFER_BIT);
		if (last) {
			glDrawArrays(GL_TRIANGLES, 0, 12);
		} else {
			/* the gradient still, the square turning */
			glUniform1f(la, 0);
			glDrawArrays(GL_TRIANGLES, 0, 6);
			glUniform1f(la, f * 0.05f);
			glDrawArrays(GL_TRIANGLES, 6, 6);
		}
		eglSwapBuffers(dpy, surf);
		bo = gbm_surface_lock_front_buffer(gs);
		if (!shown) {
			if (drmModeSetCrtc(fd, crtc_id, fb_for(bo), 0, 0, &conn_id, 1, &mode))
				return perror("glkms: drmModeSetCrtc"), 1;
		} else {
			flip_pending = 1;
			if (drmModePageFlip(fd, crtc_id, fb_for(bo), DRM_MODE_PAGE_FLIP_EVENT, NULL))
				return perror("glkms: drmModePageFlip"), 1;
			while (flip_pending)
				drmHandleEvent(fd, &ev);
			gbm_surface_release_buffer(gs, shown);
		}
		shown = bo;
	}
	printf("%d frames in %.2f s: %.1f frames a second\n", frames, now() - t0,
	       frames / (now() - t0));
	wrong = check_screen(mode.hdisplay, mode.vdisplay);
	if (argc > 2)
		sleep(atoi(argv[2]));
	eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	eglTerminate(dpy);
	return wrong ? 1 : 0;
}
