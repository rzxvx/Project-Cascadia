/*
 * task-killer -- a way out of a program that will not let go of the screen,
 * from the glass alone. The iPad 7 (A10) port of devices/a5's task-killer.
 *
 *   task-killer            the daemon (a10-init starts it at boot)
 *   task-killer --show     open the window now
 *   task-killer --list     what Continue would kill, and stop
 *
 * Press Home three times, each press within 0.8 s of the one before (the
 * gpio-keys Home key, watched here; SIGUSR1 opens it too), and a window asks:
 *
 *   Continue      every process but the basic ones killed: the text console
 *                 (the glass shell and fbkeyboard), ssh, udev, touch, the
 *                 buttons, the USB link stay.  After a 5 s countdown,
 *                 unless "Don't use the safety countdown next time" was
 *                 ticked (kept in /var/lib/task-killer/no-countdown)
 *   Manual kill   a list of the other processes, ticked one by one, Kill
 *   Cancel
 *
 * Drawn onto /dev/fb0. The A10's screen (simpledrm) has no overlay plane, as
 * the A5's apple-sgx has: when a graphics session holds the screen (labwc),
 * the window first switches to a free VT -- seatd then stops the session
 * and the framebuffer is ours -- and Cancel switches back. A session too
 * hung to let go is drawn over all the same (nothing draws then). The
 * touchscreen is opened and grabbed only while the window is open, so that
 * what is under the window gets none of its touches.
 *
 * tools/task-killer/build.sh builds it (static, aarch64); the rootfs carries
 * it as /usr/local/bin/task-killer.  Root.
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <linux/kd.h>
#include <linux/vt.h>
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
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>


#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#include "stb_truetype.h"
#pragma GCC diagnostic pop

#ifndef DRM_MODE_ROTATE_0
#define DRM_MODE_ROTATE_0	(1 << 0)
#define DRM_MODE_ROTATE_90	(1 << 1)
#define DRM_MODE_ROTATE_180	(1 << 2)
#define DRM_MODE_ROTATE_270	(1 << 3)
#endif

#define FONT		"/usr/share/fonts/dejavu/DejaVuSans.ttf"
#define FONT_BOLD	"/usr/share/fonts/dejavu/DejaVuSans-Bold.ttf"
#define SKIP_FILE	"/var/lib/task-killer/no-countdown"
#define OV_PITCH	4096		/* the overlay: 1024 pixels a line */
#define OV_LINES	1024
#define NSLOTS		10
#define MAXPROC		512
#define COUNTDOWN	5.0

static void logf_(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	fputs("task-killer: ", stderr);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
}

