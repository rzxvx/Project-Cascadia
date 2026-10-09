/* glchurn -- programs made, drawn and deleted, over and over (docs/research/
 * p105-mesa.md, M24): their places in GPU memory, code and PDS, are taken
 * back and given to the next ones, many times over.  Each program draws
 * its own colour -- its number, in red and green -- over the whole target,
 * read back at once: a program that ran another's code (a place given
 * out again too early, or the USSE's code cache keeping the old) shows.
 * The fragment shaders are long (padding times a uniform 0), so that the
 * code heap goes round every few hundred; every third program has a vertex
 * shader of its own as well (a blue of its own), the rest share one.
 *
 *   sgx-gl glchurn [N]          (N programs, default 3000)
 */
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W 64
#define H 64

static GLuint shader(GLenum type, const char *src)
{
	GLuint s = glCreateShader(type);
	GLint ok;

	glShaderSource(s, 1, &src, NULL);
	glCompileShader(s);
	glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
	if (!ok)
		fprintf(stderr, "glchurn: a shader did not compile\n");
	return s;
}

int main(int argc, char **argv)
{
	static const char *shared_vs = "attribute vec4 p; varying float b;\n"
	                               "void main() { gl_Position = p; b = 100.0 / 255.0; }\n";
	static const float q[] = { -1, -1, 0, 1, 1, -1, 0, 1, 1, 1, 0, 1,
	                           -1, -1, 0, 1, 1, 1, 0, 1, -1, 1, 0, 1 };
	PFNEGLGETPLATFORMDISPLAYEXTPROC get_display;
	EGLint ctx_attrs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
	int n = argc > 1 ? atoi(argv[1]) : 3000, wrong = 0;
	GLuint rt, fbo, vs_shared;
	EGLDisplay dpy;
	EGLContext ctx;

	get_display = (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
	dpy = get_display ? get_display(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL) : NULL;
	if (!dpy || !eglInitialize(dpy, NULL, NULL) || !eglBindAPI(EGL_OPENGL_ES_API))
		return fprintf(stderr, "glchurn: no EGL\n"), 2;
	ctx = eglCreateContext(dpy, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, ctx_attrs);
	if (ctx == EGL_NO_CONTEXT || !eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx))
		return fprintf(stderr, "glchurn: no GLES 2 context\n"), 2;
	glGenTextures(1, &rt);
	glBindTexture(GL_TEXTURE_2D, rt);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	glGenFramebuffers(1, &fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, rt, 0);
	glViewport(0, 0, W, H);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 0, q);
	vs_shared = shader(GL_VERTEX_SHADER, shared_vs);

	for (int i = 0; i < n; i++) {
		int r = i & 255, g = (i >> 8 & 15) * 16 + 8, own_vs = i % 3 == 0;
		int b = own_vs ? (i * 37 & 127) + 128 : 100;
		char fs[2048], vs[512];
		GLuint p = glCreateProgram(), f, v = vs_shared;
		uint8_t px[4];

		/* the vertex shader's own constant: the blue the pixels get */
		snprintf(vs, sizeof(vs), "attribute vec4 p; varying float b;\n"
		         "void main() { gl_Position = p; b = %d.0 / 255.0; }\n", b);
		snprintf(fs, sizeof(fs), "precision highp float; uniform float zero; varying float b;\n"
		         "void main() { vec4 x = gl_FragCoord * vec4(%d.5, 1.25, 2.5, 0.75);\n"
		         "  for (int k = 0; k < 24; k++) x = x * vec4(1.001, 0.999, 1.002, 0.998) + vec4(0.5);\n"
		         "  gl_FragColor = vec4(%d.0 / 255.0, %d.0 / 255.0, b, 1.0) + zero * x; }\n",
		         i, r, g);
		if (own_vs)
			v = shader(GL_VERTEX_SHADER, vs);
		f = shader(GL_FRAGMENT_SHADER, fs);
		glAttachShader(p, v);
		glAttachShader(p, f);
		glBindAttribLocation(p, 0, "p");
		glLinkProgram(p);
		glUseProgram(p);
		glUniform1f(glGetUniformLocation(p, "zero"), 0.0f);
		glDrawArrays(GL_TRIANGLES, 0, 6);
		glReadPixels(W / 2, H / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
		if (abs(px[0] - r) > 1 || abs(px[1] - g) > 1 || abs(px[2] - b) > 1) {
			if (wrong < 10)
				printf("program %d: %d %d %d, want %d %d %d\n", i, px[0], px[1], px[2],
				       r, g, b);
			wrong++;
		}
		glUseProgram(0);
		glDeleteProgram(p);
		glDeleteShader(f);
		if (v != vs_shared)
			glDeleteShader(v);
	}
	printf("%d of %d programs drew their own colour\n", n - wrong, n);
	glDeleteShader(vs_shared);
	eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	eglTerminate(dpy);
	return wrong ? 1 : 0;
}
