/* glldr -- uniforms past what sa holds (docs/research/p105-mesa.md, M32):
 * read from memory by the USSE's loads (VLDST).  Fragment shaders with an
 * array of 40 vec4s read through an index from a uniform and from a
 * varying (each column of pixels its own element) and read straight past
 * sa's words; an array of 40 constants read through an index (dEQP's
 * tmp_array.*const_write*); a vertex shader with 120 vec4s; values that
 * change between two draws of a render and between two renders.  Every
 * pixel read back against what C works out.
 *
 *   sgx-gl glldr
 *   SGX_LDR_PROBE=word,... sgx-gl glldr --probe    (sgx_compiler.c: the
 *       program those words, WDF0, r0 into o0 -- a 4 x 4 texture's state in
 *       sa0..3, texel i the bytes i, 0x40 + i, 0x80 + i, 0xc0 + i; prints
 *       the pixels' bytes; GLLDR_FS=source: that fragment shader instead,
 *       its uniform int n GLLDR_N, 2 by default)
 */
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W 64
#define H 64
#define N 40            /* the fragment shaders' arrays */
#define NV 120          /* the vertex shader's */

static uint8_t px[W * H * 4];
static int cases, failed;

static GLuint
program(const char *vs, const char *fs)
{
	GLuint p = glCreateProgram();
	char log[512];
	GLint ok;

	for (int i = 0; i < 2; i++) {
		GLuint s = glCreateShader(i ? GL_FRAGMENT_SHADER : GL_VERTEX_SHADER);

		glShaderSource(s, 1, i ? &fs : &vs, NULL);
		glCompileShader(s);
		glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
		if (!ok) {
			glGetShaderInfoLog(s, sizeof(log), NULL, log);
			fprintf(stderr, "glldr: %s\n", log);
		}
		glAttachShader(p, s);
	}
	glBindAttribLocation(p, 0, "a_position");
	glLinkProgram(p);
	glGetProgramiv(p, GL_LINK_STATUS, &ok);
	if (!ok) {
		glGetProgramInfoLog(p, sizeof(log), NULL, log);
		fprintf(stderr, "glldr: %s\n", log);
	}
	glUseProgram(p);
	return p;
}

/* element i of the arrays: four values of its own, 0..1 */
static void
element(int i, float v[4])
{
	for (int c = 0; c < 4; c++)
		v[c] = ((i * 37 + c * 11) % 97) / 96.0f;
}

static void
set_array(GLuint p, const char *name, int n)
{
	float (*a)[4] = malloc(n * sizeof(*a));

	for (int i = 0; i < n; i++)
		element(i, a[i]);
	glUniform4fv(glGetUniformLocation(p, name), n, &a[0][0]);
	free(a);
}

static int
to8(float v)
{
	return (int)(fminf(fmaxf(v, 0), 1) * 255 + 0.5f);
}

/* the pixels read back against want(x, y); draw() drew them */
static void
check(const char *name, void (*want)(int x, int y, const void *arg, float rgba[4]), const void *arg)
{
	int bad = 0, at = -1;
	float rgba[4];

	glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px);
	for (int i = 0; i < W * H; i++) {
		int off = 0;

		want(i % W, i / W, arg, rgba);
		for (int c = 0; c < 4; c++)
			off |= abs(px[4 * i + c] - to8(rgba[c])) > 2;
		if (off && at < 0)
			at = i;
		bad += off;
	}
	cases++;
	if (!bad) {
		printf("%-44s ok\n", name);
		return;
	}
	failed++;
	want(at % W, at / W, arg, rgba);
	printf("%-44s WRONG: %d pixels, the first at %d, %d: %d %d %d %d, want %d %d %d %d\n", name,
	       bad, at % W, at / W, px[4 * at], px[4 * at + 1], px[4 * at + 2], px[4 * at + 3],
	       to8(rgba[0]), to8(rgba[1]), to8(rgba[2]), to8(rgba[3]));
}

