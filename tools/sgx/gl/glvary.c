/* glvary -- varyings (docs/research/p105-mesa.md, M30): 1 to 8 vec4
 * varyings, each component its own linear function of the position, read
 * back one at a time (the rest added times a uniform 0, so that the linker
 * keeps them) and all of them averaged; then dEQP's shaders.matrix
 * "dynamic" cases' shape: a mat3 attribute (three columns from arrays of
 * their own, stride 16) and a float, both varyings -- 10 floats the linker
 * packs into three vec4s -- summed in the fragment shader; whole numbers
 * through a varying, as floats and as ints (M31: dEQP's int operators), in
 * a 128 x 112 viewport too, and ints divided.  A grid of 4 x 4 quads (32
 * triangles); every pixel read back against the same sums worked out in C.
 *
 *   sgx-gl glvary [--ppm out.ppm]     (--ppm: the last case's pixels)
 */
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W 128
#define H 128
#define N 4             /* quads a side */
#define V ((N + 1) * (N + 1))

static uint8_t px[W * H * 4];
static float pos[V][4], col[3][V][4], coords[V][4], whole[V][4];
static uint16_t idx[N * N * 6];
static int cases, failed;
static int vp_h = H;     /* the viewport's height: rows above it are left clear */

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
			fprintf(stderr, "glvary: %s\n", log);
		}
		glAttachShader(p, s);
	}
	glBindAttribLocation(p, 0, "a_position");
	glBindAttribLocation(p, 1, "a_mat3");      /* 1, 2, 3 */
	glBindAttribLocation(p, 4, "a_coords");
	glBindAttribLocation(p, 5, "a_whole");
	glLinkProgram(p);
	glUseProgram(p);
	return p;
}

/* varying j's component c at (sx, sy), 0..1 across the target */
static float
vary(int j, int c, float sx, float sy)
{
	return (j + 1) / 10.0f + 0.04f * sx + 0.03f * c * sy;
}

/* column c of the mat3 at (sx, sy) */
static void
column(int c, float sx, float sy, float out[4])
{
	out[0] = 0.1f * c + 0.2f * sx;
	out[1] = 0.05f + 0.1f * sy - 0.03f * c;
	out[2] = 0.15f * sx * sy + 0.02f * c;
	out[3] = 1.0f;
}

static int
to8(float v)
{
	return (int)(fminf(fmaxf(v, 0), 1) * 255 + 0.5f);
}

/* the grid drawn, every pixel read back against want(sx, sy) */
static void
check(const char *name, void (*want)(float sx, float sy, const void *arg, float rgba[4]),
      const void *arg)
{
	int bad = 0, at = -1;
	float rgba[4];

	glClearColor(0, 0, 0, 0);
	glClear(GL_COLOR_BUFFER_BIT);
	glDrawElements(GL_TRIANGLES, N * N * 6, GL_UNSIGNED_SHORT, idx);
	glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px);
	for (int i = 0; i < W * H; i++) {
		int off = 0;

		if (i / W >= vp_h)
			rgba[0] = rgba[1] = rgba[2] = rgba[3] = 0;
		else
			want((i % W + 0.5f) / W, (i / W + 0.5f) / vp_h, arg, rgba);
		for (int c = 0; c < 4; c++)
			off |= abs(px[4 * i + c] - to8(rgba[c])) > 3;
		if (off && at < 0)
			at = i;
		bad += off;
	}
	cases++;
	if (!bad) {
		printf("%-36s ok\n", name);
		return;
	}
	failed++;
	want((at % W + 0.5f) / W, (at / W + 0.5f) / vp_h, arg, rgba);
	printf("%-36s WRONG: %d pixels, the first at %d, %d: %d %d %d %d, want %d %d %d %d\n", name,
	       bad, at % W, at / W, px[4 * at], px[4 * at + 1], px[4 * at + 2], px[4 * at + 3],
	       to8(rgba[0]), to8(rgba[1]), to8(rgba[2]), to8(rgba[3]));
}

struct pick {
	int n, k;       /* k: the varying out, or -1 for the average of all */
};

static void
want_vary(float sx, float sy, const void *arg, float rgba[4])
{
	const struct pick *p = arg;

	for (int c = 0; c < 4; c++) {
		rgba[c] = 0;
		for (int j = 0; j < p->n; j++)
			if (p->k < 0 || j == p->k)
				rgba[c] += vary(j, c, sx, sy);
		if (p->k < 0)
			rgba[c] /= p->n;
	}
}

