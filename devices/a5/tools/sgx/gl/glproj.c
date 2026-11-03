/* glproj -- projective lookups and rectangle textures as the PDS's (docs/
 * research/p105-mesa.md, M39): texture2DProj of a vec4 (z not the
 * divisor) and of a vec3, texture2DRect and texture2DRectProj, the fixed
 * function's rectangle texture by glBegin(GL_QUADS) as xfwm4 composites
 * (the CPU's vertex side) and by triangles (the GPU's), its projective
 * 2D texture (glTexCoord4f), and a vec4 whose z the program reads besides
 * (the vertex side must leave it).  A 16 x 16 texture, every texel its
 * own, on a 64 x 64 target: every pixel read back against what C works
 * out.  SGX_NO_PROJ_FETCH=1: the same in the program.
 *
 *   sgx-gl glproj [CASE]        (CASE: a name's start; all by default)
 *
 * Exit status 0 when every case passed.
 */
#include <EGL/egl.h>
#include <EGL/eglext.h>
#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include <GL/glext.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W 64
#define H 64
#define T 16

static uint8_t texel[T][T][4];
static uint8_t px[W * H * 4];
static int cases, failed;
static const char *only;

static void
make_texels(void)
{
	for (int y = 0; y < T; y++)
		for (int x = 0; x < T; x++) {
			texel[y][x][0] = x * 16 + 8;
			texel[y][x][1] = y * 16 + 8;
			texel[y][x][2] = (x ^ y) * 16;
			texel[y][x][3] = 255 - x * 8;
		}
}

static GLuint
program(const char *vs_body, const char *fs_body)
{
	char vs[1024], fs[1024], log[512];
	const char *src[2] = { vs, fs };
	GLuint p = glCreateProgram();
	GLint ok;

	snprintf(vs, sizeof(vs), "#version 120\nattribute vec4 a_position;\n"
	         "varying vec4 v4;\nvarying vec3 v3;\nvarying vec2 v2;\n"
	         "void main() { gl_Position = a_position; vec2 uv = a_position.xy * 0.5 + 0.5;"
	         " %s }\n", vs_body);
	snprintf(fs, sizeof(fs), "#version 120\n#extension GL_ARB_texture_rectangle : enable\n"
	         "varying vec4 v4;\nvarying vec3 v3;\nvarying vec2 v2;\n"
	         "uniform sampler2D t2;\nuniform sampler2DRect tr;\n"
	         "void main() { %s }\n", fs_body);
	for (int i = 0; i < 2; i++) {
		GLuint s = glCreateShader(i ? GL_FRAGMENT_SHADER : GL_VERTEX_SHADER);

		glShaderSource(s, 1, &src[i], NULL);
		glCompileShader(s);
		glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
		if (!ok) {
			glGetShaderInfoLog(s, sizeof(log), NULL, log);
			fprintf(stderr, "glproj: %s\n", log);
		}
		glAttachShader(p, s);
	}
	glBindAttribLocation(p, 0, "a_position");
	glLinkProgram(p);
	glUseProgram(p);
	glUniform1i(glGetUniformLocation(p, "t2"), 0);
	glUniform1i(glGetUniformLocation(p, "tr"), 1);
	return p;
}

/* every pixel against the texel under its centre (4 x 4 pixels a texel);
 * alpha_one: the texture is RGB */
static void
check(const char *name, int alpha_one)
{
	int bad = 0, first = -1;

	glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px);
	for (int y = 0; y < H; y++)
		for (int x = 0; x < W; x++) {
			const uint8_t *t = texel[y / 4][x / 4], *p = px + 4 * (y * W + x);

			if (p[0] != t[0] || p[1] != t[1] || p[2] != t[2] ||
			    p[3] != (alpha_one ? 255 : t[3])) {
				if (first < 0)
					first = y * W + x;
				bad++;
			}
		}
	cases++;
	if (bad) {
		const uint8_t *p = px + 4 * first;
		int x = first % W, y = first / W;
		const uint8_t *t = texel[y / 4][x / 4];

		failed++;
		printf("%-34s WRONG: %d pixels; (%d, %d) %02x%02x%02x%02x, want %02x%02x%02x%02x\n",
		       name, bad, x, y, p[0], p[1], p[2], p[3], t[0], t[1], t[2],
		       alpha_one ? 255 : t[3]);
	} else {
		printf("%-34s ok\n", name);
	}
}

