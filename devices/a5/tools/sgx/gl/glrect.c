/* glrect -- draws into part of a target, where the scissor or the viewport
 * puts them (docs/research/p105-mesa.md, M46: written for renders of a
 * rectangle's tiles alone -- tried, right, and no faster, so not kept; the
 * cases stay, for the clip rectangles every draw gets).  Each case clears
 * the target red, draws, and checks every pixel: green where the draw
 * went, red everywhere else.
 *
 *   scissor       a 256 x 256 target, a full-screen quad scissored to 64 x
 *                 64 at 96, 64 (whole tiles)
 *   scissor-odd   scissored to 50 x 40 at 100, 70 (the tiles around it
 *                 rendered, the rest of them kept)
 *   viewport      no scissor, the viewport 64 x 48 at 128, 160
 *   texture       the viewport 64 x 64 at 64, 32, a 4 x 4 checker texture
 *                 over it (nearest): each texel's 16 x 16 pixels
 *   two           two scissored quads far apart in one render, 32 x 32 at
 *                 32, 32 and at 192, 192
 *   edge          a 250 x 170 target, scissored to 50 x 30 at 200, 140 (the
 *                 last tiles partial)
 *   fragcoord     a scissored quad whose colour is gl_FragCoord's (the
 *                 PDS iterates the pixel's place in the render)
 *   copy          glCopyTexSubImage2D of 40 x 24 into a 256 x 256 texture at
 *                 150, 100, from a rendered target
 *
 *   sgx-gl glrect [CASE...]     (a name, or a prefix; all by default)
 *
 * Exit status 0 when every case passed.
 */
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int run, failed;

static const char *vs_src =
	"attribute vec4 p;\n"
	"varying vec2 st;\n"
	"void main() { st = p.xy * 0.5 + 0.5; gl_Position = p; }\n";
static const char *fs_green =
	"precision highp float;\n"
	"void main() { gl_FragColor = vec4(0.0, 1.0, 0.0, 1.0); }\n";
static const char *fs_tex =
	"precision highp float;\n"
	"varying vec2 st;\n"
	"uniform sampler2D t;\n"
	"void main() { gl_FragColor = texture2D(t, st); }\n";
static const char *fs_coord =
	"precision highp float;\n"
	"void main() { gl_FragColor = vec4(gl_FragCoord.x / 256.0, gl_FragCoord.y / 256.0, 0.0, 1.0); }\n";

static const float quad[12] = { -1, -1, 1, -1, 1, 1, -1, -1, 1, 1, -1, 1 };

static GLuint build(const char *fs)
{
	const char *src[2] = { vs_src, fs };
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
			fprintf(stderr, "glrect: shader: %s\n", log);
			exit(2);
		}
		glAttachShader(p, s);
	}
	glBindAttribLocation(p, 0, "p");
	glLinkProgram(p);
	glGetProgramiv(p, GL_LINK_STATUS, &ok);
	if (!ok) {
		glGetProgramInfoLog(p, sizeof(log), NULL, log);
		fprintf(stderr, "glrect: link: %s\n", log);
		exit(2);
	}
	return p;
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

/* a target of w x h (texture and framebuffer), cleared red */
static GLuint target(int w, int h, GLuint *tex)
{
	GLuint fbo;

	glGenTextures(1, tex);
	glBindTexture(GL_TEXTURE_2D, *tex);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glGenFramebuffers(1, &fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, *tex, 0);
	glViewport(0, 0, w, h);
	glDisable(GL_SCISSOR_TEST);
	glClearColor(1, 0, 0, 1);
	glClear(GL_COLOR_BUFFER_BIT);
	glFinish();
	return fbo;
}

/* the pixel expected at x, y (rows from the bottom, GL's) */
typedef void (*expect_fn)(int x, int y, uint8_t want[4], const void *arg);

struct rect { int x, y, w, h; };

static int inside(const struct rect *r, int x, int y)
{
	return x >= r->x && x < r->x + r->w && y >= r->y && y < r->y + r->h;
}

