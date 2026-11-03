/*
 * screenpad -- an on-screen controller: a preset's buttons, d-pads and
 * sticks drawn over whatever is on the screen, and worked by touch.
 *
 *   screenpad PRESET [-- COMMAND [ARG...]]
 *   screenpad -l                  the presets there are
 *
 * PRESET is a name (~/.config/screenpad/NAME.json, then the installed
 * share/screenpad/presets/NAME.json) or a file.  A preset (JSON; made with
 * screenpad-editor, its GTK editor) is a screen, landscape or portrait, and its
 * controls, each a place and a size in that screen's pixels, a colour, an
 * opacity, a label and what it does:
 *
 *   button   circle or rounded rectangle: "action" a key ("key:SPACE"), a
 *            gamepad button ("pad:A") or a gamepad's d-pad ("pad:UP")
 *   dpad     four arms: "up" "down" "left" "right", each such an action
 *   stick    "action" "pad:LSTICK" / "pad:RSTICK", an analogue stick; or
 *            four actions, as a d-pad that follows the finger
 *
 * A finger that lands on a control is the controller's: on a button it
 * presses whichever button it is over (it may slide from one to the
 * next), on a d-pad or a stick it works that one wherever it goes.  Every
 * other finger goes on to X as before.
 *
 * Drawn by the kernel: the controls go into a buffer of the GPU's render
 * node, set as the screen's overlay (apple-sgx's OVERLAY ioctl), which the
 * kernel blends over every frame it copies onto the screen -- over X, over
 * a fullscreen game that bypasses the compositor, over a program on KMS.
 * It shows while the screen is turned the preset's way (landscape or
 * portrait) and something is on KMS; otherwise it hides and keeps out of
 * the way.
 *
 * The touches: the touchscreen is grabbed (EVIOCGRAB) while the controls
 * show.  The fingers that are not the controller's go on to a touchscreen
 * of its own (uinput, "screenpad touch") in the turned screen's
 * coordinates, so X, which maps a new touchscreen to its whole screen,
 * needs no turn for it.  Keys go out of a uinput keyboard; the gamepad is
 * a uinput Xbox 360 pad (its IDs and layout), so SDL maps it as one.
 *
 * -- COMMAND: run it (a game) with the controls; when it ends, so does
 * screenpad.  Ctrl-C or kill end it too, and the screen and the
 * touchscreen are as before.  Root (the overlay, uinput, the grab).
 *
 * docs/research/p105-screenpad.md; tools/screenpad/build.sh builds and
 * installs it.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <drm/drm.h>
#include "apple_sgx_drm.h"

/* (the labels' glyphs; public domain, v1.26) */
#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#include "stb_truetype.h"
#pragma GCC diagnostic pop

#define MAX_CONTROLS	48
#define NSLOTS		10
#define CLONE_MAX	4095	/* the touch clone's axes: the turned screen, 0..4095 */
#define OV_PITCH	4096	/* the overlay buffer: 1024 pixels a line, either way */
#define OV_LINES	1024
#define FONT		"/usr/share/fonts/dejavu/DejaVuSans-Bold.ttf"

static void die(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	fputs("screenpad: ", stderr);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
	exit(1);
}

static double now(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec / 1e9;
}

/* ---- JSON: just enough for a preset ----------------------------------------- */

enum { J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ };

struct jv {
	int t;
	double n;
	char *s;
	int len;		/* members of an array or object */
	struct jv *v;
	char **k;		/* an object's keys */
};

static const char *jp, *jerr;

static void jws(void)
{
	while (*jp == ' ' || *jp == '\t' || *jp == '\n' || *jp == '\r')
		jp++;
}

static char *jstring(void)
{
	size_t n = 0, cap = 16;
	char *s = malloc(cap);

	jp++;					/* the quote */
	while (*jp && *jp != '"') {
		char c = *jp++;
		unsigned u = 0;

		if (n + 5 >= cap)
			s = realloc(s, cap *= 2);
		if (c != '\\') {
			s[n++] = c;
			continue;
		}
		switch (c = *jp++) {
		case 'b': s[n++] = '\b'; break;
		case 'f': s[n++] = '\f'; break;
		case 'n': s[n++] = '\n'; break;
		case 'r': s[n++] = '\r'; break;
		case 't': s[n++] = '\t'; break;
		case 'u':
			for (int i = 0; i < 4; i++, jp++) {
				char h = *jp;

				if (!h)
					break;
				u = u << 4 | (h <= '9' ? h - '0' : (h | 32) - 'a' + 10);
			}
			if (u >= 0xd800 && u < 0xe000)
				u = '?';		/* (no surrogate pairs) */
			if (u < 0x80) {
				s[n++] = u;
			} else if (u < 0x800) {
				s[n++] = 0xc0 | u >> 6;
				s[n++] = 0x80 | (u & 0x3f);
			} else {
				s[n++] = 0xe0 | u >> 12;
				s[n++] = 0x80 | (u >> 6 & 0x3f);
				s[n++] = 0x80 | (u & 0x3f);
			}
			break;
		default:
			if (!c) {
				jp--;
				break;
			}
			s[n++] = c;
		}
	}
	if (*jp != '"') {
		jerr = "a string does not end";
		free(s);
		return NULL;
	}
	jp++;
	s[n] = 0;
	return s;
}

static int jvalue(struct jv *o, int depth)
{
	memset(o, 0, sizeof(*o));
	jws();
	if (depth > 32)
		return jerr = "nested too deep", -1;
	if (*jp == '{' || *jp == '[') {
		int obj = *jp++ == '{';

		o->t = obj ? J_OBJ : J_ARR;
		jws();
		if (*jp == (obj ? '}' : ']')) {
			jp++;
			return 0;
		}
		for (;;) {
			char *key = NULL;

			jws();
			if (obj) {
				if (*jp != '"')
					return jerr = "a key is not a string", -1;
				if (!(key = jstring()))
					return -1;
				jws();
				if (*jp++ != ':') {
					free(key);
					return jerr = "no ':' after a key", -1;
				}
			}
			o->v = realloc(o->v, (o->len + 1) * sizeof(*o->v));
			if (obj)
				o->k = realloc(o->k, (o->len + 1) * sizeof(*o->k));
			if (obj)
				o->k[o->len] = key;
			if (jvalue(&o->v[o->len++], depth + 1))
				return -1;
			jws();
			if (*jp == ',') {
				jp++;
				continue;
			}
			if (*jp++ == (obj ? '}' : ']'))
				return 0;
			return jerr = obj ? "no ',' or '}' in an object" : "no ',' or ']' in an array", -1;
		}
	}
	if (*jp == '"') {
		o->t = J_STR;
		return (o->s = jstring()) ? 0 : -1;
	}
	if (!strncmp(jp, "true", 4) || !strncmp(jp, "false", 5)) {
		o->t = J_BOOL;
		o->n = *jp == 't';
		jp += o->n ? 4 : 5;
		return 0;
	}
	if (!strncmp(jp, "null", 4)) {
		jp += 4;
		return 0;
	}
	if (*jp == '-' || (*jp >= '0' && *jp <= '9')) {
		char *end;

		o->t = J_NUM;
		o->n = strtod(jp, &end);
		jp = end;
		return 0;
	}
	return jerr = "not a value", -1;
}

static void jfree(struct jv *o)
{
	for (int i = 0; i < o->len; i++) {
		jfree(&o->v[i]);
		if (o->k)
			free(o->k[i]);
	}
	free(o->v);
	free(o->k);
	free(o->s);
}

