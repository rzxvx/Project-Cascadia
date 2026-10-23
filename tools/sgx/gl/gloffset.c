/* gloffset -- glPolygonOffset (docs/research/p105-mesa.md, M37): a quad at
 * window depth 0.5, flat, and one whose depth goes 0.25..0.75 across the
 * target, drawn with an offset (factor, units) and their depth read back at
 * the middle against GL's: z + factor * slope + units * r (the draw
 * module's r, or with SGX_ISP_OFFSET=1 the ISP's).  --clamp: whether an
 * offset depth past 1 is clamped (GLOFFSET_Z=z1,z2,units other depths).
 * --probe: the same quads with nothing asked of GL and
 * SGX_STATE=word:mask flipping the ISP's bits (finding them: --probe prints
 * the depths alone).
 *
 *   sgx-gl gloffset [--probe]
 *
 * Exit status 0 when every case passed.
 */
#include <EGL/egl.h>
#include <EGL/eglext.h>
#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include <GL/glext.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W 64
#define H 64

static int cases, failed;

/* the depth at the middle after a quad, its depth at the left z0 and at the
 * right z1 (window depths) */
static double
draw(double z0, double z1, float factor, float units, int offset)
{
	float d[4];

	glClearDepth(1.0);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	glEnable(GL_DEPTH_TEST);
	glDepthFunc(GL_ALWAYS);
	if (offset) {
		glEnable(GL_POLYGON_OFFSET_FILL);
		glPolygonOffset(factor, units);
	}
	glBegin(GL_QUADS);
	glVertex3f(-1, -1, 2 * z0 - 1);
	glVertex3f(1, -1, 2 * z1 - 1);
	glVertex3f(1, 1, 2 * z1 - 1);
	glVertex3f(-1, 1, 2 * z0 - 1);
	glEnd();
	glDisable(GL_POLYGON_OFFSET_FILL);
	glReadPixels(W / 2 - 1, H / 2, 2, 1, GL_DEPTH_COMPONENT, GL_FLOAT, d);
	return (d[0] + d[1]) / 2;
}

/* GL's r here: the ISP's unit, 32 of 24-bit depth's at depths 0.5..1
 * (half that below 0.5: floating point) -- iOS's units, its es1 e19 --
 * with SGX_ISP_OFFSET=1; the draw module's, 8 of them, without */
static double R = 8.0 / 16777216.0;

static void
check(const char *name, double z0, double z1, float factor, float units)
{
	double z = draw(z0, z1, factor, units, 1), base = draw(z0, z1, 0, 0, 0);
	/* the slope: the depth's change a pixel, the larger axis; the units of
	 * a triangle whose vertices reach below 0.5 half as much or the same
	 * (the middle is two triangles') */
	double slope = fabs(z1 - z0) / W, want = base + factor * slope + units * R;
	double tol = 2.5 / 16777216.0 + fabs(factor) * slope * 0.05 +
	             (z0 < 0.5 || z1 < 0.5 ? fabs(units) * R / 2 : 0);

	cases++;
	if (fabs(z - want) > tol) {
		failed++;
		printf("%-34s WRONG: %.9f, want %.9f (%+.2f r off; without the offset %.9f)\n",
		       name, z, want, (z - want) / R, base);
	} else {
		printf("%-34s ok   : %.9f (%+.2f r)\n", name, z, (z - base) / R);
	}
}

