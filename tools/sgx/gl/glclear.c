/* glclear -- the Mesa driver's first check: OpenGL ES 2.0 through EGL with
 * no window (EGL_MESA_platform_surfaceless), a framebuffer object the size
 * of the screen, glClear, and the pixels read back.
 *
 *   sgx-gl glclear [N] [--fb]
 *
 * Clears N times (default 1) through a cycle of colours, reads the result
 * back each time and checks it; with --fb the last frame is also copied to
 * /dev/fb0 (the console is in the way: echo 0 > .../vtcon1/bind first).
 * A full-screen clear is a render on the GPU when the template frame is
 * installed (./cascadia gpu), and done by the CPU otherwise; the driver
 * says which when it starts.
 *
 * docs/research/p105-mesa.md, M12.  Exit status 0 when every pixel was right.
 */
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define W 768
#define H 1024

static double now(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

static int fail(const char *what)
{
	fprintf(stderr, "glclear: %s (EGL 0x%x, GL 0x%x)\n", what, eglGetError(), glGetError());
	return 1;
}

int main(int argc, char **argv)
{
	static const float colours[][4] = {
		{ 1, 0, 0, 1 }, { 0, 1, 0, 1 }, { 0, 0, 1, 1 }, { 1, 1, 1, 1 },
		{ 0.2f, 0.4f, 0.6f, 1 }, { 0, 0, 0, 0 },
	};
	PFNEGLGETPLATFORMDISPLAYEXTPROC get_display;
	EGLint ctx_attrs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
	int n = 1, to_fb = 0, bad_frames = 0, i;
	EGLDisplay dpy;
	EGLContext ctx;
	GLuint tex, fbo;
	uint8_t *px;
	double t0, t;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--fb"))
			to_fb = 1;
		else
			n = atoi(argv[i]) > 0 ? atoi(argv[i]) : 1;
	}

	get_display = (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
	if (!get_display)
		return fail("no eglGetPlatformDisplayEXT");
	dpy = get_display(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
	if (dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, NULL, NULL))
		return fail("no surfaceless EGL display");
	if (!eglBindAPI(EGL_OPENGL_ES_API))
		return fail("no OpenGL ES");
	ctx = eglCreateContext(dpy, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, ctx_attrs);
	if (ctx == EGL_NO_CONTEXT || !eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx))
		return fail("no GLES 2 context");
	printf("GL_RENDERER %s\nGL_VERSION  %s\nGL_VENDOR   %s\n",
	       glGetString(GL_RENDERER), glGetString(GL_VERSION), glGetString(GL_VENDOR));

	glGenTextures(1, &tex);
	glBindTexture(GL_TEXTURE_2D, tex);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	glGenFramebuffers(1, &fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
	if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
		return fail("the framebuffer object is not complete");

	px = malloc(W * H * 4);
	t0 = now();
	for (i = 0; i < n; i++) {
		const float *c = colours[i % (sizeof(colours) / sizeof(colours[0]))];
		uint8_t want[4];
		int bad = 0, k;

		glClearColor(c[0], c[1], c[2], c[3]);
		glClear(GL_COLOR_BUFFER_BIT);
		glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px);
		for (k = 0; k < 4; k++)
			want[k] = (uint8_t)(c[k] * 255.0f + 0.5f);
		for (k = 0; k < W * H; k++) {
			int ch;

			for (ch = 0; ch < 4; ch++) {
				int d = px[k * 4 + ch] - want[ch];

				if (d < -1 || d > 1)
					break;
			}
			if (ch < 4 && bad++ < 3)
				fprintf(stderr, "  frame %d pixel %d,%d: %02x %02x %02x %02x, want %02x %02x %02x %02x\n",
					i, k % W, k / W, px[k * 4], px[k * 4 + 1], px[k * 4 + 2], px[k * 4 + 3],
					want[0], want[1], want[2], want[3]);
		}
		if (bad) {
			fprintf(stderr, "  frame %d: %d of %d pixels wrong\n", i, bad, W * H);
			bad_frames++;
		}
	}
	t = now() - t0;
	printf("%d clear%s with read-back in %.3f s: %.1f ms each; %d wrong\n",
	       n, n == 1 ? "" : "s", t, t * 1000 / n, bad_frames);

	if (to_fb) {
		int fd = open("/dev/fb0", O_WRONLY);
		uint8_t *row = malloc(W * 4);
		int y, x;

		if (fd < 0)
			return fail("no /dev/fb0");
		/* GL's row 0 is the bottom; the framebuffer is B G R A */
		for (y = 0; y < H; y++) {
			const uint8_t *s = px + (H - 1 - y) * W * 4;

			for (x = 0; x < W; x++) {
				row[x * 4 + 0] = s[x * 4 + 2];
				row[x * 4 + 1] = s[x * 4 + 1];
				row[x * 4 + 2] = s[x * 4 + 0];
				row[x * 4 + 3] = s[x * 4 + 3];
			}
			if (pwrite(fd, row, W * 4, (off_t)y * W * 4) != W * 4)
				return fail("writing /dev/fb0");
		}
		close(fd);
		free(row);
	}
	eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	eglTerminate(dpy);
	free(px);
	return bad_frames ? 1 : 0;
}