static const struct jv *jget(const struct jv *o, const char *key)
{
	for (int i = 0; o && o->t == J_OBJ && i < o->len; i++)
		if (!strcmp(o->k[i], key))
			return &o->v[i];
	return NULL;
}

static double jnum(const struct jv *o, const char *key, double def)
{
	const struct jv *v = jget(o, key);

	return v && v->t == J_NUM ? v->n : def;
}

static const char *jstr(const struct jv *o, const char *key, const char *def)
{
	const struct jv *v = jget(o, key);

	return v && v->t == J_STR ? v->s : def;
}

/* ---- actions ------------------------------------------------------------------ */

enum { A_NONE, A_KEY, A_BTN, A_HAT, A_TRIGGER, A_LSTICK, A_RSTICK };

struct action {
	int kind, code;		/* KEY_*; BTN_*; the hat's direction 0..3; ABS_Z/RZ */
};

#define K(n) { #n, KEY_##n }
static const struct { const char *name; int code; } keys[] = {
	K(ESC), K(1), K(2), K(3), K(4), K(5), K(6), K(7), K(8), K(9), K(0), K(MINUS),
	K(EQUAL), K(BACKSPACE), K(TAB), K(Q), K(W), K(E), K(R), K(T), K(Y), K(U), K(I),
	K(O), K(P), K(LEFTBRACE), K(RIGHTBRACE), K(ENTER), K(LEFTCTRL), K(A), K(S), K(D),
	K(F), K(G), K(H), K(J), K(K), K(L), K(SEMICOLON), K(APOSTROPHE), K(GRAVE),
	K(LEFTSHIFT), K(BACKSLASH), K(Z), K(X), K(C), K(V), K(B), K(N), K(M), K(COMMA),
	K(DOT), K(SLASH), K(RIGHTSHIFT), K(LEFTALT), K(SPACE), K(CAPSLOCK), K(F1), K(F2),
	K(F3), K(F4), K(F5), K(F6), K(F7), K(F8), K(F9), K(F10), K(F11), K(F12),
	K(RIGHTCTRL), K(RIGHTALT), K(HOME), K(UP), K(PAGEUP), K(LEFT), K(RIGHT), K(END),
	K(DOWN), K(PAGEDOWN), K(INSERT), K(DELETE), K(PAUSE), K(LEFTMETA), K(RIGHTMETA),
	K(KPPLUS), K(KPMINUS), K(VOLUMEUP), K(VOLUMEDOWN), K(MUTE),
	{ "CTRL", KEY_LEFTCTRL }, { "SHIFT", KEY_LEFTSHIFT }, { "ALT", KEY_LEFTALT },
	{ "RETURN", KEY_ENTER }, { "ESCAPE", KEY_ESC },
};

/* the Xbox 360 pad's buttons, as xpad reports them */
static const struct { const char *name; int code; } pad_buttons[] = {
	{ "A", BTN_A }, { "B", BTN_B }, { "X", BTN_X }, { "Y", BTN_Y }, { "LB", BTN_TL },
	{ "RB", BTN_TR }, { "BACK", BTN_SELECT }, { "SELECT", BTN_SELECT },
	{ "START", BTN_START }, { "GUIDE", BTN_MODE }, { "LS", BTN_THUMBL }, { "RS", BTN_THUMBR },
};

static int uses_keys, uses_pad;

static struct action parse_action(const char *s)
{
	struct action a = { A_NONE, 0 };
	static const char *hat[] = { "UP", "DOWN", "LEFT", "RIGHT" };
	char up[32];
	size_t i;

	if (!s)
		return a;
	for (i = 0; s[i] && i < sizeof(up) - 1; i++)
		up[i] = s[i] >= 'a' && s[i] <= 'z' ? s[i] - 32 : s[i];
	up[i] = 0;
	if (!strncmp(up, "KEY:", 4)) {
		for (i = 0; i < sizeof(keys) / sizeof(*keys); i++)
			if (!strcmp(up + 4, keys[i].name)) {
				a.kind = A_KEY;
				a.code = keys[i].code;
				uses_keys = 1;
				return a;
			}
	} else if (!strncmp(up, "PAD:", 4)) {
		const char *n = up + 4;

		uses_pad = 1;
		for (i = 0; i < sizeof(pad_buttons) / sizeof(*pad_buttons); i++)
			if (!strcmp(n, pad_buttons[i].name)) {
				a.kind = A_BTN;
				a.code = pad_buttons[i].code;
				return a;
			}
		for (i = 0; i < 4; i++)
			if (!strcmp(n, hat[i])) {
				a.kind = A_HAT;
				a.code = i;
				return a;
			}
		if (!strcmp(n, "LT") || !strcmp(n, "RT")) {
			a.kind = A_TRIGGER;
			a.code = n[0] == 'L' ? ABS_Z : ABS_RZ;
			return a;
		}
		if (!strcmp(n, "LSTICK") || !strcmp(n, "RSTICK")) {
			a.kind = n[0] == 'L' ? A_LSTICK : A_RSTICK;
			return a;
		}
	}
	fprintf(stderr, "screenpad: \"%s\": no such action\n", s);
	return a;
}

/* ---- the preset ------------------------------------------------------------------ */

enum { C_BUTTON, C_DPAD, C_STICK };
enum { DIR_UP, DIR_DOWN, DIR_LEFT, DIR_RIGHT };

struct control {
	int type, circle;
	float x, y, w, h, radius;	/* the screen's pixels */
	uint32_t rgb;
	float opacity;
	char label[48];
	struct action act, dir[4];
	/* now */
	int on;				/* a button: pressed; a d-pad: a bit per direction */
	float sx, sy;			/* a stick: -1..1 */
};

static struct {
	char name[64];
	int landscape;
	int n;
	struct control c[MAX_CONTROLS];
} P;

static uint32_t parse_rgb(const char *s, uint32_t def)
{
	unsigned v;

	if (s && s[0] == '#' && strlen(s) == 7 && sscanf(s + 1, "%x", &v) == 1)
		return v;
	return def;
}

