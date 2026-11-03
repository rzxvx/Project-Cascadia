/* glupload -- textures written while draws read them (docs/research/
 * p105-mesa.md, M37): a texture updated box by box with glTexSubImage2D,
 * each version drawn into a cell of its own before the next box -- all in
 * one render, so each draw must read the copy it was given, not the
 * latest.  RGBA 128 x 64 (two Morton squares side by side), ALPHA (glamor's
 * glyphs), and 100 x 60 (padded to 128 x 64: a box at the edges).  Each
 * format the sampler reads as it is.  Then the time a 1024 x 1024 upload
 * takes, and 64 boxes of 16 x 16 drawn one by one.
 *
 *   sgx-gl glupload
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
#include <time.h>

#define STEPS 8

static int cases, failed;

static double
now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

static GLuint
program(const char *fs_body)
{
	static const char *vs = "attribute vec2 p; varying vec2 v;\n"
	                        "void main() { gl_Position = vec4(p, 0.0, 1.0); v = p * 0.5 + 0.5; }\n";
	char fs[512];
	const char *src[2] = { vs, fs };
	GLuint p = glCreateProgram();

	snprintf(fs, sizeof(fs), "precision mediump float; uniform sampler2D t; uniform vec4 cell;\n"
	         "varying vec2 v;\nvoid main() { vec2 c = (v - cell.xy) / cell.zw; %s }\n", fs_body);
	for (int i = 0; i < 2; i++) {
		GLuint s = glCreateShader(i ? GL_FRAGMENT_SHADER : GL_VERTEX_SHADER);

		glShaderSource(s, 1, &src[i], NULL);
		glCompileShader(s);
		glAttachShader(p, s);
	}
	glBindAttribLocation(p, 0, "p");
	glLinkProgram(p);
	return p;
}

/* texel (x, y) of box k's colour: the box's step in red, its own x, y */
static void
box_texel(int k, int x, int y, uint8_t c[4])
{
	c[0] = 30 * (k + 1), c[1] = (x * 7) & 255, c[2] = (y * 13) & 255, c[3] = 255 - 20 * k;
}

/* box k of a w x h texture: corners (x0, y0), size (bw, bh) -- the last
 * ones at the right and top edges */
static void
box_of(int k, int w, int h, int *x0, int *y0, int *bw, int *bh)
{
	*bw = w / 4 + 3;
	*bh = h / 3;
	*x0 = (k * w / 5) % (w - *bw + 1);
	*y0 = k == STEPS - 1 ? h - *bh : (k * h / 7) % (h - *bh + 1);
	if (k == STEPS - 2)
		*x0 = w - *bw;
}

/* One texture of w x h in format fmt, STEPS versions drawn in a row of
 * cells, each checked against its own version */
