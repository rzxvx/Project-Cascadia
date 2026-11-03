/* glpersp -- perspective-correct varyings (docs/research/p105-mesa.md,
 * M14): a full-screen quad whose left edge has w = 1 and right edge w = 3,
 * with a varying s = 0 on the left and 1 on the right in the quad's own
 * space.  Across the screen (lambda = 0..1) s is perspective-correct when
 * it is (lambda / 3) / ((1 - lambda) + lambda / 3), not lambda.
 *
 *   sgx-gl glpersp
 */
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define W 768
#define H 1024

int main(void)
{
	static const char *vs = "attribute vec4 p; varying float s;\n"
	                        "void main() {\n"
	                        "  float w = p.x < 0.0 ? 1.0 : 3.0;\n"
	                        "  s = p.x * 0.5 + 0.5;\n"
	                        "  gl_Position = vec4(p.xy * w, 0.0, w);\n"
	                        "}\n";
	static const char *fs = "precision highp float; varying float s;\n"
	                        "void main() { gl_FragColor = vec4(s, 0.0, 0.0, 1.0); }\n";
	static const float quad[] = { -1, -1, 0, 1,  1, -1, 0, 1,  1, 1, 0, 1,
	                              -1, -1, 0, 1,  1, 1, 0, 1,  -1, 1, 0, 1 };
	PFNEGLGETPLATFORMDISPLAYEXTPROC get_display;
	EGLint ctx_attrs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
	uint8_t *px = malloc(W * H * 4);
	int worst = 0, worst_affine = 0, wx = 0;
	GLuint rt, fbo, p;
	EGLDisplay dpy;
	EGLContext ctx;

	get_display = (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
	dpy = get_display ? get_display(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL) : NULL;
	if (!dpy || !eglInitialize(dpy, NULL, NULL) || !eglBindAPI(EGL_OPENGL_ES_API))
		return fprintf(stderr, "glpersp: no EGL\n"), 2;
	ctx = eglCreateContext(dpy, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, ctx_attrs);
	if (ctx == EGL_NO_CONTEXT || !eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx))
		return fprintf(stderr, "glpersp: no GLES 2 context\n"), 2;
	glGenTextures(1, &rt);
	glBindTexture(GL_TEXTURE_2D, rt);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	glGenFramebuffers(1, &fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, rt, 0);
	glViewport(0, 0, W, H);
	p = glCreateProgram();
	for (int i = 0; i < 2; i++) {
		GLuint s = glCreateShader(i ? GL_FRAGMENT_SHADER : GL_VERTEX_SHADER);

		glShaderSource(s, 1, i ? &fs : &vs, NULL);
		glCompileShader(s);
		glAttachShader(p, s);
	}
	glBindAttribLocation(p, 0, "p");
	glLinkProgram(p);
	glUseProgram(p);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 0, quad);
	glClearColor(0, 0, 0, 1);
	glClear(GL_COLOR_BUFFER_BIT);
	glDrawArrays(GL_TRIANGLES, 0, 6);
	glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px);

	for (int x = 8; x < W; x += 16) {
		float l = (x + 0.5f) / W, s = (l / 3) / ((1 - l) + l / 3);
		int got = px[(H / 3 * W + x) * 4], want = (int)(s * 255 + 0.5f);
		int affine = (int)(l * 255 + 0.5f);

		if (abs(got - want) > worst)
			worst = abs(got - want), wx = x;
		if (abs(got - affine) > worst_affine)
			worst_affine = abs(got - affine);
	}
	printf("perspective-correct: largest difference %d (at x %d); from the affine "
	       "interpolation: %d\n", worst, wx, worst_affine);
	printf("%s\n", worst <= 2 ? "ok" : "WRONG");
	eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	eglTerminate(dpy);
	free(px);
	return worst <= 2 ? 0 : 1;
}
