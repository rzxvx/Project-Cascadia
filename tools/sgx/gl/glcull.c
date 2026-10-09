/* glcull -- culling and the viewport (docs/research/p105-mesa.md, M18): a
 * triangle anticlockwise on the left (red), one clockwise on the right
 * (green), drawn with culling off, back faces culled, front faces culled,
 * and front faces clockwise; the same with the windings the other way
 * round (the side must not matter); gl_FrontFacing, red the front and
 * green the back, both ways round; then a quad over the whole of clip
 * space in a viewport that does not start at 0, whose edges are checked,
 * and in two depth ranges (behind the depth cleared, then in front).  All
 * of it into a texture, then into a pbuffer -- the window system's kind of
 * buffer, which gallium draws upside down (its viewport's y scale
 * negative) -- but the depth ranges, which want a depth buffer.
 *
 *   sgx-gl glcull [--ppm DIR]
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

static uint8_t px[W * H * 4];

static int is(int x, int y, int r, int g, int b)
{
	const uint8_t *q = px + (y * W + x) * 4;

	return abs(q[0] - r) < 40 && abs(q[1] - g) < 40 && abs(q[2] - b) < 40;
}

static void ppm(const char *dir, const char *where, const char *name)
{
	char path[512];
	FILE *f;

	if (!dir)
		return;
	snprintf(path, sizeof(path), "%s/glcull_%s_%s.ppm", dir, where, name);
	if (!(f = fopen(path, "wb")))
		return;
	fprintf(f, "P6\n%d %d\n255\n", W, H);
	for (int y = H - 1; y >= 0; y--)
		for (int x = 0; x < W; x++)
			fwrite(px + (y * W + x) * 4, 1, 3, f);
	fclose(f);
}

/* the cases into what is bound, with program p (pf: gl_FrontFacing's);
 * the ones wrong */