static void
versions(const char *name, int w, int h, GLenum fmt)
{
	static const float quad[] = { -1, -1, 1, -1, -1, 1, 1, 1 };
	int tw = STEPS * w;
	uint8_t *tex = calloc(w * h, 4), *want = calloc((size_t)tw * h, 4), *px = calloc((size_t)tw * h, 4);
	uint8_t *box = malloc(w * h * 4);
	GLuint rt, fbo, t, p;
	int bad = 0, at = -1;

	glGenTextures(1, &rt);
	glBindTexture(GL_TEXTURE_2D, rt);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, tw, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	glGenFramebuffers(1, &fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, rt, 0);
	glViewport(0, 0, tw, h);
	glClearColor(0, 0, 0, 0);
	glClear(GL_COLOR_BUFFER_BIT);

	glGenTextures(1, &t);
	glBindTexture(GL_TEXTURE_2D, t);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	/* version 0: every texel (0, x, y, 255) */
	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++) {
			uint8_t *c = tex + 4 * (y * w + x);

			c[0] = 0, c[1] = x * 3, c[2] = y * 5, c[3] = fmt == GL_ALPHA ? y * 5 : 255;
		}
	if (fmt == GL_ALPHA) {
		for (int i = 0; i < w * h; i++)
			box[i] = tex[4 * i + 3];
		glTexImage2D(GL_TEXTURE_2D, 0, GL_ALPHA, w, h, 0, GL_ALPHA, GL_UNSIGNED_BYTE, box);
	} else {
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, tex);
	}
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

	p = program(fmt == GL_ALPHA ?
	            "if (c.x < 0.0 || c.x > 1.0) discard; gl_FragColor = vec4(texture2D(t, c).a);" :
	            "if (c.x < 0.0 || c.x > 1.0) discard; gl_FragColor = texture2D(t, c);");
	glUseProgram(p);
	glUniform1i(glGetUniformLocation(p, "t"), 0);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, quad);
	for (int k = 0; k < STEPS; k++) {
		int x0, y0, bw, bh;

		box_of(k, w, h, &x0, &y0, &bw, &bh);
		for (int y = 0; y < bh; y++)
			for (int x = 0; x < bw; x++) {
				uint8_t *c = tex + 4 * ((y0 + y) * w + x0 + x);

				box_texel(k, x0 + x, y0 + y, c);
				if (fmt == GL_ALPHA)
					box[y * bw + x] = c[3];
				else
					memcpy(box + 4 * (y * bw + x), c, 4);
			}
		glTexSubImage2D(GL_TEXTURE_2D, 0, x0, y0, bw, bh, fmt, GL_UNSIGNED_BYTE, box);
		glUniform4f(glGetUniformLocation(p, "cell"), (float)k / STEPS, 0, 1.0f / STEPS, 1);
		glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
		/* what cell k must hold */
		for (int y = 0; y < h; y++)
			for (int x = 0; x < w; x++) {
				uint8_t *d = want + 4 * (y * tw + k * w + x), *c = tex + 4 * (y * w + x);

				if (fmt == GL_ALPHA)
					d[0] = d[1] = d[2] = d[3] = c[3];
				else
					memcpy(d, c, 4);
			}
	}
	glReadPixels(0, 0, tw, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
	for (int i = 0; i < tw * h; i++) {
		int off = 0;

		for (int c = 0; c < 4; c++)
			off |= abs(px[4 * i + c] - want[4 * i + c]) > 1;
		if (off && at < 0)
			at = i;
		bad += off;
	}
	cases++;
	if (bad) {
		failed++;
		printf("%-28s WRONG: %d pixels, the first at %d, %d (cell %d): %d %d %d %d, want %d %d %d %d\n",
		       name, bad, at % tw, at / tw, at % tw / w, px[4 * at], px[4 * at + 1],
		       px[4 * at + 2], px[4 * at + 3], want[4 * at], want[4 * at + 1], want[4 * at + 2],
		       want[4 * at + 3]);
	} else {
		printf("%-28s ok\n", name);
	}
	glDisableVertexAttribArray(0);
	glUseProgram(0);
	glDeleteProgram(p);
	glDeleteTextures(1, &t);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glDeleteFramebuffers(1, &fbo);
	glDeleteTextures(1, &rt);
	free(tex);
	free(want);
	free(px);
	free(box);
}

/* The formats the sampler reads as they are (M37): a 4 x 4 texture,
 * texel i's channels the bits of i (r 1, g 2, b 4, a 8) as the format has
 * them, each drawn as a 16 x 16 block */
static void
put(uint8_t *dst, GLenum format, GLenum type, int i)
{
	int r = i & 1, g = i >> 1 & 1, b = i >> 2 & 1, a = i >> 3 & 1;

	if (type == GL_UNSIGNED_SHORT_5_6_5)
		*(uint16_t *)dst = r * 0xf800 | g * 0x07e0 | b * 0x001f;
	else if (type == GL_UNSIGNED_SHORT_4_4_4_4)
		*(uint16_t *)dst = r * 0xf000 | g * 0x0f00 | b * 0x00f0 | a * 0x000f;
	else if (type == GL_UNSIGNED_SHORT_5_5_5_1)
		*(uint16_t *)dst = r * 0xf800 | g * 0x07c0 | b * 0x003e | a;
	else if (format == GL_LUMINANCE)
		dst[0] = r * 255;
	else if (format == GL_ALPHA)
		dst[0] = a * 255;
	else if (format == GL_LUMINANCE_ALPHA)
		dst[0] = r * 255, dst[1] = a * 255;
	else if (format == GL_RGB)
		dst[0] = r * 255, dst[1] = g * 255, dst[2] = b * 255;
	else if (format == 0x80E1)      /* GL_BGRA_EXT */
		dst[0] = b * 255, dst[1] = g * 255, dst[2] = r * 255, dst[3] = a * 255;
	else
		dst[0] = r * 255, dst[1] = g * 255, dst[2] = b * 255, dst[3] = a * 255;
}