static void
draw(void)
{
	glClearColor(0, 0, 0, 0);
	glClear(GL_COLOR_BUFFER_BIT);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

/* element *(int *)arg everywhere */
static void
want_element(int x, int y, const void *arg, float rgba[4])
{
	(void)x, (void)y;
	element(*(const int *)arg, rgba);
}

/* column x: element (x + 0.5) / W * N */
static void
want_column(int x, int y, const void *arg, float rgba[4])
{
	(void)y, (void)arg;
	element((2 * x + 1) * N / (2 * W), rgba);
}

/* elements 3 + 37 (and 0's alpha) */
static void
want_direct(int x, int y, const void *arg, float rgba[4])
{
	float a[4], b[4];

	(void)x, (void)y, (void)arg;
	element(3, a);
	element(37, b);
	for (int c = 0; c < 4; c++)
		rgba[c] = 0.5f * (a[c] + b[c]);
}

/* the constant array: element i is (i + 1) / 64 * (1, 0.5, 0.25, 1) */
static void
want_const(int x, int y, const void *arg, float rgba[4])
{
	int i = *(const int *)arg;

	(void)x, (void)y;
	rgba[0] = (i + 1) / 64.0f;
	rgba[1] = (i + 1) / 128.0f;
	rgba[2] = (i + 1) / 256.0f;
	rgba[3] = 1;
}

static void
want_vs(int x, int y, const void *arg, float rgba[4])
{
	float a[4], b[4];

	(void)x, (void)y, (void)arg;
	element(17, a);
	element(113, b);
	for (int c = 0; c < 4; c++)
		rgba[c] = 0.5f * (a[c] + b[c]);
}

static int
probe(void)
{
	static const char *vs = "attribute vec4 a_position; varying vec2 v;\n"
	                        "void main() { gl_Position = a_position; v = a_position.xy * 0.5 + 0.5; }\n";
	static const char *fs = "precision mediump float; varying vec2 v; uniform sampler2D t;\n"
	                        "void main() { gl_FragColor = texture2D(t, v); }\n";
	uint8_t texels[16][4];
	GLuint tex, p;

	for (int i = 0; i < 16; i++) {
		texels[i][0] = i;
		texels[i][1] = 0x40 + i;
		texels[i][2] = 0x80 + i;
		texels[i][3] = 0xc0 + i;
	}
	glActiveTexture(GL_TEXTURE0);
	glGenTextures(1, &tex);
	glBindTexture(GL_TEXTURE_2D, tex);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 4, 4, 0, GL_RGBA, GL_UNSIGNED_BYTE, texels);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	p = program(vs, getenv("GLLDR_FS") ? getenv("GLLDR_FS") : fs);
	glUniform1i(glGetUniformLocation(p, "t"), 0);
	glUniform1i(glGetUniformLocation(p, "n"), getenv("GLLDR_N") ? atoi(getenv("GLLDR_N")) : 2);
	draw();
	glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px);
	/* a pixel of each quarter */
	for (int q = 0; q < 4; q++) {
		const uint8_t *c = px + (((q / 2) * H / 2 + H / 4) * W + (q % 2) * W / 2 + W / 4) * 4;

		printf("%s%02x %02x %02x %02x", q ? "  " : "pixels: ", c[0], c[1], c[2], c[3]);
	}
	printf("\n");
	return 0;
}