/* JSON text -> P; on a mistake P is as it was and *err says what */
static int preset_load(const char *text, const char **err)
{
	static const char *dirs[] = { "up", "down", "left", "right" };
	struct jv root, *cs;
	int ok = -1;

	jp = text;
	jerr = NULL;
	if (jvalue(&root, 0)) {
		*err = jerr;
		jfree(&root);
		return -1;
	}
	cs = (struct jv *)jget(&root, "controls");
	if (root.t != J_OBJ || !cs || cs->t != J_ARR) {
		*err = "no \"controls\" array";
	} else if (cs->len > MAX_CONTROLS) {
		*err = "too many controls";
	} else {
		const char *scr = jstr(&root, "screen", "landscape");

		uses_keys = uses_pad = 0;
		memset(&P.c, 0, sizeof(P.c));
		snprintf(P.name, sizeof(P.name), "%s", jstr(&root, "name", "preset"));
		P.landscape = strcmp(scr, "portrait") != 0;
		P.n = cs->len;
		for (int i = 0; i < P.n; i++) {
			struct control *c = &P.c[i];
			const struct jv *o = &cs->v[i];
			const char *t = jstr(o, "type", "button");

			c->type = !strcmp(t, "dpad") ? C_DPAD : !strcmp(t, "stick") ? C_STICK : C_BUTTON;
			c->circle = !strcmp(jstr(o, "shape", c->type == C_BUTTON ? "circle" : ""), "circle");
			c->x = jnum(o, "x", 0);
			c->y = jnum(o, "y", 0);
			c->w = fmaxf(16, jnum(o, "w", 120));
			c->h = fmaxf(16, jnum(o, "h", c->w));
			c->radius = jnum(o, "radius", fminf(c->w, c->h) * 0.25f);
			c->rgb = parse_rgb(jstr(o, "color", NULL), 0xffffff);
			c->opacity = fminf(1, fmaxf(0.05f, jnum(o, "opacity", 0.4)));
			snprintf(c->label, sizeof(c->label), "%s", jstr(o, "label", ""));
			c->act = parse_action(jstr(o, "action", NULL));
			for (int d = 0; d < 4; d++)
				c->dir[d] = parse_action(jstr(o, dirs[d], NULL));
			if (c->type == C_DPAD && !c->dir[0].kind && !c->dir[1].kind && !c->dir[2].kind &&
			    !c->dir[3].kind) {		/* a d-pad with none: the arrow keys */
				c->dir[0] = parse_action("key:UP");
				c->dir[1] = parse_action("key:DOWN");
				c->dir[2] = parse_action("key:LEFT");
				c->dir[3] = parse_action("key:RIGHT");
			}
		}
		ok = 0;
	}
	jfree(&root);
	return ok;
}

static char *read_file(const char *path, size_t max)
{
	FILE *f = fopen(path, "rb");
	char *s;
	size_t n;

	if (!f)
		return NULL;
	s = malloc(max + 1);
	n = fread(s, 1, max, f);
	fclose(f);
	s[n] = 0;
	return s;
}

static char share[PATH_MAX + 32], userdir[PATH_MAX];

static void paths(void)
{
	char exe[PATH_MAX];
	ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
	const char *home = getenv("HOME");

	if (getenv("SCREENPAD_SHARE")) {
		snprintf(share, sizeof(share), "%s", getenv("SCREENPAD_SHARE"));
	} else if (n > 0) {
		exe[n] = 0;
		*strrchr(exe, '/') = 0;		/* .../bin */
		snprintf(share, sizeof(share), "%s/../share/screenpad", exe);
	}
	snprintf(userdir, sizeof(userdir), "%s/.config/screenpad", home ? home : "/root");
}

static int valid_name(const char *s)
{
	size_t n = strlen(s);

	if (!n || n > 40)
		return 0;
	for (; *s; s++)
		if (!(*s == '-' || *s == '_' || (*s >= '0' && *s <= '9') ||
		      ((*s | 32) >= 'a' && (*s | 32) <= 'z')))
			return 0;
	return 1;
}

/* a preset's text by its name (the user's first) or a path */
static char *preset_text(const char *name)
{
	char path[PATH_MAX * 2];
	char *s;

	if (strchr(name, '/') || strstr(name, ".json"))
		return read_file(name, 1 << 20);
	snprintf(path, sizeof(path), "%s/%s.json", userdir, name);
	if ((s = read_file(path, 1 << 20)))
		return s;
	snprintf(path, sizeof(path), "%s/presets/%s.json", share, name);
	return read_file(path, 1 << 20);
}

/* the presets' names, the user's and the installed ones, one a line */
static void list_presets(void)
{
	char sharep[sizeof(share) + 16], seen[4096] = " ";
	const char *dirs[2] = { userdir, sharep };

	snprintf(sharep, sizeof(sharep), "%s/presets", share);
	for (int d = 0; d < 2; d++) {
		DIR *dir = opendir(dirs[d]);
		struct dirent *e;

		while (dir && (e = readdir(dir))) {
			char *dot = strrchr(e->d_name, '.'), name[64], key[70];

			if (!dot || strcmp(dot, ".json") || dot - e->d_name >= (int)sizeof(name))
				continue;
			snprintf(name, sizeof(name), "%.*s", (int)(dot - e->d_name), e->d_name);
			snprintf(key, sizeof(key), " %s ", name);
			if (!valid_name(name) || strstr(seen, key) ||
			    strlen(seen) + strlen(key) >= sizeof(seen))
				continue;
			strcat(seen, key + 1);
			puts(name);
		}
		if (dir)
			closedir(dir);
	}
}

/* ---- the overlay: the render node's buffer the kernel blends ------------------- */

static int gpu = -1;
static uint32_t ov_handle;
static uint8_t *ov_map;
static int scr_w, scr_h;		/* the plane now (0 0: nothing on KMS) */
static uint32_t scr_rot = DRM_MODE_ROTATE_0;
static int shown;

static void ov_open(void)
{
	struct drm_apple_sgx_gem_create c = { .size = (uint64_t)OV_PITCH * OV_LINES };
	struct drm_apple_sgx_gem_mmap_offset m = { 0 };
	struct drm_apple_sgx_overlay q = { .flags = APPLE_SGX_OVERLAY_QUERY };

	gpu = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
	if (gpu < 0)
		die("/dev/dri/renderD128: %s", strerror(errno));
	/* (asked for: the interface's version did not change with it) */
	if (ioctl(gpu, DRM_IOCTL_APPLE_SGX_OVERLAY, &q))
		die("this kernel's apple-sgx has no screen overlay (%s): one from "
		    "2026-10-23 or later", strerror(errno));
	if (ioctl(gpu, DRM_IOCTL_APPLE_SGX_GEM_CREATE, &c))
		die("a buffer for the overlay: %s", strerror(errno));
	ov_handle = c.handle;
	m.handle = c.handle;
	if (ioctl(gpu, DRM_IOCTL_APPLE_SGX_GEM_MMAP_OFFSET, &m))
		die("the overlay's mmap offset: %s", strerror(errno));
	ov_map = mmap(NULL, c.size, PROT_READ | PROT_WRITE, MAP_SHARED, gpu, m.offset);
	if (ov_map == MAP_FAILED)
		die("mmap of the overlay: %s", strerror(errno));
	memset(ov_map, 0, c.size);
}

/* set it (n rectangles of what ov_map has), or take it away (n < 0); the
 * plane's size and turn come back either way */
static int ov_set(const struct drm_apple_sgx_overlay_rect *r, int n, int query)
{
	struct drm_apple_sgx_overlay o = { 0 };

	if (query) {
		o.flags = APPLE_SGX_OVERLAY_QUERY;
	} else if (n >= 0) {
		o.handle = ov_handle;
		o.pitch = OV_PITCH;
		o.width = scr_w;
		o.height = scr_h;
		o.nrects = n;
		memcpy(o.rects, r, n * sizeof(*r));
	}
	if (ioctl(gpu, DRM_IOCTL_APPLE_SGX_OVERLAY, &o)) {
		if (errno == EBUSY)
			die("another program has the screen's overlay (another screenpad?)");
		fprintf(stderr, "screenpad: the overlay: %s\n", strerror(errno));
		return -1;
	}
	scr_w = o.out_width;
	scr_h = o.out_height;
	scr_rot = o.out_rotation & 0xf;
	return 0;
}

/* ---- drawing: straight-alpha BGRA, into a canvas the screen's size ------------ */

static uint32_t *cv;			/* scr_w x scr_h */
static int cw, ch;
static int clip_x0, clip_y0, clip_x1, clip_y1;	/* what may be drawn on */
static stbtt_fontinfo font;
static int have_font;