static void
varyings(int n, int k)
{
	char vs[2048], fs[2048], name[64];
	struct pick p = { n, k };
	int m = 0;

	m += snprintf(vs + m, sizeof(vs) - m, "attribute vec4 a_position;\n");
	for (int j = 0; j < n; j++)
		m += snprintf(vs + m, sizeof(vs) - m, "varying vec4 v%d;\n", j);
	m += snprintf(vs + m, sizeof(vs) - m, "void main() {\n  gl_Position = a_position;\n"
	              "  vec2 s = a_position.xy * 0.5 + 0.5;\n");
	for (int j = 0; j < n; j++)
		m += snprintf(vs + m, sizeof(vs) - m, "  v%d = vec4(%d.0 / 10.0 + 0.04 * s.x) + "
		              "vec4(0.0, 0.03, 0.06, 0.09) * s.y;\n", j, j + 1);
	snprintf(vs + m, sizeof(vs) - m, "}\n");
	m = snprintf(fs, sizeof(fs), "precision mediump float;\nuniform float u_zero;\n");
	for (int j = 0; j < n; j++)
		m += snprintf(fs + m, sizeof(fs) - m, "varying vec4 v%d;\n", j);
	if (k >= 0)
		m += snprintf(fs + m, sizeof(fs) - m, "void main() { gl_FragColor = v%d + u_zero * (v0", k);
	else
		m += snprintf(fs + m, sizeof(fs) - m, "void main() { gl_FragColor = (1.0 / %d.0) * (v0", n);
	for (int j = 1; j < n; j++)
		m += snprintf(fs + m, sizeof(fs) - m, " + v%d", j);
	snprintf(fs + m, sizeof(fs) - m, "); }\n");
	glUniform1f(glGetUniformLocation(program(vs, fs), "u_zero"), 0);
	if (k >= 0)
		snprintf(name, sizeof(name), "%d varyings, varying %d", n, k);
	else
		snprintf(name, sizeof(name), "%d varyings, averaged", n);
	check(name, want_vary, &p);
}

static void
want_zero(float sx, float sy, const void *arg, float rgba[4])
{
	(void)sx, (void)sy, (void)arg;
	rgba[0] = rgba[1] = rgba[2] = rgba[3] = 0;
}

static void
want_quotients(float sx, float sy, const void *arg, float rgba[4])
{
	(void)sx, (void)sy, (void)arg;
	for (int c = 0; c < 4; c++)
		rgba[c] = 24 / (c + 1) * 0.04f;
}

static void
want_quotients2(float sx, float sy, const void *arg, float rgba[4])
{
	(void)arg;
	for (int c = 0; c < 4; c++)
		rgba[c] = 24 / -(int)(-4 + 3 * (c & 1 ? sy : sx)) * 0.04f;
}

static void
want_quotients3(float sx, float sy, const void *arg, float rgba[4])
{
	want_quotients2(sx, sy, arg, rgba);
	for (int c = 0; c < 4; c++)
		rgba[c] = 1 - rgba[c];
}

static void
want_02(float sx, float sy, const void *arg, float rgba[4])
{
	(void)sx, (void)sy, (void)arg;
	rgba[0] = rgba[1] = rgba[2] = rgba[3] = 0.2f;
}

static void
want_06(float sx, float sy, const void *arg, float rgba[4])
{
	(void)sx, (void)sy, (void)arg;
	rgba[0] = rgba[1] = rgba[2] = rgba[3] = 0.6f;
}

static void
want_matrix(float sx, float sy, const void *arg, float rgba[4])
{
	float k = 0.1f * sx + 0.05f, cv[4];

	(void)arg;
	rgba[0] = rgba[1] = rgba[2] = 0;
	rgba[3] = 0.5f;
	for (int c = 0; c < 3; c++) {
		column(c, sx, sy, cv);
		for (int r = 0; r < 3; r++)
			rgba[r] += (cv[r] + k) * 0.5f;
	}
}