int
main(int argc, char **argv)
{
	static const char *vs = "attribute vec4 a_position; varying vec2 v;\n"
	                        "void main() { gl_Position = a_position; v = a_position.xy * 0.5 + 0.5; }\n";
	static const float quad[] = { -1, -1, 1, -1, -1, 1, 1, 1 };
	PFNEGLGETPLATFORMDISPLAYEXTPROC get_display;
	EGLint ctx_attrs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
	char fs[8192];
	GLuint rt, fbo, p;
	EGLDisplay dpy;
	EGLContext ctx;
	int k;

	get_display = (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
	dpy = get_display ? get_display(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL) : NULL;
	if (!dpy || !eglInitialize(dpy, NULL, NULL) || !eglBindAPI(EGL_OPENGL_ES_API))
		return fprintf(stderr, "glldr: no EGL\n"), 2;
	ctx = eglCreateContext(dpy, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, ctx_attrs);
	if (ctx == EGL_NO_CONTEXT || !eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx))
		return fprintf(stderr, "glldr: no GLES 2 context\n"), 2;
	glGenTextures(1, &rt);
	glBindTexture(GL_TEXTURE_2D, rt);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	glGenFramebuffers(1, &fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, rt, 0);
	glViewport(0, 0, W, H);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, quad);
	if (argc > 1 && !strcmp(argv[1], "--probe"))
		return probe();

	/* an array of 40 vec4s through a uniform index: elements past sa too */
	snprintf(fs, sizeof(fs), "precision mediump float; uniform vec4 u[%d]; uniform int i;\n"
	         "void main() { gl_FragColor = u[i]; }\n", N);
	p = program(vs, fs);
	set_array(p, "u", N);
	for (int i = 0; i < N; i += 13) {
		char name[64];

		glUniform1i(glGetUniformLocation(p, "i"), i);
		draw();
		snprintf(name, sizeof(name), "u[%d] of %d through a uniform index", i, N);
		check(name, want_element, &i);
	}
	k = N - 1;
	glUniform1i(glGetUniformLocation(p, "i"), k);
	draw();
	check("u[39] of 40 through a uniform index", want_element, &k);

	/* through an index from a varying: each column its own */
	snprintf(fs, sizeof(fs), "precision highp float; uniform vec4 u[%d]; varying vec2 v;\n"
	         "void main() { gl_FragColor = u[int(v.x * %d.0)]; }\n", N, N);
	p = program(vs, fs);
	set_array(p, "u", N);
	draw();
	check("u[x] of 40, x from a varying", want_column, NULL);

	/* read straight: one in sa, one past it */
	snprintf(fs, sizeof(fs), "precision mediump float; uniform vec4 u[%d];\n"
	         "void main() { gl_FragColor = 0.5 * (u[3] + u[37]); }\n", N);
	p = program(vs, fs);
	set_array(p, "u", N);
	draw();
	check("u[3] + u[37] of 40", want_direct, NULL);

	/* an array of 40 constants through an index (GLSL's linker makes it
	 * uniforms) */
	{
		int m = snprintf(fs, sizeof(fs), "precision mediump float; uniform int i;\n"
		                 "void main() { vec4 a[%d];\n", N);

		for (int j = 0; j < N; j++)
			m += snprintf(fs + m, sizeof(fs) - m, "  a[%d] = vec4(%d.0 / 64.0, %d.0 / 128.0, "
			              "%d.0 / 256.0, 1.0);\n", j, j + 1, j + 1, j + 1);
		snprintf(fs + m, sizeof(fs) - m, "  gl_FragColor = a[i]; }\n");
		p = program(vs, fs);
		for (int i = 0; i < N; i += 19) {
			char name[64];

			glUniform1i(glGetUniformLocation(p, "i"), i);
			draw();
			snprintf(name, sizeof(name), "constant a[%d] of %d through an index", i, N);
			check(name, want_const, &i);
		}
	}

	/* a vertex shader's 120 vec4s, read through an index and straight */
	{
		char vsu[1024];
		static const char *fsv = "precision mediump float; varying vec4 c;\n"
		                         "void main() { gl_FragColor = c; }\n";

		snprintf(vsu, sizeof(vsu), "attribute vec4 a_position; uniform vec4 u[%d]; uniform int i;\n"
		         "varying vec4 c;\n"
		         "void main() { gl_Position = a_position; c = 0.5 * (u[i] + u[113]); }\n", NV);
		p = program(vsu, fsv);
		set_array(p, "u", NV);
		glUniform1i(glGetUniformLocation(p, "i"), 17);
		draw();
		check("a vertex shader's u[i] + u[113] of 120", want_vs, NULL);
	}

	/* changed between two draws of a render (the left half, then the
	 * right), then in the next render */
	{
		static const float left[] = { -1, -1, 0, -1, -1, 1, 0, 1 };
		static const float right[] = { 0, -1, 1, -1, 0, 1, 1, 1 };
		int a = 7, b = 33;

		snprintf(fs, sizeof(fs), "precision mediump float; uniform vec4 u[%d]; uniform int i;\n"
		         "void main() { gl_FragColor = u[i]; }\n", N);
		p = program(vs, fs);
		set_array(p, "u", N);
		glClear(GL_COLOR_BUFFER_BIT);
		for (int half = 0; half < 2; half++) {
			float e[4];

			element(half ? b : a, e);
			glUniform4fv(glGetUniformLocation(p, "u[30]"), 1, e);
			glUniform1i(glGetUniformLocation(p, "i"), 30);
			glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, half ? right : left);
			glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
		}
		glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px);
		{
			int bad = 0;

			for (int i = 0; i < W * H; i++) {
				float e[4];

				element(i % W < W / 2 ? a : b, e);
				for (int c = 0; c < 4; c++)
					bad += abs(px[4 * i + c] - to8(e[c])) > 2;
			}
			cases++;
			failed += bad != 0;
			printf("%-44s %s\n", "u[30] changed between two draws", bad ? "WRONG" : "ok");
		}
		glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, quad);
		for (int r = 0; r < 3; r++) {
			float e[4];
			int want = 11 + r;

			element(want, e);
			glUniform4fv(glGetUniformLocation(p, "u[30]"), 1, e);
			draw();
			check(r ? "u[30] changed again, the next render" : "u[30] in the next render",
			      want_element, &want);
		}
	}

	printf("%d of %d cases right\n", cases - failed, cases);
	eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	eglTerminate(dpy);
	return failed ? 1 : 0;
}
