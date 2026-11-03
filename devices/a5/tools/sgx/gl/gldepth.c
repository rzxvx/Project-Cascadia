/* gldepth -- the depth test (docs/research/p105-mesa.md, M14): two quads
 * that overlap in the middle third of the screen, a near one (green, z =
 * -0.5) over the left two thirds and a far one (red, z = 0.5) over the
 * right two thirds, drawn into a target with a depth buffer.  Three pixels
 * are read: left (only the near quad), right (only the far one), middle
 * (both: what the depth test leaves).
 *
 *   sgx-gl gldepth [CASE...]     (a name, or a prefix; all by default)
 */
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W 768
#define H 1024

enum { NEAR, FAR, NONE };

static const struct test {
	const char *name;
	int test;               /* GL_DEPTH_TEST */
	GLenum func;
	GLboolean mask;
	int first, split;       /* which quad first; each in a draw of its own */
	int middle;             /* what the middle should be */
	float clear;            /* the depth clear value (0: 1.0) */
	int right;              /* what the right should be (0: FAR) */
	int finish;             /* split, a glFinish between: two renders, the depth
	                         * stored and loaded between them (M24) */
} tests[] = {
	{ "off", 0, GL_LESS, 1, NEAR, 0, FAR },
	{ "less", 1, GL_LESS, 1, NEAR, 0, NEAR },
	{ "less_far_first", 1, GL_LESS, 1, FAR, 0, NEAR },
	{ "lequal", 1, GL_LEQUAL, 1, FAR, 0, NEAR },
	{ "always", 1, GL_ALWAYS, 1, NEAR, 0, FAR },
	{ "nowrite", 1, GL_LESS, 0, NEAR, 0, FAR },
	{ "two_draws", 1, GL_LESS, 1, NEAR, 1, NEAR },
	{ "two_draws_far_first", 1, GL_LESS, 1, FAR, 1, NEAR },
	/* cleared to 0.5: the far quad (depth 0.75) fails LESS everywhere */
	{ "clear_half", 1, GL_LESS, 1, FAR, 0, NEAR, 0.5f, NONE },
	{ "two_renders", 1, GL_LESS, 1, NEAR, 1, NEAR, 0, 0, 1 },
	{ "two_renders_far_first", 1, GL_LESS, 1, FAR, 1, NEAR, 0, 0, 1 },
	{ "two_renders_clear_half", 1, GL_LESS, 1, NEAR, 1, NEAR, 0.5f, NONE, 1 },
};

static const char *vs_src =
	"attribute vec4 p;\n"
	"attribute vec4 c;\n"
	"varying vec4 v;\n"
	"void main() { v = c; gl_Position = p; }\n";
static const char *fs_src =
	"precision mediump float;\n"
	"varying vec4 v;\n"
	"void main() { gl_FragColor = v; }\n";

/* a quad from x0 to x1 at depth z in colour c, six vertices */
static void quad(float *p, float *c, float x0, float x1, float z, const float col[4])
{
	static const float cx[6] = { 0, 1, 1, 0, 1, 0 }, cy[6] = { 0, 0, 1, 0, 1, 1 };

	for (int i = 0; i < 6; i++) {
		p[4 * i + 0] = x0 + (x1 - x0) * cx[i];
		p[4 * i + 1] = -0.8f + 1.6f * cy[i];
		p[4 * i + 2] = z;
		p[4 * i + 3] = 1;
		memcpy(c + 4 * i, col, 4 * sizeof(float));
	}
}

static int wanted(int argc, char **argv, const char *name)
{
	if (argc < 2)
		return 1;
	for (int i = 1; i < argc; i++)
		if (!strncmp(name, argv[i], strlen(argv[i])))
			return 1;
	return 0;
}

int main(int argc, char **argv)
{
	static const float green[4] = { 0, 1, 0, 1 }, red[4] = { 1, 0, 0, 1 };
	static const char *what[3] = { "near (green)", "far (red)", "nothing" };
	PFNEGLGETPLATFORMDISPLAYEXTPROC get_display;
	EGLint ctx_attrs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
	uint8_t *px = malloc(W * H * 4);
	float pos[12 * 4], col[12 * 4];
	int run = 0, failed = 0;
	EGLDisplay dpy;
	EGLContext ctx;
	GLuint rt, depth, fbo, p;

	get_display = (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
	dpy = get_display ? get_display(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL) : NULL;
	if (!dpy || !eglInitialize(dpy, NULL, NULL) || !eglBindAPI(EGL_OPENGL_ES_API))
		return fprintf(stderr, "gldepth: no EGL\n"), 2;
	ctx = eglCreateContext(dpy, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, ctx_attrs);
	if (ctx == EGL_NO_CONTEXT || !eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx))
		return fprintf(stderr, "gldepth: no GLES 2 context\n"), 2;

	glGenTextures(1, &rt);
	glBindTexture(GL_TEXTURE_2D, rt);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	glGenRenderbuffers(1, &depth);
	glBindRenderbuffer(GL_RENDERBUFFER, depth);
	glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, W, H);
	glGenFramebuffers(1, &fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, rt, 0);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depth);
	if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
		return fprintf(stderr, "gldepth: the framebuffer object is not complete\n"), 2;
	glViewport(0, 0, W, H);

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

	for (unsigned k = 0; k < sizeof(tests) / sizeof(tests[0]); k++) {
		const struct test *T = &tests[k];
		static const int xs[3] = { W / 6, W / 2, 5 * W / 6 };
		int ok = 1, got[3];

		if (!wanted(argc, argv, T->name))
			continue;
		run++;
		/* the near quad x -1 .. 1/3, the far one -1/3 .. 1, in drawing order */
		quad(pos + (T->first == NEAR ? 0 : 24), col + (T->first == NEAR ? 0 : 24),
		     -1, 1 / 3.0f, -0.5f, green);
		quad(pos + (T->first == NEAR ? 24 : 0), col + (T->first == NEAR ? 24 : 0),
		     -1 / 3.0f, 1, 0.5f, red);
		glDisable(GL_DEPTH_TEST);
		glDepthMask(GL_TRUE);
		glClearColor(0, 0, 0, 1);
		glClearDepthf(T->clear ? T->clear : 1);
		glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
		if (T->test)
			glEnable(GL_DEPTH_TEST);
		glDepthFunc(T->func);
		glDepthMask(T->mask);
		if (T->split) {
			glDrawArrays(GL_TRIANGLES, 0, 6);
			if (T->finish)
				glFinish();
			glDrawArrays(GL_TRIANGLES, 6, 6);
		} else {
			glDrawArrays(GL_TRIANGLES, 0, 12);
		}
		glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px);
		for (int i = 0; i < 3; i++) {
			const uint8_t *q = px + (H / 2 * W + xs[i]) * 4;

			got[i] = q[1] > 200 && q[0] < 50 ? NEAR : q[0] > 200 && q[1] < 50 ? FAR :
				 q[0] < 50 && q[1] < 50 ? NONE : -1;
		}
		ok = got[0] == NEAR && got[2] == (T->right ? T->right : FAR) && got[1] == T->middle;
		failed += !ok;
		printf("%-20s %s: left %s, middle %s (want %s), right %s (want %s)\n", T->name,
		       ok ? "ok   " : "WRONG", got[0] < 0 ? "?" : what[got[0]],
		       got[1] < 0 ? "?" : what[got[1]], what[T->middle],
		       got[2] < 0 ? "?" : what[got[2]], what[T->right ? T->right : FAR]);
	}
	printf("%d of %d cases right\n", run - failed, run);
	eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	eglTerminate(dpy);
	free(px);
	return failed ? 1 : 0;
}