static void put(int x, int y, uint32_t rgb, float a)
{
	uint32_t *p, d;
	float da, oa, r, g, b;

	if (a <= 0.004f || x < clip_x0 || y < clip_y0 || x >= clip_x1 || y >= clip_y1)
		return;
	if (a > 1)
		a = 1;
	p = &cv[y * cw + x];
	d = *p;
	da = (d >> 24) / 255.f;
	oa = a + da * (1 - a);
	r = ((rgb >> 16 & 255) * a + (d >> 16 & 255) * da * (1 - a)) / oa;
	g = ((rgb >> 8 & 255) * a + (d >> 8 & 255) * da * (1 - a)) / oa;
	b = ((rgb & 255) * a + (d & 255) * da * (1 - a)) / oa;
	*p = (uint32_t)(oa * 255 + 0.5f) << 24 | (uint32_t)(r + 0.5f) << 16 |
	     (uint32_t)(g + 0.5f) << 8 | (uint32_t)(b + 0.5f);
}

static float cover(float sdf)		/* a signed distance -> coverage */
{
	return sdf <= -0.5f ? 1 : sdf >= 0.5f ? 0 : 0.5f - sdf;
}

static float sdf_rrect(float px, float py, float x0, float y0, float x1, float y1, float r)
{
	float cx = (x0 + x1) / 2, cy = (y0 + y1) / 2;
	float qx = fabsf(px - cx) - ((x1 - x0) / 2 - r), qy = fabsf(py - cy) - ((y1 - y0) / 2 - r);
	float ox = fmaxf(qx, 0), oy = fmaxf(qy, 0);

	return sqrtf(ox * ox + oy * oy) + fminf(fmaxf(qx, qy), 0) - r;
}

/* a filled rounded rectangle (r = half the shorter side: a circle or a
 * capsule), with a border of its own colour and alpha */
static void shape(float x0, float y0, float x1, float y1, float r, uint32_t rgb, float a,
		  uint32_t brgb, float ba, float bw)
{
	r = fminf(r, fminf(x1 - x0, y1 - y0) / 2);
	for (int y = (int)floorf(y0) - 1; y <= (int)ceilf(y1); y++)
		for (int x = (int)floorf(x0) - 1; x <= (int)ceilf(x1); x++) {
			float d = sdf_rrect(x + 0.5f, y + 0.5f, x0, y0, x1, y1, r);

			put(x, y, rgb, a * cover(d));
			if (bw > 0)
				put(x, y, brgb, ba * cover(fabsf(d + bw / 2) - bw / 2));
		}
}

/* a plus, two bars across a centre, filled and bordered as one */
static void plus(float cx, float cy, float h, float t, float r, uint32_t rgb, float a,
		 uint32_t brgb, float ba, float bw)
{
	for (int y = (int)floorf(cy - h) - 1; y <= (int)ceilf(cy + h); y++)
		for (int x = (int)floorf(cx - h) - 1; x <= (int)ceilf(cx + h); x++) {
			float px = x + 0.5f, py = y + 0.5f;
			float d = fminf(sdf_rrect(px, py, cx - h, cy - t / 2, cx + h, cy + t / 2, r),
					sdf_rrect(px, py, cx - t / 2, cy - h, cx + t / 2, cy + h, r));

			put(x, y, rgb, a * cover(d));
			put(x, y, brgb, ba * cover(fabsf(d + bw / 2) - bw / 2));
		}
}

/* a triangle, 4 x 4 samples a pixel */
static void tri(float ax, float ay, float bx, float by, float cx, float cy, uint32_t rgb, float a)
{
	int x0 = floorf(fminf(ax, fminf(bx, cx))), x1 = ceilf(fmaxf(ax, fmaxf(bx, cx)));
	int y0 = floorf(fminf(ay, fminf(by, cy))), y1 = ceilf(fmaxf(ay, fmaxf(by, cy)));
	float area = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);

	if (fabsf(area) < 1e-3f)
		return;
	for (int y = y0; y <= y1; y++)
		for (int x = x0; x <= x1; x++) {
			int in = 0;

			for (int s = 0; s < 16; s++) {
				float px = x + (s % 4 + 0.5f) / 4, py = y + (s / 4 + 0.5f) / 4;
				float w0 = (bx - ax) * (py - ay) - (by - ay) * (px - ax);
				float w1 = (cx - bx) * (py - by) - (cy - by) * (px - bx);
				float w2 = (ax - cx) * (py - cy) - (ay - cy) * (px - cx);

				in += area > 0 ? (w0 >= 0 && w1 >= 0 && w2 >= 0) :
						 (w0 <= 0 && w1 <= 0 && w2 <= 0);
			}
			put(x, y, rgb, a * in / 16);
		}
}

static uint32_t utf8_next(const char **s)
{
	const unsigned char *p = (const unsigned char *)*s;
	uint32_t c = *p++;
	int n = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : c >= 0xc0 ? 1 : 0;

	if (n)
		c &= 0x3f >> n;
	while (n-- && (*p & 0xc0) == 0x80)
		c = c << 6 | (*p++ & 0x3f);
	*s = (const char *)p;
	return c;
}

/* text centred on cx, cy, px high (ascent to descent), narrowed to maxw */
static void text(const char *s, float cx, float cy, float px, float maxw, uint32_t rgb, float a)
{
	float scale, width = 0, x;
	int asc, desc, gap;
	const char *p;

	if (!have_font || !*s)
		return;
	scale = stbtt_ScaleForPixelHeight(&font, px);
	stbtt_GetFontVMetrics(&font, &asc, &desc, &gap);
	for (p = s; *p;) {
		int adv, lsb;

		stbtt_GetCodepointHMetrics(&font, utf8_next(&p), &adv, &lsb);
		width += adv * scale;
	}
	if (width > maxw) {
		scale *= maxw / width;
		width = maxw;
	}
	x = cx - width / 2;
	for (p = s; *p;) {
		uint32_t c = utf8_next(&p);
		int adv, lsb, w, h, xo, yo;
		unsigned char *bm = stbtt_GetCodepointBitmap(&font, scale, scale, c, &w, &h, &xo, &yo);
		/* the baseline: the text's ascent and descent centred */
		int base = (int)(cy + (asc + desc) * scale / 2 + 0.5f);

		for (int j = 0; bm && j < h; j++)
			for (int i = 0; i < w; i++)
				put((int)(x + 0.5f) + xo + i, base + yo + j, rgb, a * bm[j * w + i] / 255.f);
		stbtt_FreeBitmap(bm, NULL);
		stbtt_GetCodepointHMetrics(&font, c, &adv, &lsb);
		x += adv * scale;
	}
}

static uint32_t lighter(uint32_t rgb, float t)
{
	uint32_t r = rgb >> 16 & 255, g = rgb >> 8 & 255, b = rgb & 255;

	r += (255 - r) * t;
	g += (255 - g) * t;
	b += (255 - b) * t;
	return r << 16 | g << 8 | b;
}

