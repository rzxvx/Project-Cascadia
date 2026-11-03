/* glflow -- texture lookups and derivatives in programs that branch
 * (docs/research/p105-mesa.md, M33): a lookup in an if on a uniform, in a
 * loop of a uniform count, before and after one, in nested loops, in an
 * if/else on a varying; a mipmapped texture's level in a loop; dFdx and
 * dFdy, in a loop too.  A 4 x 4 texture, texel i the bytes i, 0x40 + i,
 * 0x80 + i, 0xc0 + i, over the whole target; every pixel read back against
 * what C works out (a render that hangs comes back wrong).
 *
 *   sgx-gl glflow
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

static uint8_t px[W * H * 4];
static int cases, failed;

static GLuint
program(const char *fs_body)
{
	static const char *vs = "attribute vec4 a_position; varying vec2 v;\n"
	                        "void main() { gl_Position = a_position; v = a_position.xy * 0.5 + 0.5; }\n";
	char fs[4096];
	const char *src[2] = { vs, fs };
	GLuint p = glCreateProgram();
	char log[512];
	GLint ok;

	snprintf(fs, sizeof(fs), "#extension GL_OES_standard_derivatives : enable\n"
	         "precision highp float; varying vec2 v; uniform sampler2D t; uniform int n;\n"
	         "void main() { %s }\n", fs_body);
	for (int i = 0; i < 2; i++) {
		GLuint s = glCreateShader(i ? GL_FRAGMENT_SHADER : GL_VERTEX_SHADER);

		glShaderSource(s, 1, &src[i], NULL);
		glCompileShader(s);
		glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
		if (!ok) {
			glGetShaderInfoLog(s, sizeof(log), NULL, log);
			fprintf(stderr, "glflow: %s\n", log);
		}
		glAttachShader(p, s);
	}
	glBindAttribLocation(p, 0, "a_position");
	glLinkProgram(p);
	glUseProgram(p);
	glUniform1i(glGetUniformLocation(p, "t"), 0);
	glUniform1i(glGetUniformLocation(p, "n"), 2);
	return p;
}

/* the texel at (x, y) of a target over the texture: (u, v) 0..1 */
static void
texel(float u, float v, float c[4])
{
	int i = (int)(u * 4) + 4 * (int)(v * 4);

	c[0] = i / 255.0f;
	c[1] = (0x40 + i) / 255.0f;
	c[2] = (0x80 + i) / 255.0f;
	c[3] = (0xc0 + i) / 255.0f;
}

static int
to8(float v)
{
	return (int)(fminf(fmaxf(v, 0), 1) * 255 + 0.5f);
}

static void
check(const char *name, const char *body, void (*want)(float u, float v, float rgba[4]))
{
	static const float quad[] = { -1, -1, 1, -1, -1, 1, 1, 1 };
	int bad = 0, at = -1;
	float rgba[4];

	program(body);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, quad);
	glClearColor(0, 0, 0, 0);
	glClear(GL_COLOR_BUFFER_BIT);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px);
	for (int i = 0; i < W * H; i++) {
		int off = 0;

		want((i % W + 0.5f) / W, (i / W + 0.5f) / H, rgba);
		for (int c = 0; c < 4; c++)
			off |= abs(px[4 * i + c] - to8(rgba[c])) > 2;
		if (off && at < 0)
			at = i;
		bad += off;
	}
	if (getenv("GLFLOW_PPM") && !strcmp(name, getenv("GLFLOW_PPM"))) {
		FILE *f = fopen("/tmp/glflow.ppm", "wb");

		fprintf(f, "P6 %d %d 255\n", W, H);
		for (int y = H - 1; y >= 0; y--)
			for (int x = 0; x < W; x++)
				fwrite(px + (y * W + x) * 4, 1, 3, f);
		fclose(f);
	}
	cases++;
	if (!bad) {
		printf("%-40s ok\n", name);
		return;
	}
	failed++;
	want((at % W + 0.5f) / W, (at / W + 0.5f) / H, rgba);
	printf("%-40s WRONG: %d pixels, the first at %d, %d: %d %d %d %d, want %d %d %d %d\n", name,
	       bad, at % W, at / W, px[4 * at], px[4 * at + 1], px[4 * at + 2], px[4 * at + 3],
	       to8(rgba[0]), to8(rgba[1]), to8(rgba[2]), to8(rgba[3]));
}

static void
want_texel(float u, float v, float c[4])
{
	texel(u, v, c);
}

static void
want_twice(float u, float v, float c[4])
{
	texel(u, v, c);
	for (int i = 0; i < 4; i++)
		c[i] *= 2;
}

static void
want_four(float u, float v, float c[4])
{
	texel(u, v, c);
	for (int i = 0; i < 4; i++)
		c[i] *= 4;
}

static void
want_split(float u, float v, float c[4])
{
	if (v > 0.5f)
		texel(u, v, c);
	else
		texel(v, u, c);
}