static int
want(const char *name)
{
	return !only || !strncmp(name, only, strlen(only));
}

/* a quad over the target; a_position's z is 0 and its w 1, which the
 * compiler cannot know: values the linker cannot fold */
static void
draw_quad(const char *vs_body, const char *fs_body)
{
	static const float quad[] = { -1, -1, 1, -1, -1, 1, 1, 1 };
	GLuint p;

	glClearColor(0, 0, 0, 0);
	glClear(GL_COLOR_BUFFER_BIT);
	p = program(vs_body, fs_body);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, quad);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	glDisableVertexAttribArray(0);
	glUseProgram(0);
	glDeleteProgram(p);
}

static void
glsl_case(const char *name, const char *vs_body, const char *fs_body)
{
	if (!want(name))
		return;
	draw_quad(vs_body, fs_body);
	check(name, 0);
}

/* against the same lookup in the program (fs_ref: a shape the PDS does
 * not take), at most slack pixels apart -- the iterator's division and
 * the program's can round a texel's edge apart */
static void
glsl_compare(const char *name, const char *vs_body, const char *fs_body, const char *fs_ref,
             int slack)
{
	static uint8_t ref[W * H * 4];
	int bad = 0, first = -1;

	if (!want(name))
		return;
	draw_quad(vs_body, fs_ref);
	glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, ref);
	draw_quad(vs_body, fs_body);
	glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px);
	for (int i = 0; i < W * H; i++)
		if (memcmp(px + 4 * i, ref + 4 * i, 4)) {
			if (first < 0)
				first = i;
			bad++;
		}
	cases++;
	if (bad > slack) {
		failed++;
		printf("%-34s WRONG: %d pixels; (%d, %d) %02x%02x%02x%02x, want %02x%02x%02x%02x\n",
		       name, bad, first % W, first / W, px[4 * first], px[4 * first + 1],
		       px[4 * first + 2], px[4 * first + 3], ref[4 * first], ref[4 * first + 1],
		       ref[4 * first + 2], ref[4 * first + 3]);
	} else {
		printf("%-34s ok (%d pixels apart)\n", name, bad);
	}
}

/* the fixed function: target the texture's (GL_TEXTURE_2D or the
 * rectangle), s t the coordinates at the top right, q each vertex's q;
 * quads or two triangles */
static void
ff_case(const char *name, GLenum target, GLuint tex, float s, float t, float q, int quads,
        int alpha_one)
{
	static const float corner[6][2] = { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 0 }, { 1, 1 }, { 0, 1 } };
	static const int order[4] = { 0, 1, 2, 5 };

	if (!want(name))
		return;
	glClearColor(0, 0, 0, 0);
	glClear(GL_COLOR_BUFFER_BIT);
	glActiveTexture(GL_TEXTURE0);
	glEnable(target);
	glBindTexture(target, tex);
	glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
	glBegin(quads ? GL_QUADS : GL_TRIANGLES);
	for (int i = 0; i < (quads ? 4 : 6); i++) {
		const float *c = corner[quads ? order[i] : i];

		glTexCoord4f(c[0] * s * q, c[1] * t * q, 0.25f, q);
		glVertex2f(c[0] * 2 - 1, c[1] * 2 - 1);
	}
	glEnd();
	glDisable(target);
	check(name, alpha_one);
}

