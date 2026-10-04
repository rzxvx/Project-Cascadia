/* gltri -- the Mesa driver's first draws (docs/research/p105-mesa.md,
 * M13a): OpenGL ES 2.0 through EGL with no window, a framebuffer object the
 * size of the screen, and three draws whose pixels are read back and
 * checked:
 *
 *   1. a triangle with a colour per vertex (red, green, blue), the vertex
 *      shader passing the position and the colour through;
 *   2. a rectangle (a triangle strip) in a uniform's colour;
 *   3. a triangle turned a quarter round by the vertex shader (a uniform
 *      mat2), in a constant colour.
 *
 *   sgx-gl gltri [--fb] [--ppm FILE]
 *
 * The vertex shaders run on the CPU (the draw module) and the triangles are
 * drawn by the GPU through the template frame; with --fb the result is
 * also copied to /dev/fb0 (echo 0 > .../vtcon1/bind first), with --ppm
 * written to FILE as a PPM image (top row first, as it should look).  Exit
 * status 0 when every check passed.
 */
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>

#include <fcntl.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define W 768
#define H 1024

static uint8_t *px;
static int checks, bad;

static double now(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

static int fail(const char *what)
{
	fprintf(stderr, "gltri: %s (EGL 0x%x, GL 0x%x)\n", what, eglGetError(), glGetError());
	return 1;
}

static GLuint program(const char *vs_src, const char *fs_src)
{
	const char *src[2] = { vs_src, fs_src };
	GLenum type[2] = { GL_VERTEX_SHADER, GL_FRAGMENT_SHADER };
	GLuint p = glCreateProgram();
	char log[1024];
	GLint ok;

	for (int i = 0; i < 2; i++) {
		GLuint s = glCreateShader(type[i]);

		glShaderSource(s, 1, &src[i], NULL);
		glCompileShader(s);
		glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
		if (!ok) {
			glGetShaderInfoLog(s, sizeof(log), NULL, log);
			fprintf(stderr, "gltri: shader: %s\n", log);
			exit(1);
		}
		glAttachShader(p, s);
	}
	glBindAttribLocation(p, 0, "p");
	glBindAttribLocation(p, 1, "c");
	glLinkProgram(p);
	glGetProgramiv(p, GL_LINK_STATUS, &ok);
	if (!ok) {
		glGetProgramInfoLog(p, sizeof(log), NULL, log);
		fprintf(stderr, "gltri: link: %s\n", log);
		exit(1);
	}
	return p;
}

/* the pixel at x, y in [-1, 1] (GL's: y up), against want +- tol (0..255) */
static void check(const char *what, float x, float y, const float want[4], int tol)
{
	int ix = (int)((x + 1) / 2 * W), iy = (int)((y + 1) / 2 * H);
	const uint8_t *p = px + (iy * W + ix) * 4;
	int ok = 1;

	for (int ch = 0; ch < 4; ch++) {
		int d = p[ch] - (int)(want[ch] * 255 + 0.5f);

		if (d < -tol || d > tol)
			ok = 0;
	}
	checks++;
	if (!ok)
		bad++;
	printf("  %-34s pixel %3d,%4d: %02x %02x %02x %02x, want %02x %02x %02x %02x %s\n",
	       what, ix, iy, p[0], p[1], p[2], p[3], (int)(want[0] * 255 + 0.5f),
	       (int)(want[1] * 255 + 0.5f), (int)(want[2] * 255 + 0.5f),
	       (int)(want[3] * 255 + 0.5f), ok ? "ok" : "WRONG");
}

static void read_back(void)
{
	glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px);
}