/* green in the rectangles (a list ended by w 0), red elsewhere */
static void expect_rects(int x, int y, uint8_t want[4], const void *arg)
{
	const struct rect *r = arg;
	int in = 0;

	for (; r->w; r++)
		in |= inside(r, x, y);
	want[0] = in ? 0 : 255;
	want[1] = in ? 255 : 0;
	want[2] = 0;
	want[3] = 255;
}

static void check(const char *name, int w, int h, expect_fn fn, const void *arg, int tol)
{
	uint8_t *px = malloc(w * h * 4), want[4];
	int worst = 0, wx = 0, wy = 0, bad = 0;
	uint8_t got_at[4] = { 0 }, want_at[4] = { 0 };

	glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++) {
			const uint8_t *q = px + (y * w + x) * 4;
			int e = 0;

			fn(x, y, want, arg);
			for (int i = 0; i < 4; i++)
				if (abs(q[i] - want[i]) > e)
					e = abs(q[i] - want[i]);
			if (e > tol)
				bad++;
			if (e > worst) {
				worst = e, wx = x, wy = y;
				memcpy(got_at, q, 4);
				memcpy(want_at, want, 4);
			}
		}
	run++;
	if (bad) {
		failed++;
		printf("%-12s WRONG: %d pixels off, the worst at %d, %d: %d %d %d %d, not %d %d %d %d\n",
		       name, bad, wx, wy, got_at[0], got_at[1], got_at[2], got_at[3],
		       want_at[0], want_at[1], want_at[2], want_at[3]);
	} else {
		printf("%-12s ok (largest difference %d)\n", name, worst);
	}
	free(px);
}

/* a full-screen quad through p, scissored to each rectangle given (w 0
 * ends the list; none: not scissored) -- one render */
static void draw_quads(GLuint p, const struct rect *r)
{
	glUseProgram(p);
	if (!r) {
		glDrawArrays(GL_TRIANGLES, 0, 6);
		return;
	}
	glEnable(GL_SCISSOR_TEST);
	for (; r->w; r++) {
		glScissor(r->x, r->y, r->w, r->h);
		glDrawArrays(GL_TRIANGLES, 0, 6);
	}
	glDisable(GL_SCISSOR_TEST);
}

static const uint8_t checker[4][4][4] = {
	{ { 255, 255, 255, 255 }, { 0, 0, 255, 255 }, { 255, 255, 0, 255 }, { 0, 128, 0, 255 } },
	{ { 0, 0, 255, 255 }, { 255, 255, 255, 255 }, { 0, 128, 0, 255 }, { 255, 255, 0, 255 } },
	{ { 255, 0, 255, 255 }, { 0, 255, 255, 255 }, { 64, 64, 64, 255 }, { 200, 100, 50, 255 } },
	{ { 0, 255, 255, 255 }, { 255, 0, 255, 255 }, { 200, 100, 50, 255 }, { 64, 64, 64, 255 } },
};

static void expect_texture(int x, int y, uint8_t want[4], const void *arg)
{
	const struct rect *r = arg;

	if (!inside(r, x, y)) {
		const uint8_t red[4] = { 255, 0, 0, 255 };

		memcpy(want, red, 4);
		return;
	}
	memcpy(want, checker[(y - r->y) * 4 / r->h][(x - r->x) * 4 / r->w], 4);
}

static void expect_coord(int x, int y, uint8_t want[4], const void *arg)
{
	const struct rect *r = arg;

	if (!inside(r, x, y)) {
		want[0] = 255, want[1] = 0, want[2] = 0, want[3] = 255;
		return;
	}
	want[0] = (int)((x + 0.5f) / 256 * 255 + 0.5f);
	want[1] = (int)((y + 0.5f) / 256 * 255 + 0.5f);
	want[2] = 0;
	want[3] = 255;
}

