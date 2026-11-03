/* glwrap -- texture wrap modes (docs/research/p105-mesa.md, M24): a 4 x 4
 * texture, texel (i, j) red i, green j, drawn three times over across the
 * target (coordinates -1..2), nearest.  Read: the texel column along the
 * middle row (twelve places across), the texel row up the middle column;
 * each wrap mode on each axis, nine cases.
 *
 *   sgx-gl glwrap
 */
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define W 192
#define H 192

static uint8_t px[W * H * 4];

int main(void)
{
	static const char *vs = "attribute vec4 p; varying vec2 tc;\n"
	                        "void main() { tc = p.xy * 1.5 + 0.5; gl_Position = p; }\n";
	static const char *fs = "precision mediump float; varying vec2 tc; uniform sampler2D t;\n"
	                        "void main() { gl_FragColor = texture2D(t, tc); }\n";
	static const float q[] = { -1, -1, 0, 1, 1, -1, 0, 1, 1, 1, 0, 1,
	                           -1, -1, 0, 1, 1, 1, 0, 1, -1, 1, 0, 1 };
	static const struct {
		GLenum mode;
		const char *name, *want;
	} modes[] = {
		{ GL_REPEAT, "repeat", "012301230123" },
		{ GL_CLAMP_TO_EDGE, "clamp", "000001233333" },
		{ GL_MIRRORED_REPEAT, "mirror", "321001233210" },
	};
	PFNEGLGETPLATFORMDISPLAYEXTPROC get_display;
	EGLint ctx_attrs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
	uint8_t tex[4 * 4 * 4];
	int failed = 0;
	GLuint rt, fbo, p, t;
	EGLDisplay dpy;
	EGLContext ctx;

	for (int j = 0; j < 4; j++)
		for (int i = 0; i < 4; i++) {
			uint8_t *c = tex + (j * 4 + i) * 4;

			c[0] = i * 64 + 32;
			c[1] = j * 64 + 32;
			c[2] = 0;
			c[3] = 255;
		}
	get_display = (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
	dpy = get_display ? get_display(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL) : NULL;
	if (!dpy || !eglInitialize(dpy, NULL, NULL) || !eglBindAPI(EGL_OPENGL_ES_API))
		return fprintf(stderr, "glwrap: no EGL\n"), 2;
	ctx = eglCreateContext(dpy, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, ctx_attrs);
	if (ctx == EGL_NO_CONTEXT || !eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx))
		return fprintf(stderr, "glwrap: no GLES 2 context\n"), 2;
	glGenTextures(1, &rt);
	glBindTexture(GL_TEXTURE_2D, rt);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	glGenFramebuffers(1, &fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, rt, 0);
	glGenTextures(1, &t);
	glBindTexture(GL_TEXTURE_2D, t);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 4, 4, 0, GL_RGBA, GL_UNSIGNED_BYTE, tex);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
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
	glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 0, q);
	glViewport(0, 0, W, H);

	for (int ws = 0; ws < 3; ws++)
		for (int wt = 0; wt < 3; wt++) {
			char s[13], tt[13];
			int ok;

			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, modes[ws].mode);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, modes[wt].mode);
			glClear(GL_COLOR_BUFFER_BIT);
			glDrawArrays(GL_TRIANGLES, 0, 6);
			glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px);
			for (int k = 0; k < 12; k++) {
				s[k] = '0' + px[((H / 2 + 8) * W + k * 16 + 8) * 4] / 64;
				tt[k] = '0' + px[((k * 16 + 8) * W + W / 2 + 8) * 4 + 1] / 64;
			}
			s[12] = tt[12] = 0;
			ok = !strcmp(s, modes[ws].want) && !strcmp(tt, modes[wt].want);
			failed += !ok;
			printf("s %-6s t %-6s %s: s %s t %s\n", modes[ws].name, modes[wt].name,
			       ok ? "ok   " : "WRONG", s, tt);
		}
	printf("%d of 9 cases right\n", 9 - failed);
	eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	eglTerminate(dpy);
	return failed ? 1 : 0;
}
