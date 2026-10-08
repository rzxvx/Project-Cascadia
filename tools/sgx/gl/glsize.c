/* glsize -- render targets of any size (docs/research/p105-mesa.md, M10):
 * for each size, a texture of that size as the target, a gradient over the
 * whole of it (red across, green down, from a varying) and a blue square
 * over its middle half, and every pixel read back and checked.  The sizes
 * run past the frame's slots for render target data (eight) and come back
 * to the first ones.
 *
 *   sgx-gl glsize [--ppm DIR] [WxH...]
 */
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *sizes[] = {
	"64x64", "100x50", "256x128", "128x256", "300x200", "33x17", "1x1", "768x1024",
	"1024x768", "512x512", "2048x2048", "64x64", "300x200", "31x33", "768x1024",
};

static const char *vs_src =
	"attribute vec4 p;\n"
	"attribute vec4 c;\n"
	"varying vec4 v;\n"
	"void main() { v = c; gl_Position = p; }\n";
static const char *fs_src =
	"precision highp float;\n"
	"varying vec4 v;\n"
	"void main() { gl_FragColor = v; }\n";

/* a quad from (x0, y0) to (x1, y1), six vertices; its colour from the
 * position (r = x / 2 + 1 / 2, g = y / 2 + 1 / 2), or c */
static void quad(float *p, float *col, float x0, float y0, float x1, float y1, const float *c)
{
	static const float cx[6] = { 0, 1, 1, 0, 1, 0 }, cy[6] = { 0, 0, 1, 0, 1, 1 };

	for (int i = 0; i < 6; i++) {
		float x = x0 + (x1 - x0) * cx[i], y = y0 + (y1 - y0) * cy[i];

		p[4 * i + 0] = x;
		p[4 * i + 1] = y;
		p[4 * i + 2] = 0;
		p[4 * i + 3] = 1;
		col[4 * i + 0] = c ? c[0] : x / 2 + 0.5f;
		col[4 * i + 1] = c ? c[1] : y / 2 + 0.5f;
		col[4 * i + 2] = c ? c[2] : 0;
		col[4 * i + 3] = 1;
	}
}

static void ppm(const char *dir, int w, int h, const uint8_t *px)
{
	char path[512];
	FILE *f;

	snprintf(path, sizeof(path), "%s/glsize_%dx%d.ppm", dir, w, h);
	if (!(f = fopen(path, "wb")))
		return;
	fprintf(f, "P6\n%d %d\n255\n", w, h);
	for (int i = 0; i < w * h; i++)
		fwrite(px + 4 * i, 1, 3, f);
	fclose(f);
}

int main(int argc, char **argv)
{
	static const float blue[3] = { 0, 0, 1 };
	PFNEGLGETPLATFORMDISPLAYEXTPROC get_display;
	EGLint ctx_attrs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
	const char *dir = NULL, **list = sizes;
	int n = sizeof(sizes) / sizeof(sizes[0]), failed = 0;
	float pos[12 * 4], col[12 * 4];
	EGLDisplay dpy;
	EGLContext ctx;
	GLuint p;

	if (argc > 2 && !strcmp(argv[1], "--ppm")) {
		dir = argv[2];
		argc -= 2;
		argv += 2;
	}
	if (argc > 1) {
		list = (const char **)argv + 1;
		n = argc - 1;
	}

	get_display = (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
	dpy = get_display ? get_display(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL) : NULL;
	if (!dpy || !eglInitialize(dpy, NULL, NULL) || !eglBindAPI(EGL_OPENGL_ES_API))
		return fprintf(stderr, "glsize: no EGL\n"), 2;
	ctx = eglCreateContext(dpy, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, ctx_attrs);
	if (ctx == EGL_NO_CONTEXT || !eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx))
		return fprintf(stderr, "glsize: no GLES 2 context\n"), 2;

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
	glEnableVertexAttribArray(0);
	glEnableVertexAttribArray(1);
	glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 0, pos);
	glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, 0, col);
	quad(pos, col, -1, -1, 1, 1, NULL);
	quad(pos + 24, col + 24, -0.5f, -0.5f, 0.5f, 0.5f, blue);

	for (int k = 0; k < n; k++) {
		int w, h, wrong = 0, fx = -1, fy = -1;
		uint8_t *px, got[4] = { 0 }, want[4] = { 0 };
		GLuint rt, fbo;

		if (sscanf(list[k], "%dx%d", &w, &h) != 2 || w < 1 || h < 1)
			return fprintf(stderr, "glsize: %s is not WxH\n", list[k]), 2;
		px = malloc((size_t)w * h * 4);
		glGenTextures(1, &rt);
		glBindTexture(GL_TEXTURE_2D, rt);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
		glGenFramebuffers(1, &fbo);
		glBindFramebuffer(GL_FRAMEBUFFER, fbo);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, rt, 0);
		glViewport(0, 0, w, h);
		glClearColor(1, 0, 1, 1);
		glClear(GL_COLOR_BUFFER_BIT);
		glDrawArrays(GL_TRIANGLES, 0, 12);
		memset(px, 0x55, (size_t)w * h * 4);
		glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);

		for (int y = 0; y < h; y++)
			for (int x = 0; x < w; x++) {
				const uint8_t *q = px + ((size_t)y * w + x) * 4;
				/* the pixel's centre, and whether it is in the square
				 * (centres exactly on its edge go either way) */
				float cx = (x + 0.5f) / w, cy = (y + 0.5f) / h;
				int in = cx > 0.25f && cx < 0.75f && cy > 0.25f && cy < 0.75f;
				int edge = (cx == 0.25f || cx == 0.75f) || (cy == 0.25f || cy == 0.75f);
				uint8_t e[4] = { in ? 0 : (uint8_t)(cx * 255 + 0.5f),
						 in ? 0 : (uint8_t)(cy * 255 + 0.5f), in ? 255 : 0, 255 };
				int bad = 0;

				for (int c = 0; c < 4; c++)
					bad |= abs(q[c] - e[c]) > 2;
				if (bad && edge)
					continue;
				if (bad && !wrong++) {
					fx = x;
					fy = y;
					memcpy(got, q, 4);
					memcpy(want, e, 4);
				}
			}
		if (dir)
			ppm(dir, w, h, px);
		failed += !!wrong;
		if (wrong)
			printf("%-10s WRONG: %d of %d pixels; the first at %d,%d: %02x%02x%02x%02x, "
			       "not %02x%02x%02x%02x\n", list[k], wrong, w * h, fx, fy, got[0], got[1],
			       got[2], got[3], want[0], want[1], want[2], want[3]);
		else
			printf("%-10s ok\n", list[k]);
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
		glDeleteFramebuffers(1, &fbo);
		glDeleteTextures(1, &rt);
		free(px);
	}
	printf("%d of %d sizes right\n", n - failed, n);
	eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	eglTerminate(dpy);
	return failed ? 1 : 0;
}