int
main(int argc, char **argv)
{
	static const char *vs =
		"attribute highp vec4 a_position;\n"
		"attribute mediump mat3 a_mat3;\n"
		"attribute mediump float a_coords;\n"
		"varying mediump mat3 v_mat3;\n"
		"varying mediump float v_coords;\n"
		"void main() { gl_Position = a_position; v_mat3 = a_mat3; v_coords = a_coords; }\n";
	static const char *fs =
		"precision mediump float;\n"
		"varying mediump mat3 v_mat3;\n"
		"varying mediump float v_coords;\n"
		"void main() {\n"
		"  mediump mat3 res = v_mat3 + v_coords;\n"
		"  gl_FragColor = vec4(res[0] + res[1] + res[2], 1.0) * 0.5;\n"
		"}\n";
	PFNEGLGETPLATFORMDISPLAYEXTPROC get_display;
	EGLint ctx_attrs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
	const char *ppm = argc > 2 && !strcmp(argv[1], "--ppm") ? argv[2] : NULL;
	GLuint rt, fbo;
	EGLDisplay dpy;
	EGLContext ctx;

	get_display = (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
	dpy = get_display ? get_display(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL) : NULL;
	if (!dpy || !eglInitialize(dpy, NULL, NULL) || !eglBindAPI(EGL_OPENGL_ES_API))
		return fprintf(stderr, "glvary: no EGL\n"), 2;
	ctx = eglCreateContext(dpy, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, ctx_attrs);
	if (ctx == EGL_NO_CONTEXT || !eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx))
		return fprintf(stderr, "glvary: no GLES 2 context\n"), 2;
	glGenTextures(1, &rt);
	glBindTexture(GL_TEXTURE_2D, rt);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	glGenFramebuffers(1, &fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, rt, 0);
	glViewport(0, 0, W, H);

	for (int y = 0; y <= N; y++)
		for (int x = 0; x <= N; x++) {
			int v = y * (N + 1) + x;
			float sx = (float)x / N, sy = (float)y / N;

			pos[v][0] = sx * 2 - 1;
			pos[v][1] = sy * 2 - 1;
			pos[v][2] = 0;
			pos[v][3] = 1;
			for (int c = 0; c < 3; c++)
				column(c, sx, sy, col[c][v]);
			coords[v][0] = 0.1f * sx + 0.05f;
			coords[v][1] = coords[v][2] = coords[v][3] = 7;
			whole[v][0] = 24;
			whole[v][1] = -3;
			whole[v][2] = 7;
			whole[v][3] = 100;
		}
	for (int y = 0; y < N; y++)
		for (int x = 0; x < N; x++) {
			uint16_t *t = idx + (y * N + x) * 6, a = y * (N + 1) + x;

			t[0] = a;
			t[1] = a + 1;
			t[2] = a + N + 1;
			t[3] = a + N + 1;
			t[4] = a + 1;
			t[5] = a + N + 2;
		}
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 0, pos);

	for (int n = 1; n <= 8; n++) {
		varyings(n, 0);
		if (n > 1)
			varyings(n, n - 1);
		if (n > 2)
			varyings(n, n / 2);
		varyings(n, -1);
	}

	for (int c = 0; c < 3; c++) {
		glEnableVertexAttribArray(1 + c);
		glVertexAttribPointer(1 + c, 3, GL_FLOAT, GL_FALSE, 16, col[c]);
	}
	glEnableVertexAttribArray(4);
	glVertexAttribPointer(4, 4, GL_FLOAT, GL_FALSE, 0, coords);
	program(vs, fs);
	check("a mat3 and a float, packed", want_matrix, NULL);

	/* the same at every vertex: the same at every pixel, to the last bit
	 * here -- and as an int (dEQP's int operators: ivec4(v) of 24 at every
	 * vertex, 24) */
	glEnableVertexAttribArray(5);
	glVertexAttribPointer(5, 4, GL_FLOAT, GL_FALSE, 0, whole);
	program("attribute vec4 a_position; attribute vec4 a_whole; varying vec4 v_whole;\n"
	        "void main() { gl_Position = a_position; v_whole = a_whole; }\n",
	        "precision highp float; varying vec4 v_whole;\n"
	        "void main() { gl_FragColor = abs(v_whole - vec4(24.0, -3.0, 7.0, 100.0)) * 1e5; }\n");
	check("whole numbers through a varying", want_zero, NULL);
	program("attribute vec4 a_position; attribute vec4 a_whole; varying vec4 v_whole;\n"
	        "void main() { gl_Position = a_position; v_whole = a_whole; }\n",
	        "precision mediump float; varying vec4 v_whole;\n"
	        "void main() { gl_FragColor = abs(vec4(ivec4(v_whole)) - vec4(24.0, -3.0, 7.0, 100.0)); }\n");
	check("whole numbers through a varying, as ints", want_zero, NULL);
	/* (dEQP's viewport: there 24 comes 23.9995 -- M31) */
	glViewport(0, 0, W, vp_h = 112);
	check("the same, 128 x 112", want_zero, NULL);
	glViewport(0, 0, W, vp_h = H);
	{
		GLuint p = program("attribute vec4 a_position; attribute vec4 a_whole; varying vec4 v_whole;\n"
		                   "void main() { gl_Position = a_position; v_whole = a_whole; }\n",
		                   "precision mediump float; varying vec4 v_whole; uniform ivec4 u_div;\n"
		                   "void main() { ivec4 q = ivec4(v_whole.xxxx) / u_div;\n"
		                   "  gl_FragColor = vec4(q) * -0.04; }\n");

		glUniform4i(glGetUniformLocation(p, "u_div"), -1, -2, -3, -4);
		check("24 / -1, -2, -3, -4 as ints", want_quotients, NULL);
		program("attribute vec4 a_position; attribute vec4 a_whole; varying vec4 v_whole;\n"
		        "varying vec4 v_div;\n"
		        "void main() { gl_Position = a_position; v_whole = a_whole;\n"
		        "  v_div = vec4(-4.0) + 3.0 * (a_position.xyxy * 0.5 + 0.5); }\n",
		        "precision mediump float; varying vec4 v_whole; varying vec4 v_div;\n"
		        "void main() { ivec4 q = ivec4(v_whole.xxxx) / ivec4(v_div);\n"
		        "  gl_FragColor = vec4(q) * -0.04; }\n");
		check("24 / int(-4..-1) as ints", want_quotients2, NULL);
		program("attribute vec4 a_position; attribute vec4 a_whole; varying mediump vec4 v_in0;\n"
		        "varying mediump vec4 v_in1;\n"
		        "void main() { gl_Position = a_position; v_in0 = a_whole.xxxx;\n"
		        "  v_in1 = (vec4(-4.0) + 3.0 * (a_position.xyxy * 0.5 + 0.5)).wzyx; }\n",
		        "varying mediump vec4 v_in0;\nvarying mediump vec4 v_in1;\n"
		        "void main()\n{\n\tmediump ivec4 in0 = ivec4(v_in0.yzwx);\n"
		        "\tmediump ivec4 in1 = ivec4(v_in1.wzyx);\n\tmediump ivec4 res = ivec4(0.0);\n"
		        "\tres = in0 / in1;\n\tmediump vec4 color = vec4(0.0, 0.0, 0.0, 1.0);\n"
		        "\tcolor.xyzw = vec4(res);\n\tcolor = color * 0.04 + 1.00;\n"
		        "\tgl_FragColor = color;\n}\n");
		check("dEQP's div.mediump_ivec4_fragment", want_quotients3, NULL);
		glViewport(0, 0, W, vp_h = 112);
		check("the same, 128 x 112", want_quotients3, NULL);
		glViewport(0, 0, W, vp_h = H);
	}

	/* a select on a uniform bool (dEQP's loops' select_iteration_count) */
	for (int t = 0; t < 2; t++) {
		GLuint p = program("attribute vec4 a_position;\n"
		                   "void main() { gl_Position = a_position; }\n",
		                   "precision mediump float; uniform bool u_t;\n"
		                   "void main() { gl_FragColor = vec4(u_t ? 0.2 : 0.6); }\n");

		glUniform1i(glGetUniformLocation(p, "u_t"), t);
		check(t ? "a uniform bool's select, true" : "a uniform bool's select, false",
		      t ? want_02 : want_06, NULL);
	}

	if (ppm) {
		FILE *f = fopen(ppm, "wb");

		fprintf(f, "P6 %d %d 255\n", W, H);
		for (int y = H - 1; y >= 0; y--)
			for (int x = 0; x < W; x++)
				fwrite(px + (y * W + x) * 4, 1, 3, f);
		fclose(f);
	}
	printf("%d of %d cases right\n", cases - failed, cases);
	eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	eglTerminate(dpy);
	return failed ? 1 : 0;
}