static int cases(const char *where, const char *dir, int depth, GLuint p, GLuint pf, int *n)
{
	/* left: anticlockwise (on the screen, GL's y up); right: clockwise */
	static const float pos[] = {
		-0.9f, -0.5f, 0, 1,  -0.1f, -0.5f, 0, 1,  -0.5f, 0.5f, 0, 1,
		 0.1f, -0.5f, 0, 1,   0.5f, 0.5f, 0, 1,    0.9f, -0.5f, 0, 1,
	};
	/* the same, each wound the other way */
	static const float rpos[] = {
		-0.9f, -0.5f, 0, 1,  -0.5f, 0.5f, 0, 1,  -0.1f, -0.5f, 0, 1,
		 0.1f, -0.5f, 0, 1,   0.9f, -0.5f, 0, 1,   0.5f, 0.5f, 0, 1,
	};
	static const float col[] = {
		1, 0, 0, 1,  1, 0, 0, 1,  1, 0, 0, 1,
		0, 1, 0, 1,  0, 1, 0, 1,  0, 1, 0, 1,
	};
	static const float quad[] = { -1, -1, 0, 1,  1, -1, 0, 1,  1, 1, 0, 1,
	                              -1, -1, 0, 1,  1, 1, 0, 1,  -1, 1, 0, 1 };
	static const float blue[] = { 0, 0, 1, 1,  0, 0, 1, 1,  0, 0, 1, 1,
	                              0, 0, 1, 1,  0, 0, 1, 1,  0, 0, 1, 1 };
	static const struct {
		const char *name;
		int cull;          /* 0 off, else glCullFace's */
		GLenum front;
		int swap;          /* the left one clockwise, the right one not */
		int left, right;   /* visible? */
	} c[] = {
		{ "off", 0, GL_CCW, 0, 1, 1 },
		{ "back", GL_BACK, GL_CCW, 0, 1, 0 },
		{ "front", GL_FRONT, GL_CCW, 0, 0, 1 },
		{ "back_cw", GL_BACK, GL_CW, 0, 0, 1 },
		{ "swap_off", 0, GL_CCW, 1, 1, 1 },
		{ "swap_back", GL_BACK, GL_CCW, 1, 0, 1 },
		{ "swap_front", GL_FRONT, GL_CCW, 1, 1, 0 },
	};
	int failed = 0;

	for (unsigned k = 0; k < sizeof(c) / sizeof(c[0]); k++, (*n)++) {
		int l, r;

		glViewport(0, 0, W, H);
		glClearColor(0, 0, 0, 1);
		glClear(GL_COLOR_BUFFER_BIT);
		if (c[k].cull) {
			glEnable(GL_CULL_FACE);
			glCullFace(c[k].cull);
		} else {
			glDisable(GL_CULL_FACE);
		}
		glFrontFace(c[k].front);
		glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 0, c[k].swap ? rpos : pos);
		glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, 0, col);
		glDrawArrays(GL_TRIANGLES, 0, 6);
		glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px);
		l = is(W / 4, H / 2 - 100, 255, 0, 0);
		r = is(3 * W / 4, H / 2 - 100, 0, 255, 0);
		failed += l != c[k].left || r != c[k].right;
		printf("%-7s %-10s %s: left (%s) %s, right (%s) %s\n", where, c[k].name,
		       l == c[k].left && r == c[k].right ? "ok   " : "WRONG",
		       c[k].swap ? "clockwise" : "anticlockwise", l ? "drawn" : "culled",
		       c[k].swap ? "anticlockwise" : "clockwise", r ? "drawn" : "culled");
		ppm(dir, where, c[k].name);
	}

	/* gl_FrontFacing, culling off: red the front, green the back */
	for (int cw = 0; cw < 2; cw++, (*n)++) {
		int lr, lg, rr, rg, ok;

		glUseProgram(pf);
		glDisable(GL_CULL_FACE);
		glFrontFace(cw ? GL_CW : GL_CCW);
		glClear(GL_COLOR_BUFFER_BIT);
		glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 0, pos);
		glDrawArrays(GL_TRIANGLES, 0, 6);
		glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px);
		lr = is(W / 4, H / 2 - 100, 255, 0, 0);
		lg = is(W / 4, H / 2 - 100, 0, 255, 0);
		rr = is(3 * W / 4, H / 2 - 100, 255, 0, 0);
		rg = is(3 * W / 4, H / 2 - 100, 0, 255, 0);
		ok = cw ? lg && rr : lr && rg;
		failed += !ok;
		printf("%-7s %-10s %s: left (anticlockwise) %s, right (clockwise) %s\n", where,
		       cw ? "facing_cw" : "facing", ok ? "ok   " : "WRONG",
		       lr ? "front" : lg ? "back" : "neither", rr ? "front" : rg ? "back" : "neither");
		ppm(dir, where, cw ? "facing_cw" : "facing");
		glFrontFace(GL_CCW);
		glUseProgram(p);
	}

	/* the viewport: x 192..576, y 128..640 (GL's, from the bottom) */
	{
		static const int xs[] = { 191, 193, 575, 577 }, ys[] = { 127, 129, 639, 641 };
		int ok = 1;

		glDisable(GL_CULL_FACE);
		glFrontFace(GL_CCW);
		glViewport(0, 0, W, H);
		glClear(GL_COLOR_BUFFER_BIT);
		glViewport(192, 128, 384, 512);
		glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 0, quad);
		glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, 0, blue);
		glDrawArrays(GL_TRIANGLES, 0, 6);
		glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px);
		for (int i = 0; i < 4; i++) {
			int in = i == 1 || i == 2;

			ok &= is(xs[i], 384, 0, 0, in ? 255 : 0) && is(384, ys[i], 0, 0, in ? 255 : 0);
		}
		failed += !ok;
		(*n)++;
		printf("%-7s viewport   %s: blue at x %d %d %d %d, y %d %d %d %d (want 0 1 1 0 each)\n",
		       where, ok ? "ok   " : "WRONG", is(191, 384, 0, 0, 255), is(193, 384, 0, 0, 255),
		       is(575, 384, 0, 0, 255), is(577, 384, 0, 0, 255), is(384, 127, 0, 0, 255),
		       is(384, 129, 0, 0, 255), is(384, 639, 0, 0, 255), is(384, 641, 0, 0, 255));
		ppm(dir, where, "viewport");
	}

	/* the depth range: 0.5..1 puts z 0 at 0.75 -- behind a clear to 0.5,
	 * so less fails; the scale and the translate the other way round
	 * would put it at 0.25 */
	if (depth) {
		static const float zq[] = { -1, -1, 0, 1,  1, -1, 0, 1,  1, 1, 0, 1,
		                            -1, -1, 0, 1,  1, 1, 0, 1,  -1, 1, 0, 1 };
		int front, behind;

		glViewport(0, 0, W, H);
		glEnable(GL_DEPTH_TEST);
		glDepthFunc(GL_LESS);
		glClearDepthf(0.5f);
		glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
		glDepthRangef(0.5f, 1);
		glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 0, zq);
		glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, 0, blue);
		glDrawArrays(GL_TRIANGLES, 0, 6);
		glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px);
		behind = !is(W / 2, H / 2, 0, 0, 255);
		/* and 0..0.5: z 0 at 0.25, in front */
		glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
		glDepthRangef(0, 0.5f);
		glDrawArrays(GL_TRIANGLES, 0, 6);
		glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px);
		front = is(W / 2, H / 2, 0, 0, 255);
		glDepthRangef(0, 1);
		glDisable(GL_DEPTH_TEST);
		failed += !behind || !front;
		(*n)++;
		printf("%-7s depthrange %s: 0.5..1 %s, 0..0.5 %s\n", where,
		       behind && front ? "ok   " : "WRONG", behind ? "behind" : "IN FRONT",
		       front ? "in front" : "BEHIND");
	}
	return failed;
}