static void
want_texel(GLenum format, GLenum type, int i, uint8_t c[4])
{
	int r = i & 1, g = i >> 1 & 1, b = i >> 2 & 1, a = i >> 3 & 1;

	c[0] = r * 255, c[1] = g * 255, c[2] = b * 255, c[3] = a * 255;
	if (type == GL_UNSIGNED_SHORT_5_6_5 || format == GL_RGB)
		c[3] = 255;
	if (format == GL_LUMINANCE)
		c[1] = c[2] = c[0], c[3] = 255;
	if (format == GL_LUMINANCE_ALPHA)
		c[1] = c[2] = c[0];
	if (format == GL_ALPHA)
		c[0] = c[1] = c[2] = 0;
}

static void
format_case(const char *name, GLenum format, GLenum type, int bytes)
{
	static const float quad[] = { -1, -1, 1, -1, -1, 1, 1, 1 };
	uint8_t texels[16 * 4], px[64 * 64 * 4];
	GLuint rt, fbo, t, p;
	int bad = 0, at = -1;

	for (int i = 0; i < 16; i++)
		put(texels + i * bytes, format, type, i);
	glGenTextures(1, &rt);
	glBindTexture(GL_TEXTURE_2D, rt);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 64, 64, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	glGenFramebuffers(1, &fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, rt, 0);
	glViewport(0, 0, 64, 64);
	glGenTextures(1, &t);
	glBindTexture(GL_TEXTURE_2D, t);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	glTexImage2D(GL_TEXTURE_2D, 0, format == 0x80E1 ? 0x80E1 : format, 4, 4, 0, format, type,
	             texels);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	p = program("gl_FragColor = texture2D(t, v);");
	glUseProgram(p);
	glUniform1i(glGetUniformLocation(p, "t"), 0);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, quad);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	glReadPixels(0, 0, 64, 64, GL_RGBA, GL_UNSIGNED_BYTE, px);
	for (int i = 0; i < 64 * 64; i++) {
		uint8_t c[4];
		int off = 0;

		want_texel(format, type, (i % 64) / 16 + 4 * ((i / 64) / 16), c);
		for (int k = 0; k < 4; k++)
			off |= abs(px[4 * i + k] - c[k]) > 2;
		if (off && at < 0)
			at = i;
		bad += off;
	}
	cases++;
	if (bad) {
		uint8_t c[4];

		want_texel(format, type, (at % 64) / 16 + 4 * ((at / 64) / 16), c);
		failed++;
		printf("%-28s WRONG: %d pixels, the first at %d, %d: %d %d %d %d, want %d %d %d %d\n",
		       name, bad, at % 64, at / 64, px[4 * at], px[4 * at + 1], px[4 * at + 2],
		       px[4 * at + 3], c[0], c[1], c[2], c[3]);
	} else {
		printf("%-28s ok\n", name);
	}
	glDisableVertexAttribArray(0);
	glDeleteProgram(p);
	glDeleteTextures(1, &t);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glDeleteFramebuffers(1, &fbo);
	glDeleteTextures(1, &rt);
}

/* the times: a whole 1024 x 1024 texture uploaded and drawn, and 64 boxes
 * of 16 x 16 each drawn after its upload (glamor's glyphs) */