static void draw_control(const struct control *c)
{
	float x0 = c->x, y0 = c->y, x1 = c->x + c->w, y1 = c->y + c->h;
	float a = c->opacity, ba = fminf(1, a * 1.8f), la = fminf(1, a * 2.2f + 0.1f);
	uint32_t hi = lighter(c->rgb, 0.45f);

	if (c->type == C_BUTTON) {
		float r = c->circle ? fminf(c->w, c->h) / 2 : c->radius;

		shape(x0, y0, x1, y1, r, c->on ? hi : c->rgb, c->on ? fminf(1, a * 1.9f) : a,
		      hi, ba, 3);
		text(c->label, (x0 + x1) / 2, (y0 + y1) / 2, fminf(c->w * 0.8f, c->h * 0.42f),
		     c->w * (c->circle ? 0.7f : 0.8f), 0xffffff, la);
	} else if (c->type == C_DPAD) {
		float s = fminf(c->w, c->h), cx = (x0 + x1) / 2, cy = (y0 + y1) / 2, t = s / 3;
		float h = s / 2, r = t * 0.25f, k = t * 0.22f;
		/* the four arms, each a rounded rectangle from the centre out */
		const float arm[4][4] = {
			{ cx - t / 2, cy - h, cx + t / 2, cy },		/* up */
			{ cx - t / 2, cy, cx + t / 2, cy + h },		/* down */
			{ cx - h, cy - t / 2, cx, cy + t / 2 },		/* left */
			{ cx, cy - t / 2, cx + h, cy + t / 2 },		/* right */
		};

		plus(cx, cy, h, t, r, c->rgb, a, hi, ba, 3);
		for (int d = 0; d < 4; d++) {
			const float *m = arm[d];
			float ax = (m[0] + m[2]) / 2, ay = (m[1] + m[3]) / 2;
			float dx = d == DIR_LEFT ? -1 : d == DIR_RIGHT ? 1 : 0;
			float dy = d == DIR_UP ? -1 : d == DIR_DOWN ? 1 : 0;

			if (c->on & 1 << d)
				shape(m[0], m[1], m[2], m[3], r, hi, fminf(1, a * 1.9f), 0, 0, 0);
			/* its arrow, pointing out, in the arm's outer half */
			ax += dx * h / 4;
			ay += dy * h / 4;
			tri(ax + dx * k, ay + dy * k, ax - dx * k - dy * k, ay - dy * k + dx * k,
			    ax - dx * k + dy * k, ay - dy * k - dx * k, 0xffffff, la);
		}
	} else {
		float s = fminf(c->w, c->h), cx = (x0 + x1) / 2, cy = (y0 + y1) / 2;
		float R = s / 2, kr = s * 0.21f, reach = R - kr;
		float kx = cx + c->sx * reach, ky = cy + c->sy * reach;

		shape(cx - R, cy - R, cx + R, cy + R, R, c->rgb, a * 0.7f, hi, ba, 3);
		shape(kx - kr, ky - kr, kx + kr, ky + kr, kr, c->on ? hi : c->rgb,
		      fminf(1, a * (c->on ? 2.2f : 1.6f)), hi, ba, 2);
		text(c->label, kx, ky, kr * 0.9f, kr * 1.6f, 0xffffff, la);
	}
}

struct box {
	int x0, y0, x1, y1;
};

/* a control's box on the screen, out to 16 pixels as the kernel blends;
 * 0 if it is off the screen */
static int box_of(const struct control *c, struct box *b)
{
	b->x0 = (int)floorf(c->x) - 3;
	b->y0 = (int)floorf(c->y) - 3;
	b->x1 = (int)ceilf(c->x + c->w) + 3;
	b->y1 = (int)ceilf(c->y + c->h) + 3;
	b->x0 = b->x0 < 0 ? 0 : b->x0 & ~15;
	b->y0 = b->y0 < 0 ? 0 : b->y0 & ~15;
	b->x1 = (b->x1 + 15) & ~15;
	b->y1 = (b->y1 + 15) & ~15;
	b->x1 = b->x1 > scr_w ? scr_w : b->x1;
	b->y1 = b->y1 > scr_h ? scr_h : b->y1;
	return b->x1 > b->x0 && b->y1 > b->y0;
}

static int boxes_meet(const struct box *a, const struct box *b)
{
	return a->x0 < b->x1 && b->x0 < a->x1 && a->y0 < b->y1 && b->y0 < a->y1;
}

/* the controls' boxes, merged down to the kernel's 16 */
static int boxes(struct drm_apple_sgx_overlay_rect *out)
{
	struct box b[MAX_CONTROLS];
	int n = 0;

	for (int i = 0; i < P.n; i++)
		n += box_of(&P.c[i], &b[n]);
	while (n > APPLE_SGX_OVERLAY_MAX_RECTS) {	/* the pair whose union grows least */
		long best = -1;
		int bi = 0, bj = 1;

		for (int i = 0; i < n; i++)
			for (int j = i + 1; j < n; j++) {
				int x0 = b[i].x0 < b[j].x0 ? b[i].x0 : b[j].x0;
				int y0 = b[i].y0 < b[j].y0 ? b[i].y0 : b[j].y0;
				int x1 = b[i].x1 > b[j].x1 ? b[i].x1 : b[j].x1;
				int y1 = b[i].y1 > b[j].y1 ? b[i].y1 : b[j].y1;
				long grow = (long)(x1 - x0) * (y1 - y0) -
					    (long)(b[i].x1 - b[i].x0) * (b[i].y1 - b[i].y0) -
					    (long)(b[j].x1 - b[j].x0) * (b[j].y1 - b[j].y0);

				if (best < 0 || grow < best) {
					best = grow < 0 ? 0 : grow;
					bi = i;
					bj = j;
				}
			}
		b[bi].x0 = b[bi].x0 < b[bj].x0 ? b[bi].x0 : b[bj].x0;
		b[bi].y0 = b[bi].y0 < b[bj].y0 ? b[bi].y0 : b[bj].y0;
		b[bi].x1 = b[bi].x1 > b[bj].x1 ? b[bi].x1 : b[bj].x1;
		b[bi].y1 = b[bi].y1 > b[bj].y1 ? b[bi].y1 : b[bj].y1;
		b[bj] = b[--n];
	}
	for (int i = 0; i < n; i++) {
		out[i].x = b[i].x0;
		out[i].y = b[i].y0;
		out[i].w = b[i].x1 - b[i].x0;
		out[i].h = b[i].y1 - b[i].y0;
	}
	return n;
}

static int dirty[MAX_CONTROLS], full_redraw = 1;

/* box b of the canvas drawn again -- every control that reaches it, in
 * order, kept within it -- and put into the overlay's buffer */
static void redraw_box(const struct box *b)
{
	struct box o;

	clip_x0 = b->x0;
	clip_y0 = b->y0;
	clip_x1 = b->x1;
	clip_y1 = b->y1;
	for (int y = b->y0; y < b->y1; y++)
		memset(&cv[y * cw + b->x0], 0, (b->x1 - b->x0) * 4);
	for (int j = 0; j < P.n; j++)
		if (box_of(&P.c[j], &o) && boxes_meet(&o, b))
			draw_control(&P.c[j]);
	for (int y = b->y0; y < b->y1; y++)
		memcpy(ov_map + (size_t)y * OV_PITCH + b->x0 * 4, &cv[y * cw + b->x0],
		       (b->x1 - b->x0) * 4);
}

/* what changed drawn again (everything after a new preset or screen), and
 * the overlay set: drawing is the CPU's, so only the controls that changed */