int main(int argc, char **argv)
{
	static const char *vs_pass =
		"attribute vec4 p;\n"
		"attribute vec4 c;\n"
		"varying vec4 v;\n"
		"void main() { gl_Position = p; v = c; }\n";
	static const char *fs_varying =
		"precision mediump float;\n"
		"varying vec4 v;\n"
		"void main() { gl_FragColor = v; }\n";
	static const char *vs_plain =
		"attribute vec4 p;\n"
		"void main() { gl_Position = p; }\n";
	static const char *fs_uniform =
		"precision mediump float;\n"
		"uniform vec4 u;\n"
		"void main() { gl_FragColor = u; }\n";
	static const char *vs_turn =
		"attribute vec4 p;\n"
		"uniform mat2 m;\n"
		"void main() { gl_Position = vec4(m * p.xy, 0.0, 1.0); }\n";
	static const char *fs_const =
		"precision mediump float;\n"
		"void main() { gl_FragColor = vec4(1.0, 1.0, 0.0, 1.0); }\n";
	static const float tri[] = { -0.8f, -0.8f, 0, 1,  0.8f, -0.8f, 0, 1,  0, 0.8f, 0, 1 };
	static const float tri_c[] = { 1, 0, 0, 1,  0, 1, 0, 1,  0, 0, 1, 1 };
	static const float rect[] = { -0.9f, 0.5f, 0, 1,  -0.5f, 0.5f, 0, 1,
	                              -0.9f, 0.9f, 0, 1,  -0.5f, 0.9f, 0, 1 };
	/* a thin triangle along +x, at x 0.5 .. 0.9; turned a quarter round
	 * (anticlockwise) it lies along +y */
	static const float thin[] = { 0.5f, -0.05f, 0, 1,  0.9f, 0, 0, 1,  0.5f, 0.05f, 0, 1 };
	static const float black[4] = { 0, 0, 0, 1 }, red[4] = { 1, 0, 0, 1 },
		green[4] = { 0, 1, 0, 1 }, blue[4] = { 0, 0, 1, 1 },
		third[4] = { 1 / 3.0f, 1 / 3.0f, 1 / 3.0f, 1 },
		orange[4] = { 1, 0.5f, 0.25f, 1 }, yellow[4] = { 1, 1, 0, 1 };
	static const float turn[4] = { 0, 1, -1, 0 };    /* column-major: (x, y) -> (-y, x) */
	PFNEGLGETPLATFORMDISPLAYEXTPROC get_display;
	EGLint ctx_attrs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
	const char *ppm = NULL;
	int to_fb = 0;
	EGLDisplay dpy;
	EGLContext ctx;
	GLuint tex, fbo, p1, p2, p3;
	double t0, t[3];

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--fb"))
			to_fb = 1;
		else if (!strcmp(argv[i], "--ppm") && i + 1 < argc)
			ppm = argv[++i];
		else {
			fprintf(stderr, "usage: gltri [--fb] [--ppm FILE]\n");
			return 2;
		}
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
	printf("GL_RENDERER %s\n", glGetString(GL_RENDERER));

	glGenTextures(1, &tex);
	glBindTexture(GL_TEXTURE_2D, tex);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	glGenFramebuffers(1, &fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
	if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
		return fail("the framebuffer object is not complete");
	glViewport(0, 0, W, H);
	px = malloc(W * H * 4);

	p1 = program(vs_pass, fs_varying);
	p2 = program(vs_plain, fs_uniform);
	p3 = program(vs_turn, fs_const);

	glClearColor(0, 0, 0, 1);
	glClear(GL_COLOR_BUFFER_BIT);

	/* 1: red, green, blue corners */
	t0 = now();
	glUseProgram(p1);
	glEnableVertexAttribArray(0);
	glEnableVertexAttribArray(1);
	glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 0, tri);
	glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, 0, tri_c);
	glDrawArrays(GL_TRIANGLES, 0, 3);
	glFinish();
	t[0] = now() - t0;
	read_back();
	printf("1. a triangle, a colour per vertex (%.1f ms)\n", t[0] * 1000);
	check("near the red corner", -0.75f, -0.77f, red, 40);
	check("near the green corner", 0.75f, -0.77f, green, 40);
	check("near the blue corner", 0, 0.75f, blue, 40);
	check("the centroid", 0, -0.8f / 3, third, 12);
	check("outside, top left", -0.9f, 0.0f, black, 2);
	check("outside, bottom right", 0.9f, 0.9f, black, 2);

	/* 2: a uniform's colour */
	t0 = now();
	glUseProgram(p2);
	glDisableVertexAttribArray(1);
	glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 0, rect);
	glUniform4fv(glGetUniformLocation(p2, "u"), 1, orange);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	glFinish();
	t[1] = now() - t0;
	read_back();
	printf("2. a rectangle in a uniform's colour (%.1f ms)\n", t[1] * 1000);
	check("inside", -0.7f, 0.7f, orange, 2);
	check("the triangle from 1 still there", 0, -0.8f / 3, third, 12);
	check("outside, right of it", -0.45f, 0.7f, black, 2);

	/* 3: turned by the vertex shader */
	t0 = now();
	glUseProgram(p3);
	glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 0, thin);
	glUniformMatrix2fv(glGetUniformLocation(p3, "m"), 1, GL_FALSE, turn);
	glDrawArrays(GL_TRIANGLES, 0, 3);
	glFinish();
	t[2] = now() - t0;
	read_back();
	printf("3. a triangle turned a quarter round (%.1f ms)\n", t[2] * 1000);
	check("where it was turned to", 0, 0.6f, yellow, 2);
	check("where it was before", 0.6f, 0, black, 2);

	printf("%d of %d checks right\n", checks - bad, checks);

	if (ppm) {
		FILE *fp = fopen(ppm, "wb");

		if (!fp)
			return fail("cannot write the PPM file");
		fprintf(fp, "P6\n%d %d\n255\n", W, H);
		for (int y = H - 1; y >= 0; y--)
			for (int x = 0; x < W; x++)
				fwrite(px + (y * W + x) * 4, 1, 3, fp);
		fclose(fp);
		printf("the last read-back: %s\n", ppm);
	}
	if (to_fb) {
		int fd = open("/dev/fb0", O_WRONLY);
		uint8_t *row = malloc(W * 4);

		if (fd < 0)
			return fail("no /dev/fb0");
		/* GL's row 0 is the bottom; the framebuffer is B G R A */
		for (int y = 0; y < H; y++) {
			const uint8_t *s = px + (H - 1 - y) * W * 4;

			for (int x = 0; x < W; x++) {
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
	return bad ? 1 : 0;
}