int
main(int argc, char **argv)
{
	PFNEGLGETPLATFORMDISPLAYEXTPROC get_display;
	EGLint ctx_attrs[] = { EGL_CONTEXT_MAJOR_VERSION, 2, EGL_CONTEXT_MINOR_VERSION, 1, EGL_NONE };
	GLuint fbo, rb[2];
	EGLDisplay dpy;
	EGLContext ctx;
	int probe = argc > 1 && !strcmp(argv[1], "--probe");

	get_display = (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
	dpy = get_display ? get_display(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL) : NULL;
	if (!dpy || !eglInitialize(dpy, NULL, NULL) || !eglBindAPI(EGL_OPENGL_API))
		return fprintf(stderr, "gloffset: no EGL\n"), 2;
	ctx = eglCreateContext(dpy, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, ctx_attrs);
	if (ctx == EGL_NO_CONTEXT || !eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx))
		return fprintf(stderr, "gloffset: no OpenGL 2.1 context\n"), 2;
	glGenFramebuffers(1, &fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	glGenRenderbuffers(2, rb);
	glBindRenderbuffer(GL_RENDERBUFFER, rb[0]);
	glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, W, H);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb[0]);
	glBindRenderbuffer(GL_RENDERBUFFER, rb[1]);
	glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, W, H);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, rb[1]);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, rb[1]);
	glViewport(0, 0, W, H);

	if (getenv("SGX_ISP_OFFSET") && atoi(getenv("SGX_ISP_OFFSET")))
		R = 32.0 / 16777216.0;
	if (argc > 1 && !strcmp(argv[1], "--clamp")) {
		/* whether the ISP clamps an offset depth to 1: a quad at the far
		 * plane offset 8 further, then one at the far plane drawn where it
		 * is nearer (LESS) -- seen if the first's went past 1 */
		static const float tri[] = { -1, -1, 1, -1, -1, 1, -1, 1, 1, -1, 1, 1 };
		uint8_t px[4];

		/* (triangles from an array: the GPU's vertex path, as dEQP's) */
		glEnableClientState(GL_VERTEX_ARRAY);
		glVertexPointer(2, GL_FLOAT, 0, tri);
		glClearColor(0, 0, 0, 0);
		glClearDepth(1.0);
		glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
		glEnable(GL_DEPTH_TEST);
		glDepthFunc(GL_ALWAYS);
		/* (GLOFFSET_Z=first,second,units: other depths, finding what
		 * the ISP's bits do) */
		{
			const char *e = getenv("GLOFFSET_Z");
			float z1 = 1, z2 = 1, u = 8;

			if (e)
				sscanf(e, "%f,%f,%f", &z1, &z2, &u);
			glDepthRange(z1, z1);     /* every depth the far plane's */
			glEnable(GL_POLYGON_OFFSET_FILL);
			glPolygonOffset(0, u);
			glColor4f(1, 1, 1, 1);
			glDrawArrays(GL_TRIANGLES, 0, 6);
			glDisable(GL_POLYGON_OFFSET_FILL);
			glDepthFunc(GL_LESS);
			glDepthRange(z2, z2);
			glColor4f(1, 0, 0, 1);
			glDrawArrays(GL_TRIANGLES, 0, 6);
		}
		glReadPixels(W / 2, H / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
		printf("%s\n", px[1] ? "clamped (white)" : px[0] ? "not clamped (red)" : "nothing");
		return 0;
	}
	if (probe) {
		/* what the state bits make of the quads, nothing asked of GL */
		double flat = draw(0.5, 0.5, 0, 0, 0), sloped = draw(0.25, 0.75, 0, 0, 0);

		printf("flat %.9f (%+.2f units)  sloped %.9f (%+.2f units)\n", flat,
		       (flat - 0.5) * 16777216.0, sloped, (sloped - 0.5) * 16777216.0);
		return 0;
	}
	check("units 1", 0.5, 0.5, 0, 1);
	check("units 2", 0.5, 0.5, 0, 2);
	check("units 15", 0.5, 0.5, 0, 15);
	check("units -4", 0.5, 0.5, 0, -4);
	check("units 2.5 (the draw module's)", 0.5, 0.5, 0, 2.5f);
	check("units 40 (the draw module's)", 0.5, 0.5, 0, 40);
	check("factor 1, flat", 0.5, 0.5, 1, 0);
	check("factor 1, sloped", 0.25, 0.75, 1, 0);
	check("factor 2, sloped", 0.25, 0.75, 2, 0);
	check("factor -1, sloped", 0.25, 0.75, -1, 0);
	check("factor 1, units 2, sloped", 0.25, 0.75, 1, 2);
	check("factor 1.5, sloped (the draw module's)", 0.25, 0.75, 1.5f, 0);
	printf("%d of %d cases right\n", cases - failed, cases);
	eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	eglTerminate(dpy);
	return failed ? 1 : 0;
}