static void redraw(void)
{
	struct drm_apple_sgx_overlay_rect r[APPLE_SGX_OVERLAY_MAX_RECTS];
	struct box b;

	if (!shown)
		return;
	if (cw != scr_w || ch != scr_h || !cv) {
		free(cv);
		cw = scr_w;
		ch = scr_h;
		cv = calloc((size_t)cw * ch, 4);
		memset(ov_map, 0, (size_t)OV_PITCH * OV_LINES);
		full_redraw = 1;
	}
	if (full_redraw) {
		b = (struct box){ 0, 0, cw, ch };
		clip_x0 = clip_y0 = 0;
		clip_x1 = cw;
		clip_y1 = ch;
		memset(cv, 0, (size_t)cw * ch * 4);
		for (int i = 0; i < P.n; i++)
			draw_control(&P.c[i]);
		for (int y = 0; y < ch; y++)
			memcpy(ov_map + (size_t)y * OV_PITCH, &cv[y * cw], cw * 4);
	} else {
		for (int i = 0; i < P.n; i++)
			if (dirty[i] && box_of(&P.c[i], &b))
				redraw_box(&b);
	}
	memset(dirty, 0, sizeof(dirty));
	full_redraw = 0;
	ov_set(r, boxes(r), 0);
}

/* ---- uinput: the keyboard, the gamepad, the touchscreen for X ------------------- */

static int kbd = -1, pad = -1, clone = -1;

static void emit(int fd, int type, int code, int value)
{
	struct input_event e = { .type = type, .code = code, .value = value };

	if (fd >= 0 && write(fd, &e, sizeof(e)) != sizeof(e))
		perror("screenpad: uinput");
}

static int uinput_new(void)
{
	int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC);

	if (fd < 0)
		die("/dev/uinput: %s", strerror(errno));
	return fd;
}

static void uinput_up(int fd, const char *name, uint16_t bus, uint16_t vendor,
		      uint16_t product, uint16_t version)
{
	struct uinput_setup s = { .id = { bus, vendor, product, version } };

	snprintf(s.name, sizeof(s.name), "%s", name);
	if (ioctl(fd, UI_DEV_SETUP, &s) || ioctl(fd, UI_DEV_CREATE))
		die("uinput %s: %s", name, strerror(errno));
}

static void abs_setup(int fd, int code, int min, int max, int flat)
{
	struct uinput_abs_setup a = { .code = code, .absinfo = { .minimum = min, .maximum = max,
								 .flat = flat } };

	ioctl(fd, UI_SET_ABSBIT, code);
	if (ioctl(fd, UI_ABS_SETUP, &a))
		die("uinput axis %d: %s", code, strerror(errno));
}

static void uinput_open(void)
{
	if (uses_keys && kbd < 0) {
		kbd = uinput_new();
		ioctl(kbd, UI_SET_EVBIT, EV_KEY);
		for (size_t i = 0; i < sizeof(keys) / sizeof(*keys); i++)
			ioctl(kbd, UI_SET_KEYBIT, keys[i].code);
		uinput_up(kbd, "screenpad keys", BUS_VIRTUAL, 0x1209, 0x5350, 1);
	}
	if (uses_pad && pad < 0) {
		static const int axes[] = { ABS_X, ABS_Y, ABS_RX, ABS_RY };

		pad = uinput_new();
		ioctl(pad, UI_SET_EVBIT, EV_KEY);
		ioctl(pad, UI_SET_EVBIT, EV_ABS);
		for (size_t i = 0; i < sizeof(pad_buttons) / sizeof(*pad_buttons); i++)
			ioctl(pad, UI_SET_KEYBIT, pad_buttons[i].code);
		for (int i = 0; i < 4; i++)
			abs_setup(pad, axes[i], -32768, 32767, 128);
		abs_setup(pad, ABS_Z, 0, 255, 0);
		abs_setup(pad, ABS_RZ, 0, 255, 0);
		abs_setup(pad, ABS_HAT0X, -1, 1, 0);
		abs_setup(pad, ABS_HAT0Y, -1, 1, 0);
		/* (the Xbox 360 pad's IDs: SDL's mapping for it applies) */
		uinput_up(pad, "Microsoft X-Box 360 pad", BUS_USB, 0x045e, 0x028e, 0x0114);
	}
	if (clone < 0) {
		clone = uinput_new();
		ioctl(clone, UI_SET_EVBIT, EV_KEY);
		ioctl(clone, UI_SET_EVBIT, EV_ABS);
		ioctl(clone, UI_SET_KEYBIT, BTN_TOUCH);
		ioctl(clone, UI_SET_PROPBIT, INPUT_PROP_DIRECT);
		abs_setup(clone, ABS_X, 0, CLONE_MAX, 0);
		abs_setup(clone, ABS_Y, 0, CLONE_MAX, 0);
		abs_setup(clone, ABS_MT_SLOT, 0, NSLOTS - 1, 0);
		abs_setup(clone, ABS_MT_TRACKING_ID, 0, 65535, 0);
		abs_setup(clone, ABS_MT_POSITION_X, 0, CLONE_MAX, 0);
		abs_setup(clone, ABS_MT_POSITION_Y, 0, CLONE_MAX, 0);
		uinput_up(clone, "screenpad touch", BUS_VIRTUAL, 0x1209, 0x5351, 1);
	}
}

/* an action on or off; for a stick, its position */
static int hat[4];			/* the gamepad's d-pad, from any control */

static void act(const struct action *a, int on)
{
	switch (a->kind) {
	case A_KEY:
		emit(kbd, EV_KEY, a->code, on);
		emit(kbd, EV_SYN, SYN_REPORT, 0);
		break;
	case A_BTN:
		emit(pad, EV_KEY, a->code, on);
		emit(pad, EV_SYN, SYN_REPORT, 0);
		break;
	case A_TRIGGER:
		emit(pad, EV_ABS, a->code, on ? 255 : 0);
		emit(pad, EV_SYN, SYN_REPORT, 0);
		break;
	case A_HAT:
		hat[a->code] = on;
		emit(pad, EV_ABS, ABS_HAT0X, hat[DIR_RIGHT] - hat[DIR_LEFT]);
		emit(pad, EV_ABS, ABS_HAT0Y, hat[DIR_DOWN] - hat[DIR_UP]);
		emit(pad, EV_SYN, SYN_REPORT, 0);
		break;
	}
}

static void stick_axes(const struct action *a, float sx, float sy)
{
	int x = (int)lrintf(sx * 32767), y = (int)lrintf(sy * 32767);

	emit(pad, EV_ABS, a->kind == A_LSTICK ? ABS_X : ABS_RX, x);
	emit(pad, EV_ABS, a->kind == A_LSTICK ? ABS_Y : ABS_RY, y);
	emit(pad, EV_SYN, SYN_REPORT, 0);
}

/* every control let go (hidden, a new preset, the end) */
static void release_all(void)
{
	for (int i = 0; i < P.n; i++) {
		struct control *c = &P.c[i];

		if (c->type == C_BUTTON && c->on)
			act(&c->act, 0);
		for (int d = 0; d < 4; d++)
			if (c->type != C_BUTTON && (c->on & 1 << d))
				act(&c->dir[d], 0);
		if (c->type == C_STICK && (c->act.kind == A_LSTICK || c->act.kind == A_RSTICK) &&
		    (c->sx || c->sy))
			stick_axes(&c->act, 0, 0);
		c->on = 0;
		c->sx = c->sy = 0;
	}
}

/* ---- the touchscreen ------------------------------------------------------------- */

static int ts = -1, grabbed;
static struct input_absinfo tsx, tsy;
static struct finger {
	int id, x, y;		/* now (id -1: up) */
	int down;		/* as the last frame left it */
	int mine;		/* the controller's: ctl is its control */
	int ctl;
	int moved;
	float lx, ly;		/* in the turned screen, pixels */
} F[NSLOTS];
static int cur_slot, clone_touch;