int
main(int argc, char **argv)
{
	PFNEGLGETPLATFORMDISPLAYEXTPROC get_display;
	EGLint ctx_attrs[] = { EGL_CONTEXT_MAJOR_VERSION, 2, EGL_CONTEXT_MINOR_VERSION, 1, EGL_NONE };
	GLuint rt, fbo, t2, tr, trgb;
	uint8_t rgb[T][T][3];
	EGLDisplay dpy;
	EGLContext ctx;

	only = argc > 1 ? argv[1] : NULL;
	get_display = (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
	dpy = get_display ? get_display(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL) : NULL;
	if (!dpy || !eglInitialize(dpy, NULL, NULL) || !eglBindAPI(EGL_OPENGL_API))
		return fprintf(stderr, "glproj: no EGL\n"), 2;
	ctx = eglCreateContext(dpy, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, ctx_attrs);
	if (ctx == EGL_NO_CONTEXT || !eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx))
		return fprintf(stderr, "glproj: no OpenGL 2.1 context\n"), 2;
	glGenTextures(1, &rt);
	glBindTexture(GL_TEXTURE_2D, rt);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	glGenFramebuffers(1, &fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, rt, 0);
	if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
		return fprintf(stderr, "glproj: the target is not complete\n"), 2;
	glViewport(0, 0, W, H);

	make_texels();
	for (int y = 0; y < T; y++)
		for (int x = 0; x < T; x++)
			memcpy(rgb[y][x], texel[y][x], 3);
	glActiveTexture(GL_TEXTURE0);
	glGenTextures(1, &t2);
	glBindTexture(GL_TEXTURE_2D, t2);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, T, T, 0, GL_RGBA, GL_UNSIGNED_BYTE, texel);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glActiveTexture(GL_TEXTURE1);
	glGenTextures(1, &tr);
	glBindTexture(GL_TEXTURE_RECTANGLE_ARB, tr);
	glTexImage2D(GL_TEXTURE_RECTANGLE_ARB, 0, GL_RGBA8, T, T, 0, GL_RGBA, GL_UNSIGNED_BYTE, texel);
	glTexParameteri(GL_TEXTURE_RECTANGLE_ARB, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_RECTANGLE_ARB, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glActiveTexture(GL_TEXTURE0);
	/* (an RGB rectangle texture: a 24-bit window's, xfwm4's) */
	glGenTextures(1, &trgb);
	glBindTexture(GL_TEXTURE_RECTANGLE_ARB, trgb);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	glTexImage2D(GL_TEXTURE_RECTANGLE_ARB, 0, GL_RGB8, T, T, 0, GL_RGB, GL_UNSIGNED_BYTE, rgb);
	glTexParameteri(GL_TEXTURE_RECTANGLE_ARB, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_RECTANGLE_ARB, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

	glsl_case("texture2D (the reference)", "v2 = uv;", "gl_FragColor = texture2D(t2, v2);");
	glsl_case("texture2DProj vec4, z not w",
	          "float w = 2.0 + a_position.z; v4 = vec4(uv * w, 7.0 + a_position.z, w);",
	          "gl_FragColor = texture2DProj(t2, v4);");
	glsl_case("texture2DProj vec3",
	          "float z = 4.0 * a_position.w; v3 = vec3(uv * z, z);",
	          "gl_FragColor = texture2DProj(t2, v3);");
	glsl_case("texture2DProj vec4, z read besides",
	          "float w = 2.0 + a_position.z; v4 = vec4(uv * w, 7.0 + a_position.z, w);",
	          "gl_FragColor = texture2DProj(t2, v4) + vec4(v4.z - 7.0);");
	glsl_compare("texture2DProj, w across the quad",
	             "float w = 1.0 + 3.0 * uv.x; v4 = vec4(uv * w, a_position.z, w);",
	             "gl_FragColor = texture2DProj(t2, v4);",
	             "gl_FragColor = texture2D(t2, v4.xy / max(v4.w, 0.001));", 96);
	glsl_case("texture2DRect", "v2 = uv * 16.0;", "gl_FragColor = texture2DRect(tr, v2);");
	glsl_case("texture2DRectProj vec4",
	          "float w = 2.0 + a_position.z; v4 = vec4(uv * 16.0 * w, 5.0 + a_position.z, w);",
	          "gl_FragColor = texture2DRectProj(tr, v4);");
	glsl_case("texture2DRect, two lookups", "v2 = uv * 16.0;",
	          "gl_FragColor = texture2DRect(tr, v2) * 0.5 + texture2DRect(tr, v2) * 0.5;");
	ff_case("fixed function rect, quads", GL_TEXTURE_RECTANGLE_ARB, trgb, T, T, 1, 1, 1);
	ff_case("fixed function rect, triangles", GL_TEXTURE_RECTANGLE_ARB, trgb, T, T, 1, 0, 1);
	ff_case("fixed function rect q 2, quads", GL_TEXTURE_RECTANGLE_ARB, tr, T, T, 2, 1, 0);
	ff_case("fixed function 2D q 2, triangles", GL_TEXTURE_2D, t2, 1, 1, 2, 0, 0);
	ff_case("fixed function 2D q 2, quads", GL_TEXTURE_2D, t2, 1, 1, 2, 1, 0);
	printf("%d of %d cases right\n", cases - failed, cases);
	eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	eglTerminate(dpy);
	return failed ? 1 : 0;
}