int main(int argc, char **argv)
{
	PFNEGLGETPLATFORMDISPLAYEXTPROC get_display;
	EGLint ctx_attrs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
	EGLDisplay dpy;
	EGLContext ctx;
	GLuint green, tex_prog, coord, rt;

	get_display = (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
	dpy = get_display ? get_display(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL) : NULL;
	if (!dpy || !eglInitialize(dpy, NULL, NULL) || !eglBindAPI(EGL_OPENGL_ES_API))
		return fprintf(stderr, "glrect: no EGL\n"), 2;
	ctx = eglCreateContext(dpy, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, ctx_attrs);
	if (ctx == EGL_NO_CONTEXT || !eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx))
		return fprintf(stderr, "glrect: no GLES 2 context\n"), 2;
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, quad);
	green = build(fs_green);
	tex_prog = build(fs_tex);
	coord = build(fs_coord);

	if (wanted(argc, argv, "scissor")) {
		const struct rect r[] = { { 96, 64, 64, 64 }, { 0 } };

		target(256, 256, &rt);
		draw_quads(green, r);
		check("scissor", 256, 256, expect_rects, r, 0);
	}
	if (wanted(argc, argv, "scissor-odd")) {
		const struct rect r[] = { { 100, 70, 50, 40 }, { 0 } };

		target(256, 256, &rt);
		draw_quads(green, r);
		check("scissor-odd", 256, 256, expect_rects, r, 0);
	}
	if (wanted(argc, argv, "viewport")) {
		const struct rect r[] = { { 128, 160, 64, 48 }, { 0 } };

		target(256, 256, &rt);
		glViewport(128, 160, 64, 48);
		draw_quads(green, NULL);
		check("viewport", 256, 256, expect_rects, r, 0);
	}
	if (wanted(argc, argv, "texture")) {
		const struct rect r = { 64, 32, 64, 64 };
		GLuint t;

		target(256, 256, &rt);
		glGenTextures(1, &t);
		glBindTexture(GL_TEXTURE_2D, t);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 4, 4, 0, GL_RGBA, GL_UNSIGNED_BYTE, checker);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		glUseProgram(tex_prog);
		glUniform1i(glGetUniformLocation(tex_prog, "t"), 0);
		glViewport(r.x, r.y, r.w, r.h);
		draw_quads(tex_prog, NULL);
		check("texture", 256, 256, expect_texture, &r, 0);
	}
	if (wanted(argc, argv, "two")) {
		const struct rect r[] = { { 32, 32, 32, 32 }, { 192, 192, 32, 32 }, { 0 } };

		target(256, 256, &rt);
		draw_quads(green, r);
		check("two", 256, 256, expect_rects, r, 0);
	}
	if (wanted(argc, argv, "edge")) {
		const struct rect r[] = { { 200, 140, 50, 30 }, { 0 } };

		target(250, 170, &rt);
		draw_quads(green, r);
		check("edge", 250, 170, expect_rects, r, 0);
	}
	if (wanted(argc, argv, "fragcoord")) {
		const struct rect r[] = { { 96, 64, 64, 64 }, { 0 } };

		target(256, 256, &rt);
		draw_quads(coord, r);
		check("fragcoord", 256, 256, expect_coord, &r[0], 1);
	}
	if (wanted(argc, argv, "copy")) {
		/* a rendered source (green over red, the left half), copied into
		 * the middle of a texture cleared red: green where the source was */
		const struct rect r[] = { { 150, 100, 20, 24 }, { 0 } };
		GLuint src, dst, src_fbo;

		src_fbo = target(64, 64, &src);
		{
			const struct rect half[] = { { 0, 0, 32, 64 }, { 0 } };

			draw_quads(green, half);
		}
		target(256, 256, &dst);
		glBindFramebuffer(GL_FRAMEBUFFER, src_fbo);
		glBindTexture(GL_TEXTURE_2D, dst);
		/* 40 x 24 from 12, 20 of the source: its x 12..31 green, 32..51 red */
		glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 150, 100, 12, 20, 40, 24);
		{
			GLuint fbo;

			glGenFramebuffers(1, &fbo);
			glBindFramebuffer(GL_FRAMEBUFFER, fbo);
			glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, dst, 0);
			check("copy", 256, 256, expect_rects, r, 0);
		}
	}
	printf("%d of %d cases right\n", run - failed, run);
	return failed ? 1 : 0;
}