static int is_touch(int fd)
{
	unsigned long abs[ABS_CNT / (8 * sizeof(long)) + 1] = { 0 };
	char name[64] = "";

	ioctl(fd, EVIOCGNAME(sizeof(name)), name);
	ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(abs)), abs);
	return strncmp(name, "screenpad", 9) &&
	       (abs[ABS_MT_POSITION_X / (8 * sizeof(long))] >> (ABS_MT_POSITION_X % (8 * sizeof(long))) & 1);
}

static void ts_open(void)
{
	const char *want = getenv("SCREENPAD_TOUCH");	/* (tests: a uinput one) */
	char path[64];

	if (want && (ts = open(want, O_RDONLY | O_NONBLOCK | O_CLOEXEC)) < 0)
		die("%s: %s", want, strerror(errno));
	for (int i = 0; i < 32 && ts < 0; i++) {
		snprintf(path, sizeof(path), "/dev/input/event%d", i);
		ts = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
		if (ts >= 0 && !is_touch(ts)) {
			close(ts);
			ts = -1;
		}
	}
	if (ts < 0)
		die("no touchscreen");
	ioctl(ts, EVIOCGABS(ABS_MT_POSITION_X), &tsx);
	ioctl(ts, EVIOCGABS(ABS_MT_POSITION_Y), &tsy);
	if (tsx.maximum <= tsx.minimum)
		tsx.maximum = tsx.minimum + 4096;
	if (tsy.maximum <= tsy.minimum)
		tsy.maximum = tsy.minimum + 4096;
	for (int s = 0; s < NSLOTS; s++)
		F[s].id = -1;
}

/* the panel's coordinates -> the turned screen's (rotate.sh's matrices) */
static void to_screen(struct finger *f)
{
	float u = (float)(f->x - tsx.minimum) / (tsx.maximum - tsx.minimum);
	float v = (float)(f->y - tsy.minimum) / (tsy.maximum - tsy.minimum), X, Y;

	switch (scr_rot) {
	case DRM_MODE_ROTATE_90:	X = 1 - v; Y = u; break;
	case DRM_MODE_ROTATE_180:	X = 1 - u; Y = 1 - v; break;
	case DRM_MODE_ROTATE_270:	X = v; Y = 1 - u; break;
	default:			X = u; Y = v; break;
	}
	f->lx = X * scr_w;
	f->ly = Y * scr_h;
}

static int hit(float x, float y)
{
	for (int i = P.n - 1; i >= 0; i--) {	/* the last drawn is on top */
		const struct control *c = &P.c[i];

		if (c->type == C_BUTTON) {
			if (c->circle) {
				float rx = c->w / 2, ry = c->h / 2;
				float dx = (x - c->x - rx) / rx, dy = (y - c->y - ry) / ry;

				if (dx * dx + dy * dy <= 1.1f)
					return i;
			} else if (x >= c->x && x < c->x + c->w && y >= c->y && y < c->y + c->h) {
				return i;
			}
		} else {
			float s = fminf(c->w, c->h), dx = x - c->x - c->w / 2, dy = y - c->y - c->h / 2;

			if (dx * dx + dy * dy <= s * s / 4 * 1.15f)
				return i;
		}
	}
	return -1;
}

static void clone_frame(void)
{
	int any = 0, first = -1, changed = 0;

	for (int s = 0; s < NSLOTS; s++) {
		struct finger *f = &F[s];
		int px = (int)lrintf(f->lx / (scr_w ? scr_w : 1) * CLONE_MAX);
		int py = (int)lrintf(f->ly / (scr_h ? scr_h : 1) * CLONE_MAX);

		if (f->mine)
			continue;
		px = px < 0 ? 0 : px > CLONE_MAX ? CLONE_MAX : px;
		py = py < 0 ? 0 : py > CLONE_MAX ? CLONE_MAX : py;
		if (f->id >= 0 && !f->down) {
			emit(clone, EV_ABS, ABS_MT_SLOT, s);
			emit(clone, EV_ABS, ABS_MT_TRACKING_ID, f->id & 0xffff);
			emit(clone, EV_ABS, ABS_MT_POSITION_X, px);
			emit(clone, EV_ABS, ABS_MT_POSITION_Y, py);
			changed = 1;
		} else if (f->id < 0 && f->down) {
			emit(clone, EV_ABS, ABS_MT_SLOT, s);
			emit(clone, EV_ABS, ABS_MT_TRACKING_ID, -1);
			changed = 1;
		} else if (f->id >= 0 && f->moved) {
			emit(clone, EV_ABS, ABS_MT_SLOT, s);
			emit(clone, EV_ABS, ABS_MT_POSITION_X, px);
			emit(clone, EV_ABS, ABS_MT_POSITION_Y, py);
			changed = 1;
		}
		if (f->id >= 0) {
			any = 1;
			if (first < 0) {
				first = s;
				if (changed) {
					emit(clone, EV_ABS, ABS_X, px);
					emit(clone, EV_ABS, ABS_Y, py);
				}
			}
		}
	}
	if (any != clone_touch) {
		emit(clone, EV_KEY, BTN_TOUCH, any);
		clone_touch = any;
		changed = 1;
	}
	if (changed)
		emit(clone, EV_SYN, SYN_REPORT, 0);
}

/* the controls from the controller's fingers: what changed is acted on */
static int controls_frame(void)
{
	int on[MAX_CONTROLS] = { 0 }, redraw_ = 0;
	float sx[MAX_CONTROLS] = { 0 }, sy[MAX_CONTROLS] = { 0 };

	for (int s = 0; s < NSLOTS; s++) {
		struct finger *f = &F[s];
		const struct control *a;

		if (!f->mine || f->id < 0)
			continue;
		a = &P.c[f->ctl];
		if (a->type == C_BUTTON) {	/* whichever button it is over now */
			int i = hit(f->lx, f->ly);

			if (i >= 0 && P.c[i].type == C_BUTTON)
				on[i] = 1;
		} else {
			float s2 = fminf(a->w, a->h) / 2;
			float dx = (f->lx - a->x - a->w / 2) / s2, dy = (f->ly - a->y - a->h / 2) / s2;

			if (a->type == C_STICK) {
				float l = sqrtf(dx * dx + dy * dy), m = 0.79f;	/* the knob's reach */

				dx /= m;
				dy /= m;
				l /= m;
				if (l > 1) {
					dx /= l;
					dy /= l;
				}
				sx[f->ctl] = dx;
				sy[f->ctl] = dy;
				on[f->ctl] |= 16;	/* held */
			}
			/* four ways and the diagonals between them, a dead middle */
			if (sqrtf(dx * dx + dy * dy) > 0.25f) {
				if (fabsf(dx) > 0.414f * fabsf(dy))
					on[f->ctl] |= 1 << (dx < 0 ? DIR_LEFT : DIR_RIGHT);
				if (fabsf(dy) > 0.414f * fabsf(dx))
					on[f->ctl] |= 1 << (dy < 0 ? DIR_UP : DIR_DOWN);
			}
		}
	}
	for (int i = 0; i < P.n; i++) {
		struct control *c = &P.c[i];
		int analogue = c->type == C_STICK && (c->act.kind == A_LSTICK || c->act.kind == A_RSTICK);

		if (c->type == C_BUTTON) {
			if (on[i] != c->on) {
				act(&c->act, on[i]);
				c->on = on[i];
				dirty[i] = redraw_ = 1;
			}
			continue;
		}
		if (analogue) {
			if (sx[i] != c->sx || sy[i] != c->sy)
				stick_axes(&c->act, sx[i], sy[i]);
			on[i] &= 16;
		}
		if (c->type == C_STICK && (sx[i] != c->sx || sy[i] != c->sy))
			dirty[i] = redraw_ = 1;
		c->sx = sx[i];
		c->sy = sy[i];
		for (int d = 0; d < 4; d++)
			if ((on[i] ^ c->on) & 1 << d)
				act(&c->dir[d], on[i] >> d & 1);
		if (on[i] != c->on)
			dirty[i] = redraw_ = 1;
		c->on = on[i];
	}
	return redraw_;
}