int main(int argc, char **argv)
{
	static const char *vs = "attribute vec4 p; attribute vec4 c; varying vec4 v;\n"
	                        "void main() { v = c; gl_Position = p; }\n";
	static const char *fs = "precision mediump float; varying vec4 v;\n"
	                        "void main() { gl_FragColor = v; }\n";
	static const char *fs_facing = "precision mediump float; varying vec4 v;\n"
	                               "void main() { gl_FragColor = gl_FrontFacing ?\n"
	                               "  vec4(1.0, 0.0, 0.0, 1.0) : vec4(0.0, 1.0, v.z, 1.0); }\n";
	static const EGLint cfg_attrs[] = {
		EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
		EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE,
	};
	static const EGLint pb_attrs[] = { EGL_WIDTH, W, EGL_HEIGHT, H, EGL_NONE };
	PFNEGLGETPLATFORMDISPLAYEXTPROC get_display;
	EGLint ctx_attrs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE }, ncfg;
	const char *dir = argc > 2 && !strcmp(argv[1], "--ppm") ? argv[2] : NULL;
	int failed = 0, n = 0;
	GLuint rt, zb, fbo, p, pf;
	EGLSurface pb;
	EGLDisplay dpy;
	EGLContext ctx;
	EGLConfig cfg;

	get_display = (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
	dpy = get_display ? get_display(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL) : NULL;
	if (!dpy || !eglInitialize(dpy, NULL, NULL) || !eglBindAPI(EGL_OPENGL_ES_API))
		return fprintf(stderr, "glcull: no EGL\n"), 2;
	if (!eglChooseConfig(dpy, cfg_attrs, &cfg, 1, &ncfg) || ncfg < 1)
		return fprintf(stderr, "glcull: no pbuffer config\n"), 2;
	ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx_attrs);
	pb = eglCreatePbufferSurface(dpy, cfg, pb_attrs);
	if (ctx == EGL_NO_CONTEXT || pb == EGL_NO_SURFACE || !eglMakeCurrent(dpy, pb, pb, ctx))
		return fprintf(stderr, "glcull: no GLES 2 context on a pbuffer\n"), 2;
	glGenTextures(1, &rt);
	glBindTexture(GL_TEXTURE_2D, rt);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	glGenFramebuffers(1, &fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, rt, 0);
	glGenRenderbuffers(1, &zb);
	glBindRenderbuffer(GL_RENDERBUFFER, zb);
	glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, W, H);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, zb);
	p = glCreateProgram();
	for (int i = 0; i < 2; i++) {
		GLuint s = glCreateShader(i ? GL_FRAGMENT_SHADER : GL_VERTEX_SHADER);

		glShaderSource(s, 1, i ? &fs : &vs, NULL);
		glCompileShader(s);
		glAttachShader(p, s);
	}
	glBindAttribLocation(p, 0, "p");
	glBindAttribLocation(p, 1, "c");
	glLinkProgram(p);
	pf = glCreateProgram();
	for (int i = 0; i < 2; i++) {
		GLuint s = glCreateShader(i ? GL_FRAGMENT_SHADER : GL_VERTEX_SHADER);

		glShaderSource(s, 1, i ? &fs_facing : &vs, NULL);
		glCompileShader(s);
		glAttachShader(pf, s);
	}
	glBindAttribLocation(pf, 0, "p");
	glBindAttribLocation(pf, 1, "c");
	glLinkProgram(pf);
	glUseProgram(p);
	glEnableVertexAttribArray(0);
	glEnableVertexAttribArray(1);

	failed += cases("texture", dir, 1, p, pf, &n);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	failed += cases("pbuffer", dir, 0, p, pf, &n);
	printf("%d of %d cases right\n", n - failed, n);
	eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	eglTerminate(dpy);
	return failed ? 1 : 0;
}
