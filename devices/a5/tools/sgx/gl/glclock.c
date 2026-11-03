/* glclock -- the GPU's clock, measured by its arithmetic: a 768 x 1024 target
 * filled with fragment programs of N dependent multiply-adds (x = x * a + b,
 * a and b uniforms, x from gl_FragCoord, so nothing folds or moves to the
 * vertices), N = 0 and the Ns given, each drawn
 * DRAWS times and finished.  The time N adds over 0, a pixel and a
 * multiply-add, is a number of instructions a second -- two each, a
 * multiply and an add, as the compiler makes them (SGX_DEBUG_SHADER=1) --
 * and the SGX543MP2 issues one a clock in each of its 2 x 4 pipelines, so
 * that over 8 is the clock.  Measured: 2.0 G instructions a second, 250
 * MHz (docs/research/p105-gpu.md, "The GPU's clock, measured").
 *
 *   sgx-gl glclock [DRAWS [N...]]          (defaults 10, 64 256)
 */
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define W 768
#define H 1024

static double now(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

static GLuint shader(GLenum type, const char *src)
{
	GLuint s = glCreateShader(type);
	GLint ok;
	char log[512];

	glShaderSource(s, 1, &src, NULL);
	glCompileShader(s);
	glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		glGetShaderInfoLog(s, sizeof(log), NULL, log);
		fprintf(stderr, "glclock: %s\n", log);
		exit(1);
	}
	return s;
}

/* the program with n multiply-adds */
static GLuint program(int n)
{
	static const char *vs =
		"attribute vec2 pos;\n"
		"varying float v;\n"
		"void main() { v = pos.x; gl_Position = vec4(pos, 0.0, 1.0); }\n";
	char *fs = malloc(256 + n * 24), *p = fs;
	GLuint prog = glCreateProgram();

	/* (from gl_FragCoord, not the varying: math linear in a varying is
	 * moved to the vertices and interpolated) */
	p += sprintf(p, "precision highp float;\nvarying float v;\nuniform float a, b;\n"
		     "void main() {\n float x = gl_FragCoord.x * 0.001 + v;\n");
	for (int i = 0; i < n; i++)
		p += sprintf(p, " x = x * a + b;\n");
	sprintf(p, " gl_FragColor = vec4(x);\n}\n");
	glAttachShader(prog, shader(GL_VERTEX_SHADER, vs));
	glAttachShader(prog, shader(GL_FRAGMENT_SHADER, fs));
	glBindAttribLocation(prog, 0, "pos");
	glLinkProgram(prog);
	glUseProgram(prog);
	/* (not 1 and 0: the driver inlines a uniform's value, and x * 1 + 0
	 * folds away; x * 0.999 + 0.001 does not) */
	glUniform1f(glGetUniformLocation(prog, "a"), 0.999f);
	glUniform1f(glGetUniformLocation(prog, "b"), 0.001f);
	free(fs);
	return prog;
}

/* seconds a draw, the target filled once */
static double fill(int n, int draws)
{
	double t;

	program(n);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);	/* (the first: compiling, uploading) */
	glFinish();
	t = now();
	for (int i = 0; i < draws; i++) {
		glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
		glFinish();
	}
	return (now() - t) / draws;
}

int main(int argc, char **argv)
{
	static const float quad[] = { -1, -1, 1, -1, -1, 1, 1, 1 };
	PFNEGLGETPLATFORMDISPLAYEXTPROC get_display;
	EGLint ctx_attrs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
	int draws = argc > 1 ? atoi(argv[1]) : 10;
	int ns[16] = { 64, 256 }, nn = 2;
	GLuint fbo, rt;
	EGLDisplay dpy;
	EGLContext ctx;
	double t0;

	if (argc > 2)
		for (nn = 0; nn < 16 && nn + 2 < argc; nn++)
			ns[nn] = atoi(argv[nn + 2]);
	get_display = (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
	dpy = get_display ? get_display(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL) : NULL;
	if (!dpy || !eglInitialize(dpy, NULL, NULL) || !eglBindAPI(EGL_OPENGL_ES_API))
		return fprintf(stderr, "glclock: no EGL\n"), 2;
	ctx = eglCreateContext(dpy, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, ctx_attrs);
	if (ctx == EGL_NO_CONTEXT || !eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx))
		return fprintf(stderr, "glclock: no context\n"), 2;
	printf("%s\n", glGetString(GL_RENDERER));

	glGenTextures(1, &rt);
	glBindTexture(GL_TEXTURE_2D, rt);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	glGenFramebuffers(1, &fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, rt, 0);
	glViewport(0, 0, W, H);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, quad);
	glEnableVertexAttribArray(0);

	t0 = fill(0, draws);
	printf("N 0: %.2f ms a fill\n", t0 * 1e3);
	for (int i = 0; i < nn; i++) {
		double t = fill(ns[i], draws), per = (t - t0) / ((double)W * H * ns[i]);

		printf("N %d: %.2f ms a fill; %.3f ns a pixel's multiply-add: %.0f M instructions "
		       "a second, %.1f MHz at 8 a clock\n", ns[i], t * 1e3, per * 1e9,
		       2e-6 / per, 2e-6 / per / 8);
	}
	return 0;
}