static double last_draw;
static int draw_pending;

static void touch_frame(void)
{
	for (int s = 0; s < NSLOTS; s++) {
		struct finger *f = &F[s];

		if (f->id >= 0)
			to_screen(f);
		if (f->id >= 0 && !f->down) {
			int c = shown ? hit(f->lx, f->ly) : -1;

			f->mine = c >= 0;
			f->ctl = c;
		}
	}
	if (shown)
		clone_frame();
	if (shown && controls_frame())
		draw_pending = 1;
	for (int s = 0; s < NSLOTS; s++) {
		struct finger *f = &F[s];

		if (f->id < 0)
			f->mine = 0;
		f->down = f->id >= 0;
		f->moved = 0;
	}
}

static void touch_read(void)
{
	struct input_event ev[64];
	ssize_t n;

	while ((n = read(ts, ev, sizeof(ev))) > 0)
		for (int i = 0; i < n / (ssize_t)sizeof(*ev); i++) {
			const struct input_event *e = &ev[i];
			struct finger *f = &F[cur_slot];

			if (e->type == EV_SYN && e->code == SYN_REPORT) {
				touch_frame();
			} else if (e->type == EV_ABS) {
				switch (e->code) {
				case ABS_MT_SLOT:
					cur_slot = e->value >= 0 && e->value < NSLOTS ? e->value : 0;
					break;
				case ABS_MT_TRACKING_ID:
					f->id = e->value;
					break;
				case ABS_MT_POSITION_X:
					f->x = e->value;
					f->moved = 1;
					break;
				case ABS_MT_POSITION_Y:
					f->y = e->value;
					f->moved = 1;
					break;
				}
			}
		}
}

/* show or hide, as the screen is now */
static void show(int on)
{
	if (on == shown)
		return;
	if (!on) {
		release_all();
		/* fingers on X through the clone: let go there */
		for (int s = 0; s < NSLOTS; s++)
			if (!F[s].mine && F[s].down)
				F[s].id = -1;
		clone_frame();
		for (int s = 0; s < NSLOTS; s++)
			F[s].down = F[s].mine = 0;
		ov_set(NULL, -1, 0);
		if (grabbed && !ioctl(ts, EVIOCGRAB, 0))
			grabbed = 0;
		shown = 0;
		return;
	}
	if (!grabbed && !ioctl(ts, EVIOCGRAB, 1))
		grabbed = 1;
	/* the fingers down now are the system's until they lift */
	for (int s = 0; s < NSLOTS; s++)
		F[s].down = F[s].mine = 0, F[s].id = -1;
	shown = 1;
	full_redraw = 1;
	redraw();
}

static void screen_check(void)
{
	int w = scr_w, h = scr_h;
	uint32_t rot = scr_rot;

	ov_set(NULL, 0, 1);
	if (w != scr_w || h != scr_h || rot != scr_rot) {
		show(0);			/* (and again for the new size, below) */
		fprintf(stderr, "screenpad: the screen: %dx%d, turned %s\n", scr_w, scr_h,
			scr_rot == DRM_MODE_ROTATE_90 ? "left" : scr_rot == DRM_MODE_ROTATE_180 ?
			"upside down" : scr_rot == DRM_MODE_ROTATE_270 ? "right" : "not");
	}
	show(scr_w > 0 && (scr_w > scr_h) == P.landscape);
}

/* ---- the whole --------------------------------------------------------------- */

static volatile sig_atomic_t quit;
static pid_t child = -1;

static void on_signal(int sig)
{
	quit = sig;
}

static void usage(void)
{
	fputs("usage: screenpad PRESET [-- COMMAND [ARG...]]\n"
	      "       screenpad -l   the presets\n", stderr);
	exit(2);
}

int main(int argc, char **argv)
{
	const char *name = NULL, *err;
	char **cmd = NULL, *text;
	struct sigaction sa = { .sa_handler = on_signal };

	paths();
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--")) {
			cmd = argv + i + 1;
			break;
		} else if (!strcmp(argv[i], "-l")) {
			list_presets();
			return 0;
		} else if (argv[i][0] == '-' || name) {
			usage();
		} else {
			name = argv[i];
		}
	}
	if (!name)
		usage();
	if (!(text = preset_text(name)))
		die("no preset \"%s\" (screenpad -l lists them)", name);
	if (preset_load(text, &err))
		die("%s: %s", name, err);
	free(text);

	have_font = 0;
	{
		char *f = read_file(FONT, 4 << 20);

		if (f && stbtt_InitFont(&font, (unsigned char *)f, 0))
			have_font = 1;
		else
			fprintf(stderr, "screenpad: no %s: no labels\n", FONT);
	}
	ov_open();
	ts_open();
	uinput_open();
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGHUP, &sa, NULL);
	signal(SIGPIPE, SIG_IGN);
	fprintf(stderr, "screenpad: preset \"%s\", %d controls, %s\n", P.name, P.n,
		P.landscape ? "landscape" : "portrait");
	screen_check();

	if (cmd && *cmd) {
		child = fork();
		if (child == 0) {
			signal(SIGPIPE, SIG_DFL);
			execvp(cmd[0], cmd);
			fprintf(stderr, "screenpad: %s: %s\n", cmd[0], strerror(errno));
			_exit(127);
		}
	}

	double next_check = now() + 0.25;
	int status = 0;

	while (!quit) {
		struct pollfd p = { .fd = ts, .events = POLLIN };
		int timeout;
		double t = now();

		timeout = (int)((next_check - t) * 1000) + 1;
		if (draw_pending)
			timeout = (int)((last_draw + 1.0 / 60 - t) * 1000) + 1;
		if (timeout < 0)
			timeout = 0;
		if (poll(&p, 1, timeout) < 0 && errno != EINTR)
			break;
		if (p.revents & POLLIN)
			touch_read();
		t = now();
		if (draw_pending && t - last_draw >= 1.0 / 60) {
			draw_pending = 0;
			last_draw = t;
			redraw();
		}
		if (t >= next_check) {
			next_check = t + 0.25;
			screen_check();
		}
		if (child > 0 && waitpid(child, &status, WNOHANG) == child) {
			child = -1;
			break;
		}
	}
	release_all();
	show(0);
	if (child > 0 && quit)
		kill(child, quit);
	if (kbd >= 0)
		ioctl(kbd, UI_DEV_DESTROY);
	if (pad >= 0)
		ioctl(pad, UI_DEV_DESTROY);
	if (clone >= 0)
		ioctl(clone, UI_DEV_DESTROY);
	return WIFEXITED(status) ? WEXITSTATUS(status) : 0;
}