static void die(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	fputs("task-killer: ", stderr);
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

/* ---- the screen: the framebuffer, a free VT when a session holds it ------- */

enum { OUT_NONE, OUT_FB };
static int out = OUT_NONE;

static int pw, ph;			/* fb0 */
static int rot;				/* the picture's turn: 0, 90 (left), 180, 270 (right) */
static int canvas_turned;		/* (always 0 here: no plane that turns) */

static int fb = -1;
static uint8_t *fb_map, *fb_saved;
static int fb_line, fb_size;

static int vt_prev;			/* the session's VT we switched away from, or 0 */
static int vt_mine_fd = -1;		/* the free VT, held in graphics mode */

/* VT_ACTIVATE, then a bounded wait: a session too hung to let go must not
 * hang this too */
static int vt_switch(int fd, int to)
{
	struct vt_stat vs;

	if (ioctl(fd, VT_ACTIVATE, to))
		return -1;
	for (int i = 0; i < 40; i++) {
		if (!ioctl(fd, VT_GETSTATE, &vs) && vs.v_active == to)
			return 0;
		usleep(50000);
	}
	return -1;
}

static void vt_away(void)
{
	int fd = open("/dev/tty0", O_RDWR | O_NOCTTY | O_CLOEXEC), t, mode = KD_TEXT, free_vt = 0;
	struct vt_stat vs;
	char path[32];

	if (fd < 0)
		return;
	if (ioctl(fd, VT_GETSTATE, &vs))
		goto out;
	snprintf(path, sizeof(path), "/dev/tty%d", vs.v_active);
	t = open(path, O_RDWR | O_NOCTTY | O_CLOEXEC);
	if (t >= 0) {
		ioctl(t, KDGETMODE, &mode);
		close(t);
	}
	if (mode != KD_GRAPHICS)
		goto out;			/* the text console: draw over it */
	if (ioctl(fd, VT_OPENQRY, &free_vt) || free_vt <= 0)
		goto out;
	if (vt_switch(fd, free_vt))
		logf_("VT %d -> %d: the session did not let go; drawing over it", vs.v_active, free_vt);
	else
		logf_("VT %d -> %d: the session set aside", vs.v_active, free_vt);
	vt_prev = vs.v_active;
	snprintf(path, sizeof(path), "/dev/tty%d", free_vt);
	vt_mine_fd = open(path, O_RDWR | O_NOCTTY | O_CLOEXEC);
	if (vt_mine_fd >= 0)
		ioctl(vt_mine_fd, KDSETMODE, KD_GRAPHICS);	/* fbcon keeps off fb0 */
out:
	close(fd);
}

static void vt_back(void)
{
	int fd;

	if (vt_mine_fd >= 0) {
		ioctl(vt_mine_fd, KDSETMODE, KD_TEXT);
		close(vt_mine_fd);
		vt_mine_fd = -1;
	}
	if (!vt_prev)
		return;
	fd = open("/dev/tty0", O_RDWR | O_NOCTTY | O_CLOEXEC);
	if (fd >= 0) {
		if (vt_switch(fd, vt_prev))
			logf_("back to VT %d: not (yet) active", vt_prev);
		close(fd);
	}
	vt_prev = 0;
}

static void fb_close(void)
{
	if (fb_map && fb_saved)
		memcpy(fb_map, fb_saved, fb_size);
	if (fb_map)
		munmap(fb_map, fb_size);
	free(fb_saved);
	fb_map = fb_saved = NULL;
	if (fb >= 0)
		close(fb);
	fb = -1;
}

static int fb_open(void)
{
	struct fb_var_screeninfo v;
	struct fb_fix_screeninfo f;

	fb = open("/dev/fb0", O_RDWR | O_CLOEXEC);
	if (fb < 0 || ioctl(fb, FBIOGET_VSCREENINFO, &v) || ioctl(fb, FBIOGET_FSCREENINFO, &f) ||
	    v.bits_per_pixel != 32) {
		logf_("/dev/fb0: not usable");
		if (fb >= 0)
			close(fb);
		fb = -1;
		return -1;
	}
	pw = v.xres;
	ph = v.yres;
	fb_line = f.line_length;
	fb_size = fb_line * ph;
	fb_map = mmap(NULL, fb_size, PROT_READ | PROT_WRITE, MAP_SHARED, fb, 0);
	if (fb_map == MAP_FAILED) {
		fb_map = NULL;
		fb_close();
		return -1;
	}
	fb_saved = malloc(fb_size);
	memcpy(fb_saved, fb_map, fb_size);
	return 0;
}

/* where the window goes now: fb0, after a VT switch away from a graphics
 * session */
static void screen_pick(void)
{
	vt_away();
	if (!fb_open()) {
		rot = 0;
		canvas_turned = 0;
		out = OUT_FB;
		return;
	}
	vt_back();
	out = OUT_NONE;
}

/* ---- drawing: a canvas in the picture's own coordinates ------------------- */

static uint32_t *cv;
static int lw, lh;			/* the canvas: the screen as the user sees it */
static int clip_x0, clip_y0, clip_x1, clip_y1;
static stbtt_fontinfo font_r, font_b;
static int have_r, have_b;

static int font_load(stbtt_fontinfo *fi, const char *path)
{
	FILE *f = fopen(path, "rb");
	long n;
	unsigned char *d;

	if (!f)
		return 0;
	fseek(f, 0, SEEK_END);
	n = ftell(f);
	fseek(f, 0, SEEK_SET);
	d = malloc(n);
	if (fread(d, 1, n, f) != (size_t)n) {
		fclose(f);
		free(d);
		return 0;
	}
	fclose(f);
	return stbtt_InitFont(fi, d, stbtt_GetFontOffsetForIndex(d, 0));
}

static void clip_all(void)
{
	clip_x0 = clip_y0 = 0;
	clip_x1 = lw;
	clip_y1 = lh;
}

static inline void put(int x, int y, uint32_t rgb, float a)
{
	uint32_t *p, d;
	float da, oa, r, g, b;

	if (a <= 0.004f || x < clip_x0 || y < clip_y0 || x >= clip_x1 || y >= clip_y1)
		return;
	if (a > 1)
		a = 1;
	p = &cv[y * lw + x];
	d = *p;
	da = (d >> 24) / 255.f;
	oa = a + da * (1 - a);
	if (oa <= 0)
		return;
	r = ((rgb >> 16 & 255) * a + (d >> 16 & 255) * da * (1 - a)) / oa;
	g = ((rgb >> 8 & 255) * a + (d >> 8 & 255) * da * (1 - a)) / oa;
	b = ((rgb & 255) * a + (d & 255) * da * (1 - a)) / oa;
	*p = (uint32_t)(oa * 255 + 0.5f) << 24 | (uint32_t)(r + 0.5f) << 16 |
	     (uint32_t)(g + 0.5f) << 8 | (uint32_t)(b + 0.5f);
}

/* opaque fill, fast: no blending */
static void fill(int x0, int y0, int x1, int y1, uint32_t argb)
{
	if (x0 < clip_x0) x0 = clip_x0;
	if (y0 < clip_y0) y0 = clip_y0;
	if (x1 > clip_x1) x1 = clip_x1;
	if (y1 > clip_y1) y1 = clip_y1;
	for (int y = y0; y < y1; y++)
		for (int x = x0; x < x1; x++)
			cv[y * lw + x] = argb;
}

static float cover(float sdf)
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

/* a rounded rectangle, filled with an opaque colour, bordered; the inside
 * filled fast, only the edges by their distance */
static void rrect(int x0, int y0, int x1, int y1, int r, uint32_t rgb, uint32_t brgb, float bw)
{
	int e = r + 2;

	fill(x0 + e, y0 + e, x1 - e, y1 - e, 0xff000000 | rgb);
	for (int y = y0 - 1; y <= y1; y++)
		for (int x = x0 - 1; x <= x1; x++) {
			float d;

			if (x >= x0 + e && x < x1 - e && y >= y0 + e && y < y1 - e)
				continue;
			d = sdf_rrect(x + 0.5f, y + 0.5f, x0, y0, x1, y1, r);
			put(x, y, rgb, cover(d));
			if (bw > 0)
				put(x, y, brgb, cover(fabsf(d + bw / 2) - bw / 2));
		}
}

/* a thick line with round ends (the tick) */
static void seg(float ax, float ay, float bx, float by, float t, uint32_t rgb)
{
	int x0 = floorf(fminf(ax, bx) - t), x1 = ceilf(fmaxf(ax, bx) + t);
	int y0 = floorf(fminf(ay, by) - t), y1 = ceilf(fmaxf(ay, by) + t);
	float dx = bx - ax, dy = by - ay, l2 = dx * dx + dy * dy;

	for (int y = y0; y <= y1; y++)
		for (int x = x0; x <= x1; x++) {
			float px = x + 0.5f - ax, py = y + 0.5f - ay;
			float h = fminf(fmaxf((px * dx + py * dy) / l2, 0), 1);
			float ex = px - dx * h, ey = py - dy * h;

			put(x, y, rgb, cover(sqrtf(ex * ex + ey * ey) - t / 2));
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

static stbtt_fontinfo *fnt(int bold)
{
	return bold && have_b ? &font_b : &font_r;
}

/* the width of n bytes of s */
static float text_w(const char *s, int n, float px, int bold)
{
	stbtt_fontinfo *f = fnt(bold);
	float scale = stbtt_ScaleForPixelHeight(f, px), w = 0;
	const char *p = s, *end = s + n;

	while (p < end && *p) {
		int adv, lsb;

		stbtt_GetCodepointHMetrics(f, utf8_next(&p), &adv, &lsb);
		w += adv * scale;
	}
	return w;
}

/* text from x on the baseline y; n bytes of s (n < 0: all) */
static void text_at(const char *s, int n, float x, float y, float px, int bold, uint32_t rgb)
{
	stbtt_fontinfo *f = fnt(bold);
	float scale;
	const char *p = s, *end;

	if (!have_r)
		return;
	if (n < 0)
		n = strlen(s);
	end = s + n;
	scale = stbtt_ScaleForPixelHeight(f, px);
	while (p < end && *p) {
		uint32_t c = utf8_next(&p);
		int adv, lsb, w, h, xo, yo;
		unsigned char *bm = c == ' ' ? NULL :
			stbtt_GetCodepointBitmap(f, scale, scale, c, &w, &h, &xo, &yo);

		for (int j = 0; bm && j < h; j++)
			for (int i = 0; i < w; i++)
				if (bm[j * w + i])
					put((int)(x + 0.5f) + xo + i, (int)y + yo + j, rgb, bm[j * w + i] / 255.f);
		if (bm)
			stbtt_FreeBitmap(bm, NULL);
		stbtt_GetCodepointHMetrics(f, c, &adv, &lsb);
		x += adv * scale;
	}
}

/* the baseline for text px high centred in a box from y0 to y1 */
static float base_in(float y0, float y1, float px, int bold)
{
	stbtt_fontinfo *f = fnt(bold);
	float scale = stbtt_ScaleForPixelHeight(f, px);
	int asc, desc, gap;

	stbtt_GetFontVMetrics(f, &asc, &desc, &gap);
	return (y0 + y1) / 2 + (asc + desc) * scale / 2;
}

static void text_c(const char *s, float x0, float x1, float y0, float y1, float px, int bold,
		   uint32_t rgb)
{
	float w = text_w(s, strlen(s), px, bold);

	text_at(s, -1, (x0 + x1 - w) / 2, base_in(y0, y1, px, bold), px, bold, rgb);
}

/* s wrapped to maxw, lines lh apart, its first baseline at y; how many
 * lines (drawn only with draw) */
static int text_wrap(const char *s, float x, float y, float maxw, float px, float lineh,
		     int bold, uint32_t rgb, int draw)
{
	int lines = 0;

	while (*s) {
		const char *best = NULL, *p = s;

		while (*s == ' ')
			s++;
		p = s;
		for (;;) {
			const char *q = p;

			while (*q && *q != ' ')
				q++;
			if (text_w(s, q - s, px, bold) > maxw && best)
				break;
			best = q;
			if (!*q)
				break;
			p = q + 1;
		}
		if (draw)
			text_at(s, best - s, x, y + lines * lineh, px, bold, rgb);
		lines++;
		s = best;
	}
	return lines;
}

static void canvas_fit(void);
static void layout(int draw);

/* the canvas onto the screen */
static void present(void)
{
	if (out == OUT_FB) {
		for (int y = 0; y < ph; y++) {
			uint32_t *dst = (uint32_t *)(fb_map + (size_t)y * fb_line);
			uint32_t *src = (uint32_t *)(fb_saved + (size_t)y * fb_line);

			for (int x = 0; x < pw; x++) {
				uint32_t c = cv[y * lw + x], s = src[x];
				uint32_t a = c >> 24, r, g, b;

				r = ((c >> 16 & 255) * a + (s >> 16 & 255) * (255 - a)) / 255;
				g = ((c >> 8 & 255) * a + (s >> 8 & 255) * (255 - a)) / 255;
				b = ((c & 255) * a + (s & 255) * (255 - a)) / 255;
				dst[x] = 0xff000000 | r << 16 | g << 8 | b;
			}
		}
	}
}

static void screen_release(void)
{
	if (out == OUT_FB)
		fb_close();
	vt_back();
	out = OUT_NONE;
}

/* ---- processes ----------------------------------------------------------- */

struct proc {
	pid_t pid;
	char name[48];
	long rss_kb;
	int sel;
};
static struct proc P[MAXPROC];
static int np;

struct pinfo {
	pid_t ppid, sid;
	int tty, kthread;
	char state;
	char comm[32];
	char cmd[256];		/* the command line, arguments split by spaces */
	char exe[128];
};

static int pinfo(pid_t pid, struct pinfo *pi)
{
	char path[64], buf[1024], *p;
	unsigned long flags = 0;
	int fd, n;

	memset(pi, 0, sizeof(*pi));
	snprintf(path, sizeof(path), "/proc/%d/stat", pid);
	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -1;
	n = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (n <= 0)
		return -1;
	buf[n] = 0;
	p = strchr(buf, '(');
	if (p) {
		char *q = strrchr(buf, ')');

		if (q && q > p) {
			snprintf(pi->comm, sizeof(pi->comm), "%.*s", (int)(q - p - 1), p + 1);
			p = q + 2;
		}
	}
	if (!p || sscanf(p, "%c %d %*d %d %d %*d %lu", &pi->state, &pi->ppid, &pi->sid,
			 &pi->tty, &flags) != 5)
		return -1;
	pi->kthread = (flags & 0x00200000) != 0;	/* PF_KTHREAD */
	snprintf(path, sizeof(path), "/proc/%d/cmdline", pid);
	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd >= 0) {
		n = read(fd, pi->cmd, sizeof(pi->cmd) - 1);
		close(fd);
		for (int i = 0; i < n; i++)
			if (!pi->cmd[i])
				pi->cmd[i] = ' ';
		if (n > 0)
			pi->cmd[n] = 0;
	}
	snprintf(path, sizeof(path), "/proc/%d/exe", pid);
	n = readlink(path, pi->exe, sizeof(pi->exe) - 1);
	if (n > 0)
		pi->exe[n] = 0;
	return 0;
}

/* the basic minimum, which nothing here kills: the system's own, not what
 * was started in it */
static int is_base(pid_t pid, const struct pinfo *pi, int depth)
{
	static const char *const keep[] = {
		"a10-init", "task-killer", "dropbear", "udevd", "fbkeyboard",
		"getty", "wpa_supplicant", "udhcpc", "dhcpcd", "iwd",
		"bluetoothd", "seatd", "syslogd", "klogd", "ntpd", "crond", "acpid", NULL
	};
	int major = pi->tty >> 8 & 0xfff;

	if (pid == 1 || pid == getpid() || pi->kthread || !pi->cmd[0])
		return 1;
	/* the initramfs's helpers in RAM: the USB link's watchdog and DHCP server */
	if (!strncmp(pi->cmd, "/run/cascadia/", 14) || strstr(pi->cmd, "/run/cascadia/usbwatch") ||
	    strstr(pi->cmd, "udhcpd"))
		return 1;
	for (int i = 0; keep[i]; i++)
		if (!strcmp(pi->comm, keep[i]))
			return 1;
	if (!strcmp(pi->comm, "dbus-daemon"))
		return strstr(pi->cmd, "--system") != NULL;
	/* stage 2's helpers: Wi-Fi, Bluetooth, the clock */
	if (strstr(pi->cmd, "/usr/bin/wifi") || strstr(pi->cmd, "/usr/bin/bluetooth") ||
	    strstr(pi->cmd, "ntpd "))
		return 1;
	/* the glass shell and the ACM one: a login shell leading its session
	 * on a console (not a pty: ssh's go), started by PID 1's getty */
	if ((!strcmp(pi->comm, "sh") || !strcmp(pi->comm, "ash") || !strcmp(pi->comm, "login")) &&
	    pi->tty && major != 136) {
		struct pinfo pp;

		if (pi->sid == pid && pi->ppid == 1)
			return 1;
		/* busybox login stays, the shell its child */
		if (!pinfo(pi->ppid, &pp) && !strcmp(pp.comm, "login") && pp.ppid == 1)
			return 1;
	}
	/* a sleep of something kept (stage 2's loops) */
	if (!strcmp(pi->comm, "sleep") && depth == 0) {
		struct pinfo pp;

		return !pinfo(pi->ppid, &pp) && is_base(pi->ppid, &pp, 1);
	}
	return 0;
}

static long rss_kb(pid_t pid)
{
	char path[64];
	long pages = 0, rss = 0;
	FILE *f;

	snprintf(path, sizeof(path), "/proc/%d/statm", pid);
	f = fopen(path, "r");
	if (!f)
		return 0;
	if (fscanf(f, "%ld %ld", &pages, &rss) != 2)
		rss = 0;
	fclose(f);
	return rss * (sysconf(_SC_PAGESIZE) / 1024);
}

static int by_rss(const void *a, const void *b)
{
	const struct proc *x = a, *y = b;

	return (y->rss_kb > x->rss_kb) - (y->rss_kb < x->rss_kb);
}

/* the processes that are not the basic minimum, the largest first */
static void scan(void)
{
	DIR *d = opendir("/proc");
	struct dirent *e;

	np = 0;
	if (!d)
		return;
	while ((e = readdir(d)) && np < MAXPROC) {
		struct pinfo pi;
		pid_t pid;

		if (!isdigit((unsigned char)e->d_name[0]))
			continue;
		pid = atoi(e->d_name);
		if (pinfo(pid, &pi) || pi.state == 'Z' || is_base(pid, &pi, 0))
			continue;
		P[np].pid = pid;
		P[np].sel = 0;
		P[np].rss_kb = rss_kb(pid);
		/* an interpreter's script by its name: "python3 screenpad-editor";
		 * a shell's inline script as "sh -c" */
		snprintf(P[np].name, sizeof(P[np].name), "%s", pi.comm);
		if (!strcmp(pi.comm, "sh") || !strcmp(pi.comm, "bash") || !strncmp(pi.comm, "python", 6)) {
			char *a = pi.cmd, *b;
			int inline_ = 0;

			a += strcspn(a, " ");			/* past argv[0] */
			for (;;) {
				a += strspn(a, " \t\n");
				if (*a != '-')
					break;
				if (!strncmp(a, "-c", 2))
					inline_ = 1;
				a += strcspn(a, " ");
			}
			if (inline_) {
				snprintf(P[np].name, sizeof(P[np].name), "%s -c", pi.comm);
			} else if (*a) {
				b = a + strcspn(a, " \t\n");
				*b = 0;
				if (strrchr(a, '/'))
					a = strrchr(a, '/') + 1;
				snprintf(P[np].name, sizeof(P[np].name), "%s %s", pi.comm, a);
			}
		}
		for (char *c = P[np].name; *c; c++)
			if ((unsigned char)*c < ' ')
				*c = ' ';
		np++;
	}
	closedir(d);
	qsort(P, np, sizeof(*P), by_rss);
}

static int alive(pid_t pid)
{
	struct pinfo pi;

	return !pinfo(pid, &pi) && pi.state != 'Z';
}

/* TERM to each, and KILL to what is left after 3 s */
static void kill_list(const pid_t *pids, int n)
{
	double t0 = now();
	int left;

	for (int i = 0; i < n; i++) {
		logf_("TERM %d", pids[i]);
		kill(pids[i], SIGTERM);
	}
	do {
		usleep(100000);
		left = 0;
		for (int i = 0; i < n; i++)
			left += alive(pids[i]);
	} while (left && now() - t0 < 3);
	for (int i = 0; i < n; i++)
		if (alive(pids[i])) {
			logf_("KILL %d", pids[i]);
			kill(pids[i], SIGKILL);
		}
}

/* the text console back on the glass: the VT out of graphics, drawn again,
 * and fbkeyboard as stage 2 started it */
static void console_back(void)
{
	int fd = open("/dev/tty0", O_RDWR | O_NOCTTY | O_CLOEXEC);

	if (fd >= 0) {
		struct vt_stat vs;
		struct vt_mode vm = { .mode = VT_AUTO };

		if (!ioctl(fd, VT_GETSTATE, &vs)) {
			int cur = vs.v_active;
			char path[32];
			int t;

			snprintf(path, sizeof(path), "/dev/tty%d", cur);
			t = open(path, O_RDWR | O_NOCTTY | O_CLOEXEC);
			if (t >= 0) {
				ioctl(t, KDSETMODE, KD_TEXT);
				ioctl(t, VT_SETMODE, &vm);
				ioctl(t, KDSKBMODE, K_UNICODE);
				close(t);
			}
			/* away and back: fbcon draws the whole console again */
			if (!ioctl(fd, VT_ACTIVATE, cur == 1 ? 2 : 1)) {
				ioctl(fd, VT_WAITACTIVE, cur == 1 ? 2 : 1);
				ioctl(fd, VT_ACTIVATE, cur);
				ioctl(fd, VT_WAITACTIVE, cur);
			}
		}
		close(fd);
	}
	if (system("pidof fbkeyboard >/dev/null") != 0) {
		pid_t c = fork();

		if (c == 0) {
			if (fork() == 0) {
				setsid();
				execl("/bin/sh", "sh", "-c",
				      "for n in /sys/class/input/event*; do "
				      "case \"$(cat $n/device/name 2>/dev/null)\" in 'iPad7,1'[12]' Touchscreen') "
				      "exec fbkeyboard -d /dev/input/${n##*/} -f " FONT " >/tmp/fbkeyboard.log 2>&1 </dev/null ;; "
				      "esac; done",
				      (char *)NULL);
				_exit(127);
			}
			_exit(0);
		}
		if (c > 0)
			waitpid(c, NULL, 0);
	}
}

static void kill_all(void)
{
	static pid_t pids[MAXPROC];
	int n;

	scan();
	for (n = 0; n < np; n++)
		pids[n] = P[n].pid;
	logf_("killing %d processes", n);
	kill_list(pids, n);
	/* what those left behind (a wrapper starting the next thing) */
	scan();
	for (n = 0; n < np; n++)
		pids[n] = P[n].pid;
	if (n)
		kill_list(pids, n);
	console_back();
}

/* ---- the touchscreen ----------------------------------------------------- */

static int ts = -1, grabbed;
static struct input_absinfo tsx, tsy;
static struct finger {
	int id, x, y;
	int down;		/* down as of the last frame */
	int last_id;		/* its tracking id then */
} F[NSLOTS];
static int debug;
static int cur_slot;

static int ts_open(void)
{
	char path[64], name[64];

	for (int i = 0; i < 32 && ts < 0; i++) {
		unsigned long abs[ABS_CNT / (8 * sizeof(long)) + 1] = { 0 };

		snprintf(path, sizeof(path), "/dev/input/event%d", i);
		ts = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
		if (ts < 0)
			continue;
		name[0] = 0;
		ioctl(ts, EVIOCGNAME(sizeof(name)), name);
		ioctl(ts, EVIOCGBIT(EV_ABS, sizeof(abs)), abs);
		if (strncmp(name, "screenpad", 9) && strncmp(name, "fbkeyboard", 10) &&
		    (abs[ABS_MT_POSITION_X / (8 * sizeof(long))] >> (ABS_MT_POSITION_X % (8 * sizeof(long))) & 1)) {
			logf_("touchscreen: %s (%s)", path, name);
			break;
		}
		close(ts);
		ts = -1;
	}
	if (ts < 0)
		return -1;
	ioctl(ts, EVIOCGABS(ABS_MT_POSITION_X), &tsx);
	ioctl(ts, EVIOCGABS(ABS_MT_POSITION_Y), &tsy);
	if (tsx.maximum <= tsx.minimum)
		tsx.maximum = tsx.minimum + 4096;
	if (tsy.maximum <= tsy.minimum)
		tsy.maximum = tsy.minimum + 4096;
	for (int s = 0; s < NSLOTS; s++)
		F[s].id = -1;
	return 0;
}

static void grab(int on)
{
	if (on && !grabbed && !ioctl(ts, EVIOCGRAB, 1))
		grabbed = 1;
	else if (!on && grabbed && !ioctl(ts, EVIOCGRAB, 0))
		grabbed = 0;
}

/* the glass's own coordinates, 0..1, x right and y down with Home below */
static void norm(const struct finger *f, float *u, float *v)
{
	*u = (float)(f->x - tsx.minimum) / (tsx.maximum - tsx.minimum);
	*v = (float)(f->y - tsy.minimum) / (tsy.maximum - tsy.minimum);
}

/* -> the canvas, the picture as the user sees it */
static void to_canvas(float u, float v, float *x, float *y)
{
	float X, Y;

	switch (rot) {
	case 90:  X = 1 - v; Y = u;     break;
	case 180: X = 1 - u; Y = 1 - v; break;
	case 270: X = v;     Y = 1 - u; break;
	default:  X = u;     Y = v;     break;
	}
	*x = X * lw;
	*y = Y * lh;
}

/* ---- the window ---------------------------------------------------------- */

enum { V_CLOSED, V_CONFIRM, V_MANUAL };
static int view = V_CLOSED;
static int skip_countdown;
static double deadline;
static int shown_count = -1;
static double scroll, scroll_max;

struct box { int x0, y0, x1, y1; };
enum { B_NONE, B_CANCEL, B_MANUAL, B_CONTINUE, B_SKIP, B_BACK, B_KILL, B_LIST, NB };
static struct box B[NB];
static int pressed = B_NONE;
static int row_h = 76;

#define C_BG	 0x2b3036
#define C_EDGE	 0x4a5058
#define C_TITLE	 0xffffff
#define C_TEXT	 0xc8ced4
#define C_BTN	 0x363c43
#define C_BTN_E	 0x5a616a
#define C_BTN_P	 0x4a525b
#define C_RED	 0xa32d2d
#define C_RED_E	 0xc04040
#define C_RED_P	 0xc03b3b
#define C_OFF	 0x30343a
#define C_OFF_T	 0x7c838b
#define C_LIST	 0x23272c
#define C_SEP	 0x353a40
#define C_ACCENT 0x378add

static const char *T_TITLE = "Kill all processes?";
static const char *T_BODY = "You are about to kill every process except the basic ones: the "
	"text console and fbkeyboard stay. To get back to the desktop afterwards, type "
	"desktop-labwc in the terminal.";
static const char *T_SURE = "Are you sure you want to do this?";
static const char *T_SKIP = "Don't use the safety countdown next time";

static int remaining(void)
{
	double r = deadline - now();

	return skip_countdown || r <= 0 ? 0 : (int)ceil(r);
}

static int nselected(void)
{
	int n = 0;

	for (int i = 0; i < np; i++)
		n += P[i].sel;
	return n;
}

static void button(int id, const char *label, int kind)	/* 0 plain, 1 red, 2 off */
{
	struct box b = B[id];
	uint32_t bg = kind == 1 ? (pressed == id ? C_RED_P : C_RED) : kind == 2 ? C_OFF :
		      pressed == id ? C_BTN_P : C_BTN;
	uint32_t edge = kind == 1 ? C_RED_E : kind == 2 ? 0x40454c : C_BTN_E;

	rrect(b.x0, b.y0, b.x1, b.y1, 12, bg, edge, 1.5f);
	text_c(label, b.x0 + 8, b.x1 - 8, b.y0, b.y1, 25, 0, kind == 2 ? C_OFF_T : 0xffffff);
}

static void checkbox(int x, int y, int sz, int on)
{
	if (on) {
		rrect(x, y, x + sz, y + sz, 7, C_ACCENT, C_ACCENT, 0);
		seg(x + sz * 0.24f, y + sz * 0.52f, x + sz * 0.43f, y + sz * 0.71f, sz * 0.12f, 0xffffff);
		seg(x + sz * 0.43f, y + sz * 0.71f, x + sz * 0.77f, y + sz * 0.31f, sz * 0.12f, 0xffffff);
	} else {
		rrect(x, y, x + sz, y + sz, 7, C_LIST, 0x9aa2aa, 2);
	}
}

/* the window laid out (and drawn, with draw) at the canvas's size */
static void layout(int draw)
{
	int dw = lw - 48 < 640 ? lw - 48 : 640, pad = 32, inner, dx, dy, dh, y;
	int land = lw > lh;
	float body = land ? 22 : 24, lineh = land ? 32 : 36, title = 32;

	memset(B, 0, sizeof(B));
	if (draw) {
		clip_all();
		fill(0, 0, lw, lh, 0x8c000000);	/* the screen under it, dimmed */
	}
	dx = (lw - dw) / 2;
	inner = dw - 2 * pad;
	if (view == V_CONFIRM) {
		int nb = text_wrap(T_BODY, 0, 0, inner, body, lineh, 0, 0, 0);
		int ns = text_wrap(T_SKIP, 0, 0, inner - 56, body - 2, lineh - 4, 0, 0, 0);
		int bh = land ? 64 : 76, cb = 38;
		int skip_h = ns * (lineh - 4) > cb ? ns * (lineh - 4) : cb;

		dh = pad + 46 + 12 + nb * lineh + 10 + lineh + 22 + skip_h + 26 + bh + 14 + bh + pad;
		dy = (lh - dh) / 2;
		if (dy < 8)
			dy = 8;
		if (draw)
			rrect(dx, dy, dx + dw, dy + dh, 18, C_BG, C_EDGE, 1.5f);
		y = dy + pad;
		if (draw) {
			/* a warning sign before the title */
			int s = 34, tx = dx + pad, ty = y + 6;

			seg(tx + s / 2.f, ty + 2, tx + 2, ty + s - 2, 4, 0xef9f27);
			seg(tx + s / 2.f, ty + 2, tx + s - 2, ty + s - 2, 4, 0xef9f27);
			seg(tx + 2, ty + s - 2, tx + s - 2, ty + s - 2, 4, 0xef9f27);
			seg(tx + s / 2.f, ty + 12, tx + s / 2.f, ty + s - 13, 3.5f, 0xef9f27);
			seg(tx + s / 2.f, ty + s - 7.5f, tx + s / 2.f, ty + s - 7.4f, 4, 0xef9f27);
			text_at(T_TITLE, -1, dx + pad + 48, base_in(y, y + 46, title, 1), title, 1, C_TITLE);
		}
		y += 46 + 12;
		if (draw)
			text_wrap(T_BODY, dx + pad, base_in(y, y + lineh, body, 0), inner, body, lineh, 0,
				  C_TEXT, 1);
		y += nb * lineh + 10;
		if (draw)
			text_at(T_SURE, -1, dx + pad, base_in(y, y + lineh, body, 1), body, 1, C_TITLE);
		y += lineh + 22;
		B[B_SKIP] = (struct box){ dx + pad - 8, y - 8, dx + dw - pad + 8, y + skip_h + 8 };
		if (draw) {
			checkbox(dx + pad, y + (skip_h - cb) / 2, cb, skip_countdown);
			text_wrap(T_SKIP, dx + pad + 56, base_in(y + (skip_h - ns * (lineh - 4)) / 2,
				  y + (skip_h - ns * (lineh - 4)) / 2 + lineh - 4, body - 2, 0),
				  inner - 56, body - 2, lineh - 4, 0, C_TEXT, 1);
		}
		y += skip_h + 26;
		B[B_CANCEL] = (struct box){ dx + pad, y, dx + pad + (inner - 14) / 2, y + bh };
		B[B_MANUAL] = (struct box){ dx + dw - pad - (inner - 14) / 2, y, dx + dw - pad, y + bh };
		y += bh + 14;
		B[B_CONTINUE] = (struct box){ dx + pad, y, dx + dw - pad, y + bh };
		if (draw) {
			char lab[48];
			int r = remaining();

			button(B_CANCEL, "Cancel", 0);
			button(B_MANUAL, "Manual kill", 0);
			if (r)
				snprintf(lab, sizeof(lab), "Continue (%d)", r);
			else
				snprintf(lab, sizeof(lab), "Continue");
			button(B_CONTINUE, lab, r ? 2 : 1);
		}
	} else if (view == V_MANUAL) {
		int bh = land ? 64 : 76, listh, n = nselected();

		dh = lh - 96 < 900 ? lh - 96 : 900;
		dy = (lh - dh) / 2;
		listh = dh - (pad + 46 + 16 + 20 + bh + pad);
		if (draw)
			rrect(dx, dy, dx + dw, dy + dh, 18, C_BG, C_EDGE, 1.5f);
		y = dy + pad;
		if (draw) {
			text_at("Choose processes to kill", -1, dx + pad, base_in(y, y + 46, title, 1), title, 1,
				C_TITLE);
		}
		y += 46 + 16;
		B[B_LIST] = (struct box){ dx + pad, y, dx + dw - pad, y + listh };
		scroll_max = np * row_h - listh;
		if (scroll_max < 0)
			scroll_max = 0;
		if (scroll > scroll_max)
			scroll = scroll_max;
		if (scroll < 0)
			scroll = 0;
		if (draw) {
			struct box L = B[B_LIST];

			rrect(L.x0, L.y0, L.x1, L.y1, 12, C_LIST, 0x40454c, 1.5f);
			clip_x0 = L.x0 + 2;
			clip_y0 = L.y0 + 2;
			clip_x1 = L.x1 - 2;
			clip_y1 = L.y1 - 2;
			if (!np)
				text_c("Nothing to kill but the basic processes", L.x0, L.x1, L.y0, L.y0 + 2 * row_h,
				       22, 0, C_OFF_T);
			for (int i = 0; i < np; i++) {
				int ry = L.y0 + i * row_h - (int)scroll;
				char meta[48];
				float mw;

				if (ry + row_h < L.y0 || ry > L.y1)
					continue;
				if (i)
					fill(L.x0 + 12, ry, L.x1 - 12, ry + 1, 0xff000000 | C_SEP);
				checkbox(L.x0 + 18, ry + (row_h - 34) / 2, 34, P[i].sel);
				snprintf(meta, sizeof(meta), "%d \xc2\xb7 %ld MB", P[i].pid,
					 (P[i].rss_kb + 512) / 1024);
				mw = text_w(meta, strlen(meta), 19, 0);
				text_at(meta, -1, L.x1 - 18 - mw, base_in(ry, ry + row_h, 19, 0), 19, 0, 0x9aa2aa);
				{
					/* the name, cut to the room left */
					char nm[48];
					size_t k = strlen(P[i].name);
					float room = L.x1 - 18 - mw - 24 - (L.x0 + 70);

					memcpy(nm, P[i].name, sizeof(nm));
					while (k > 1 && text_w(nm, k, 24, 0) > room)
						nm[--k] = 0;
					text_at(nm, -1, L.x0 + 70, base_in(ry, ry + row_h, 24, 0), 24, 0, 0xffffff);
				}
			}
			clip_all();
		}
		y += listh + 20;
		B[B_BACK] = (struct box){ dx + pad, y, dx + pad + (inner - 14) / 2, y + bh };
		B[B_KILL] = (struct box){ dx + dw - pad - (inner - 14) / 2, y, dx + dw - pad, y + bh };
		if (draw) {
			char lab[32];

			button(B_BACK, "Back", 0);
			if (n)
				snprintf(lab, sizeof(lab), "Kill (%d)", n);
			else
				snprintf(lab, sizeof(lab), "Kill");
			button(B_KILL, lab, n ? 1 : 2);
		}
	}
}

static void redraw(void)
{
	if (view == V_CLOSED || out == OUT_NONE)
		return;
	layout(1);
	present();
	shown_count = remaining();
}

/* the canvas at the screen's size now */
static void canvas_fit(void)
{
	int w = (!canvas_turned && (rot == 90 || rot == 270)) ? ph : pw;
	int h = (!canvas_turned && (rot == 90 || rot == 270)) ? pw : ph;

	if (w != lw || h != lh || !cv) {
		free(cv);
		lw = w;
		lh = h;
		cv = calloc((size_t)lw * lh, 4);
	}
	clip_all();
}

/* one finger works the window: where it went down, whether it scrolled */
static struct { int slot; float x0, y0, x, y; double sc0; int scrolling; } T = { .slot = -1 };

/* a signal to every process of this name; how many got it */
static int signal_comm(const char *name, int sig)
{
	DIR *d = opendir("/proc");
	struct dirent *e;
	int n = 0;

	while (d && (e = readdir(d))) {
		struct pinfo pi;
		pid_t pid;

		if (!isdigit((unsigned char)e->d_name[0]))
			continue;
		pid = atoi(e->d_name);
		if (!pinfo(pid, &pi) && !strcmp(pi.comm, name) && !kill(pid, sig))
			n++;
	}
	if (d)
		closedir(d);
	return n;
}

static int screenpad_aside;	/* screenpad was asked to let go for the window */

/* the touchscreen, opened and grabbed (screenpad may take a moment to let
 * go of it) */
static int touch_take(void)
{
	for (int i = 0; i < 20; i++) {
		if (ts < 0)
			ts_open();
		if (ts >= 0) {
			grab(1);
			if (grabbed)
				return 0;
		}
		usleep(50000);
	}
	return ts >= 0 ? 0 : -1;	/* not grabbed: shared, still usable */
}

static void touch_drop(void)
{
	grab(0);
	if (ts >= 0)
		close(ts);
	ts = -1;
	T.slot = -1;
}

static void open_window(void)
{
	if (view != V_CLOSED)
		return;
	if (signal_comm("screenpad", SIGUSR1)) {
		screenpad_aside = 1;
		usleep(200000);
		logf_("screenpad set aside");
	}
	if (touch_take()) {
		logf_("no touchscreen: no window");
		if (screenpad_aside)
			signal_comm("screenpad", SIGUSR2);
		screenpad_aside = 0;
		return;
	}
	screen_pick();
	if (out == OUT_NONE) {
		logf_("nowhere to draw the window");
		return;
	}
	canvas_fit();
	skip_countdown = access(SKIP_FILE, F_OK) == 0;
	deadline = now() + COUNTDOWN;
	view = V_CONFIRM;
	pressed = B_NONE;
	/* the fingers down now are not the window's */
	for (int s = 0; s < NSLOTS; s++)
		F[s].down = F[s].id >= 0;
	logf_("window open (%s, %dx%d, turned %d)", "fb0", lw, lh, rot);
	redraw();
}

static void close_window(void)
{
	if (view == V_CLOSED)
		return;
	view = V_CLOSED;
	screen_release();
	touch_drop();
	if (screenpad_aside)
		signal_comm("screenpad", SIGUSR2);	/* (if it is still there) */
	screenpad_aside = 0;
	logf_("window closed");
}

static void set_skip(int on)
{
	skip_countdown = on;
	if (on) {
		int fd;

		mkdir("/var/lib/task-killer", 0755);
		fd = open(SKIP_FILE, O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
		if (fd >= 0)
			close(fd);
	} else {
		unlink(SKIP_FILE);
	}
}

static int in(const struct box *b, float x, float y)
{
	return b->x1 > b->x0 && x >= b->x0 && x < b->x1 && y >= b->y0 && y < b->y1;
}

static int hit(float x, float y)
{
	for (int i = 1; i < NB; i++)
		if (in(&B[i], x, y))
			return i;
	return B_NONE;
}


static void tap(int b, float x, float y)
{
	switch (b) {
	case B_CANCEL:
		close_window();
		break;
	case B_SKIP:
		set_skip(!skip_countdown);
		redraw();
		break;
	case B_MANUAL:
		scan();
		scroll = 0;
		view = V_MANUAL;
		redraw();
		break;
	case B_CONTINUE:
		if (remaining())
			break;
		screenpad_aside = 0;		/* it goes with the rest */
		close_window();
		kill_all();
		break;
	case B_BACK:
		view = V_CONFIRM;
		deadline = now() + COUNTDOWN;
		redraw();
		break;
	case B_LIST: {
		int i = (int)((y - B[B_LIST].y0 + scroll) / row_h);

		if (i >= 0 && i < np) {
			P[i].sel = !P[i].sel;
			redraw();
		}
		break;
	}
	case B_KILL: {
		static pid_t pids[MAXPROC];
		int n = 0;

		for (int i = 0; i < np; i++)
			if (P[i].sel)
				pids[n++] = P[i].pid;
		if (!n)
			break;
		close_window();
		kill_list(pids, n);
		break;
	}
	}
}

static void touch_frame(void)
{
	for (int s = 0; s < NSLOTS; s++) {
		struct finger *f = &F[s];
		float u, v, x, y;

		norm(f, &u, &v);
		if (f->id >= 0 && (!f->down || f->id != f->last_id)) {	/* a new finger */
			if (debug)
				logf_("touch slot %d id %d at %.3f %.3f", s, f->id, u, v);
			if (view != V_CLOSED && T.slot < 0) {
				to_canvas(u, v, &x, &y);
				T.slot = s;
				T.x0 = T.x = x;
				T.y0 = T.y = y;
				T.sc0 = scroll;
				T.scrolling = 0;
				pressed = hit(x, y);
				if (pressed != B_NONE && pressed != B_LIST && pressed != B_SKIP)
					redraw();
			}
		} else if (f->id >= 0 && s == T.slot && view != V_CLOSED) {	/* it moves */
			to_canvas(u, v, &x, &y);
			T.x = x;
			T.y = y;
			if (pressed == B_LIST && fabsf(y - T.y0) > 14)
				T.scrolling = 1;
			if (T.scrolling) {
				double sc = T.sc0 - (y - T.y0);

				if (sc < 0) sc = 0;
				if (sc > scroll_max) sc = scroll_max;
				if (sc != scroll) {
					scroll = sc;
					redraw();
				}
			}
		} else if (f->id < 0 && f->down && s == T.slot) {	/* it lifts */
			int p = pressed;

			T.slot = -1;
			pressed = B_NONE;
			if (view != V_CLOSED && !T.scrolling && p != B_NONE && hit(T.x, T.y) == p)
				tap(p, T.x, T.y);
			else if (view != V_CLOSED)
				redraw();
		}
	}
	for (int s = 0; s < NSLOTS; s++) {
		F[s].down = F[s].id >= 0;
		F[s].last_id = F[s].id;
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
			} else if (e->type == EV_SYN && e->code == SYN_DROPPED) {
				for (int s = 0; s < NSLOTS; s++)
					F[s].id = -1;
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
					break;
				case ABS_MT_POSITION_Y:
					f->y = e->value;
					break;
				}
			}
		}
}

/* ---- the whole ------------------------------------------------------------ */

static volatile sig_atomic_t quit, show_req;

/* the buttons (gpio-keys): Home three times, each within 0.8 s */
static int keys = -1;
static double home_t[3];

static void keys_open(void)
{
	char path[64], name[64];

	for (int i = 0; i < 32 && keys < 0; i++) {
		snprintf(path, sizeof(path), "/dev/input/event%d", i);
		keys = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
		if (keys < 0)
			continue;
		name[0] = 0;
		ioctl(keys, EVIOCGNAME(sizeof(name)), name);
		if (!strcmp(name, "gpio-keys")) {
			logf_("buttons: %s", path);
			return;
		}
		close(keys);
		keys = -1;
	}
	logf_("no gpio-keys: only SIGUSR1 opens the window");
}

static void keys_read(void)
{
	struct input_event ev[16];
	ssize_t n;

	while ((n = read(keys, ev, sizeof(ev))) > 0)
		for (size_t i = 0; i < (size_t)n / sizeof(ev[0]); i++) {
			if (ev[i].type != EV_KEY || ev[i].code != KEY_HOMEPAGE || ev[i].value != 1)
				continue;
			home_t[0] = home_t[1];
			home_t[1] = home_t[2];
			home_t[2] = now();
			if (home_t[0] && home_t[1] - home_t[0] < 0.8 && home_t[2] - home_t[1] < 0.8) {
				home_t[0] = home_t[1] = home_t[2] = 0;
				show_req = 1;
			}
		}
}

static void on_signal(int sig)
{
	if (sig == SIGUSR1)
		show_req = 1;
	else
		quit = 1;
}

#ifdef TK_TEST
/* the window drawn into PPM files on a Linux host, for a look at the layout:
 *   cc -DTK_TEST -I patches/files/include/uapi/drm -I /usr/include/libdrm -I tools/screenpad \
 *      tools/task-killer/task-killer.c -lm && ./a.out DejaVuSans.ttf DejaVuSans-Bold.ttf */
static void dump(const char *path)
{
	FILE *f = fopen(path, "wb");

	fprintf(f, "P6 %d %d 255\n", lw, lh);
	for (int i = 0; i < lw * lh; i++) {
		uint32_t c = cv[i], a = c >> 24, bg = (i / lw / 64 + i % lw / 64) % 2 ? 0x50 : 0x70;
		unsigned char px[3] = { ((c >> 16 & 255) * a + bg * (255 - a)) / 255,
					((c >> 8 & 255) * a + bg * (255 - a)) / 255,
					((c & 255) * a + bg * (255 - a)) / 255 };
		fwrite(px, 1, 3, f);
	}
	fclose(f);
}

int main(int argc, char **argv)
{
	const char *names[] = { "supertux2", "labwc", "waybar", "Xwayland", "thunar", "foot",
				"python3 screenpad-editor", "wvkbd-mobintl", "swaybg", "dbus-daemon",
				"autorotate", "kmscube", "sh desktop-labwc" };

	have_r = font_load(&font_r, argv[1]);
	have_b = font_load(&font_b, argv[2]);
	for (int i = 0; i < 13; i++) {
		P[i].pid = 1200 + 37 * i;
		P[i].rss_kb = (13 - i) * 9000;
		P[i].sel = i == 0 || i == 4;
		snprintf(P[i].name, sizeof(P[i].name), "%s", names[i]);
	}
	np = 13;
	for (int land = 0; land < 2; land++) {
		char path[64];

		pw = land ? 1024 : 768;
		ph = land ? 768 : 1024;
		canvas_turned = 1;
		canvas_fit();
		view = V_CONFIRM;
		deadline = now() + 3.5;
		skip_countdown = 0;
		layout(1);
		snprintf(path, sizeof(path), "confirm-%d.ppm", land);
		dump(path);
		deadline = 0;
		skip_countdown = 1;
		layout(1);
		snprintf(path, sizeof(path), "confirm-ready-%d.ppm", land);
		dump(path);
		view = V_MANUAL;
		scroll = land ? 120 : 0;
		layout(1);
		snprintf(path, sizeof(path), "manual-%d.ppm", land);
		dump(path);
	}
	return 0;
}
#else
int main(int argc, char **argv)
{
	struct sigaction sa = { .sa_handler = on_signal };
	int show_now = argc > 1 && !strcmp(argv[1], "--show");

	if (argc > 1 && !strcmp(argv[1], "--list")) {
		scan();
		for (int i = 0; i < np; i++)
			printf("%6d %6ld KB  %s\n", P[i].pid, P[i].rss_kb, P[i].name);
		return 0;
	}
	if (argc > 1 && !show_now) {
		fprintf(stderr, "usage: task-killer [--show | --list]\n"
			"  Home pressed three times (or SIGUSR1): the window that\n"
			"  kills what will not let go of the screen.\n");
		return 2;
	}
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGUSR1, &sa, NULL);
	signal(SIGHUP, SIG_IGN);
	signal(SIGPIPE, SIG_IGN);
	debug = getenv("TK_DEBUG") && *getenv("TK_DEBUG") == '1';
	have_r = font_load(&font_r, FONT);
	have_b = font_load(&font_b, FONT_BOLD);
	if (!have_r)
		die("no font at %s (apk add font-dejavu)", FONT);
	keys_open();
	logf_("waiting: Home pressed three times opens the window");
	if (show_now)
		open_window();
	while (!quit) {
		struct pollfd pk[2] = { { .fd = ts, .events = POLLIN }, { .fd = keys, .events = POLLIN } };
		struct pollfd p;
		int timeout = view == V_CLOSED ? -1 : 200;

		if (poll(pk, 2, timeout) < 0 && errno != EINTR)
			break;
		p = pk[0];
		if (keys >= 0 && pk[1].revents & POLLIN)
			keys_read();
		if (ts >= 0 && p.revents & (POLLERR | POLLHUP | POLLNVAL)) {
			logf_("the touchscreen went away");
			grabbed = 0;
			close_window();
			continue;
		}
		if (p.revents & POLLIN)
			touch_read();
		if (show_req) {
			show_req = 0;
			open_window();
		}
		if (view != V_CLOSED) {
			if (view == V_CONFIRM && remaining() != shown_count)
				redraw();
		}
	}
	close_window();
	return 0;
}
#endif