static void
want_derivatives(float u, float v, float c[4])
{
	(void)u, (void)v;
	c[0] = c[1] = 0.5f;
	c[2] = 0;
	c[3] = 1;
}

/* the mipmapped texture's level 1 (green) */
static void
want_level1(float u, float v, float c[4])
{
	(void)u, (void)v;
	c[0] = 0, c[1] = 1, c[2] = 0, c[3] = 1;
}

int
main(void)
{
	PFNEGLGETPLATFORMDISPLAYEXTPROC get_display;
	EGLint ctx_attrs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
	uint8_t texels[16][4];
	GLuint rt, fbo, tex, mip;
	EGLDisplay dpy;
	EGLContext ctx;

	get_display = (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
	dpy = get_display ? get_display(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL) : NULL;
	if (!dpy || !eglInitialize(dpy, NULL, NULL) || !eglBindAPI(EGL_OPENGL_ES_API))
		return fprintf(stderr, "glflow: no EGL\n"), 2;
	ctx = eglCreateContext(dpy, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, ctx_attrs);
	if (ctx == EGL_NO_CONTEXT || !eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx))
		return fprintf(stderr, "glflow: no GLES 2 context\n"), 2;
	glGenTextures(1, &rt);
	glBindTexture(GL_TEXTURE_2D, rt);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	glGenFramebuffers(1, &fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, rt, 0);
	glViewport(0, 0, W, H);
	glEnableVertexAttribArray(0);

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

	check("in an if on a uniform", "vec4 c = vec4(0.1); if (n > 1) c = texture2D(t, v); "
	      "gl_FragColor = c;", want_texel);
	check("in a loop of a uniform count", "vec4 c = vec4(0.0); for (int i = 0; i < n; i++) "
	      "c += texture2D(t, v); gl_FragColor = c;", want_twice);
	check("in a loop, at the loop's coordinates", "vec4 c = vec4(0.0); for (int i = 0; i < n; i++) "
	      "c += texture2D(t, v + vec2(float(i) * 0.001)); gl_FragColor = c;", want_twice);
	check("before a loop", "vec4 c = texture2D(t, v); for (int i = 0; i < n; i++) c *= 2.0; "
	      "gl_FragColor = c * 0.25;", want_texel);
	check("after a loop", "float k = 0.0; for (int i = 0; i < n; i++) k += 0.5; "
	      "gl_FragColor = texture2D(t, v) * k;", want_texel);
	check("in nested loops", "vec4 c = vec4(0.0); for (int i = 0; i < n; i++) "
	      "for (int j = 0; j < n; j++) c += texture2D(t, v); gl_FragColor = c;", want_four);
	check("in an if/else on a varying", "vec4 c; if (v.y > 0.5) c = texture2D(t, v); "
	      "else c = texture2D(t, v.yx); gl_FragColor = c;", want_split);
	check("dFdx, dFdy", "gl_FragColor = vec4(dFdx(v.x) * 32.0, dFdy(v.y) * 32.0, "
	      "abs(dFdy(v.x)) + abs(dFdx(v.y)), 1.0);", want_derivatives);
	check("dFdx, dFdy in a loop", "float a = 0.0, b = 0.0; for (int i = 0; i < n; i++) "
	      "{ a += dFdx(v.x * float(i + 1)); b += dFdy(v.y * float(i + 1)); } "
	      "gl_FragColor = vec4(a * 32.0 / 3.0, b * 32.0 / 3.0, 0.0, 1.0);", want_derivatives);

	/* a 128 x 128 texture, a colour a level, drawn at 64 x 64: level 1 --
	 * in a loop */
	{
		static const uint8_t colour[8][4] = {
			{ 255, 0, 0, 255 }, { 0, 255, 0, 255 }, { 0, 0, 255, 255 }, { 255, 255, 0, 255 },
			{ 0, 255, 255, 255 }, { 255, 0, 255, 255 }, { 255, 255, 255, 255 }, { 0, 0, 0, 255 },
		};
		uint8_t *img = malloc(128 * 128 * 4);

		glGenTextures(1, &mip);
		glBindTexture(GL_TEXTURE_2D, mip);
		for (int l = 0; l < 8; l++) {
			int s = 128 >> l;

			for (int i = 0; i < s * s; i++)
				memcpy(img + 4 * i, colour[l], 4);
			glTexImage2D(GL_TEXTURE_2D, l, GL_RGBA, s, s, 0, GL_RGBA, GL_UNSIGNED_BYTE, img);
		}
		free(img);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		check("a mipmap's level, in a loop", "vec4 c = vec4(0.0); for (int i = 0; i < n; i++) "
		      "c += 0.5 * texture2D(t, v); gl_FragColor = c;", want_level1);
		glBindTexture(GL_TEXTURE_2D, tex);
	}

	printf("%d of %d cases right\n", cases - failed, cases);
	eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	eglTerminate(dpy);
	return failed ? 1 : 0;
}