static void
timing(void)
{
	static const float quad[] = { -1, -1, 1, -1, -1, 1, 1, 1 };
	enum { N = 1024 };
	uint8_t *img = malloc(N * N * 4), px[4];
	GLuint rt, fbo, t, p;
	double t0, full, boxes;

	for (int i = 0; i < N * N * 4; i++)
		img[i] = i * 31;
	glGenTextures(1, &rt);
	glBindTexture(GL_TEXTURE_2D, rt);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 256, 256, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	glGenFramebuffers(1, &fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, rt, 0);
	glViewport(0, 0, 256, 256);
	glGenTextures(1, &t);
	glBindTexture(GL_TEXTURE_2D, t);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, N, N, 0, GL_RGBA, GL_UNSIGNED_BYTE, img);
	p = program("gl_FragColor = texture2D(t, v);");
	glUseProgram(p);
	glUniform1i(glGetUniformLocation(p, "t"), 0);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, quad);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
	t0 = now_ms();
	for (int i = 0; i < 5; i++) {
		glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, N, N, GL_RGBA, GL_UNSIGNED_BYTE, img);
		glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	}
	glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
	full = (now_ms() - t0) / 5;
	t0 = now_ms();
	for (int i = 0; i < 64; i++) {
		glTexSubImage2D(GL_TEXTURE_2D, 0, (i % 8) * 128, (i / 8) * 128, 16, 16, GL_RGBA,
		                GL_UNSIGNED_BYTE, img);
		glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	}
	glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
	boxes = now_ms() - t0;
	printf("a 1024 x 1024 upload and draw: %.1f ms; 64 boxes of 16 x 16, a draw each: %.1f ms\n",
	       full, boxes);
	glDisableVertexAttribArray(0);
	glDeleteProgram(p);
	glDeleteTextures(1, &t);
	glDeleteFramebuffers(1, &fbo);
	glDeleteTextures(1, &rt);
	free(img);
}

int
main(void)
{
	PFNEGLGETPLATFORMDISPLAYEXTPROC get_display;
	EGLint ctx_attrs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
	EGLDisplay dpy;
	EGLContext ctx;

	get_display = (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
	dpy = get_display ? get_display(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL) : NULL;
	if (!dpy || !eglInitialize(dpy, NULL, NULL) || !eglBindAPI(EGL_OPENGL_ES_API))
		return fprintf(stderr, "glupload: no EGL\n"), 2;
	ctx = eglCreateContext(dpy, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, ctx_attrs);
	if (ctx == EGL_NO_CONTEXT || !eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx))
		return fprintf(stderr, "glupload: no GLES 2 context\n"), 2;
	versions("RGBA 128 x 64, 8 versions", 128, 64, GL_RGBA);
	versions("ALPHA 128 x 64, 8 versions", 128, 64, GL_ALPHA);
	versions("RGBA 100 x 60, 8 versions", 100, 60, GL_RGBA);
	versions("RGBA 64 x 128, 8 versions", 64, 128, GL_RGBA);
	format_case("format RGBA8", GL_RGBA, GL_UNSIGNED_BYTE, 4);
	format_case("format RGB8", GL_RGB, GL_UNSIGNED_BYTE, 3);
	format_case("format RGB565", GL_RGB, GL_UNSIGNED_SHORT_5_6_5, 2);
	format_case("format RGBA4444", GL_RGBA, GL_UNSIGNED_SHORT_4_4_4_4, 2);
	format_case("format RGBA5551", GL_RGBA, GL_UNSIGNED_SHORT_5_5_5_1, 2);
	format_case("format LUMINANCE", GL_LUMINANCE, GL_UNSIGNED_BYTE, 1);
	format_case("format ALPHA", GL_ALPHA, GL_UNSIGNED_BYTE, 1);
	format_case("format LUMINANCE_ALPHA", GL_LUMINANCE_ALPHA, GL_UNSIGNED_BYTE, 2);
	if (strstr((const char *)glGetString(GL_EXTENSIONS), "GL_EXT_texture_format_BGRA8888"))
		format_case("format BGRA8", 0x80E1, GL_UNSIGNED_BYTE, 4);
	timing();
	printf("%d of %d cases right\n", cases - failed, cases);
	eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	eglTerminate(dpy);
	return failed ? 1 : 0;
}
