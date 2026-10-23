/*
 * p105-keys -- what the iPad's buttons do.
 *
 *   side switch        what the volume buttons set: the speakers' volume, or,
 *                      switched the other way, the backlight
 *   volume up / down   one step louder / quieter: 16 steps of 4 dB from
 *                      -48 dB to +12 dB, and below the first, off -- both
 *                      amps ("Left/Right Speaker Volume" on the iPadMini
 *                      card, set through the ALSA control device's ioctls);
 *                      or the backlight brighter / dimmer in 32 steps (the
 *                      PMU's current DAC is exponential already, so equal
 *                      steps look equal).  Held: keeps going
 *   hold               screen off / on; while it is off the touchscreen is
 *                      grabbed, so a finger on the dark glass types nothing.
 *                      The volume still works with the screen off
 *   home, held 0.8 s   the rotation lock on / off (/run/rotation-lock, the
 *                      file tools/desktop/autorotate.sh obeys): the
 *                      backlight dips once for on, twice for off
 *
 * It takes the buttons (gpio-keys) for itself with EVIOCGRAB, so nothing else
 * acts on them -- an XFCE session would otherwise offer to shut down on Hold.
 * Started by stage 2; the backlight is /sys/class/backlight/apple-pmu-wled.
 *
 * Built static by ./cascadia build (like z2-boot): one C file, no libraries.
 */
#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <sound/asound.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#define BL "/sys/class/backlight/apple-pmu-wled/"
#define BITS(n) (((n) + 8 * sizeof(long) - 1) / (8 * sizeof(long)))

static int rd_int(const char *f)
{
	char b[16] = "";
	int fd = open(f, O_RDONLY), n;

	if (fd < 0)
		return -1;
	n = read(fd, b, sizeof(b) - 1);
	close(fd);
	return n > 0 ? atoi(b) : -1;
}

static void wr_int(const char *f, int v)
{
	char b[16];
	int fd = open(f, O_WRONLY);

	if (fd < 0)
		return;
	snprintf(b, sizeof(b), "%d\n", v);
	if (write(fd, b, strlen(b)) < 0)
		perror(f);
	close(fd);
}

static int has_bit(const unsigned long *b, int n)
{
	return (b[n / (8 * sizeof(long))] >> (n % (8 * sizeof(long)))) & 1;
}

/* the first event device that answers PRED */
static int find(int (*pred)(int fd), char *path, size_t len)
{
	DIR *d = opendir("/dev/input");
	struct dirent *e;
	int fd = -1;

	while (d && (e = readdir(d))) {
		if (strncmp(e->d_name, "event", 5))
			continue;
		snprintf(path, len, "/dev/input/%s", e->d_name);
		fd = open(path, O_RDONLY);
		if (fd >= 0 && pred(fd))
			break;
		if (fd >= 0)
			close(fd);
		fd = -1;
	}
	if (d)
		closedir(d);
	return fd;
}

static int is_buttons(int fd)
{
	char name[64] = "";

	ioctl(fd, EVIOCGNAME(sizeof(name)), name);
	return !strcmp(name, "gpio-keys");
}

static int is_touch(int fd)
{
	unsigned long abs[BITS(ABS_CNT)] = { 0 };

	ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(abs)), abs);
	return has_bit(abs, ABS_MT_POSITION_X);
}

#define STEP 64		/* of 0..2047; 64 is about 1 nit, the bottom */

#define LOCK "/run/rotation-lock"
#define HOME_HOLD_MS 800

static long now_ms(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1000L + t.tv_nsec / 1000000;
}

/*
 * The rotation lock turned over.  autorotate shows it on the panel, but
 * Home is held without looking there: the backlight dips once for on,
 * twice for off (not with the screen off).
 */
static void rotation_lock(int off, int max)
{
	int on = access(LOCK, F_OK) != 0, level = rd_int(BL "brightness"), fd;

	if (on) {
		fd = open(LOCK, O_WRONLY | O_CREAT, 0644);
		if (fd >= 0)
			close(fd);
	} else {
		unlink(LOCK);
	}
	fprintf(stderr, "rotation lock %s\n", on ? "on" : "off");
	if (off || max <= 0 || level <= 0)
		return;
	for (int i = 0; i < (on ? 1 : 2); i++) {
		wr_int(BL "brightness", level / 4 > 0 ? level / 4 : 1);
		usleep(120000);
		wr_int(BL "brightness", level);
		usleep(120000);
	}
}

/*
 * The side switch's state (SW_MUTE_DEVICE) in which the volume buttons set
 * the backlight; in the other they set the speakers.  1 is the switch's
 * pin pulled low: the position iOS calls "mute", orange showing.
 */
#define SWITCH_BACKLIGHT 1

/* the speakers' volume: 0.5 dB a unit, 204 = 0 dB, 228 = +12 dB, 0 = -102 dB */
#define VOL_0DB 204
#define VOL_STEPS 16	/* 4 dB each, the top +12 dB: 1 = -48 dB */

static const char * const vol_ctls[] = { "Left Speaker Volume", "Right Speaker Volume" };

/* the iPadMini card's control device, or -1 */
static int sound_ctl(void)
{
	char path[32];

	for (int i = 0; i < 8; i++) {
		struct snd_ctl_card_info info = { 0 };
		int fd;

		snprintf(path, sizeof(path), "/dev/snd/controlC%d", i);
		fd = open(path, O_RDWR);
		if (fd < 0)
			continue;
		if (!ioctl(fd, SNDRV_CTL_IOCTL_CARD_INFO, &info) &&
		    !strcmp((char *)info.id, "iPadMini"))
			return fd;
		close(fd);
	}
	return -1;
}

static int vol_get(int fd, const char *name, long *v)
{
	struct snd_ctl_elem_value e = { 0 };

	e.id.iface = SNDRV_CTL_ELEM_IFACE_MIXER;
	strncpy((char *)e.id.name, name, sizeof(e.id.name) - 1);
	if (ioctl(fd, SNDRV_CTL_IOCTL_ELEM_READ, &e))
		return -1;
	*v = e.value.integer.value[0];
	return 0;
}

static void vol_set(int fd, const char *name, long v)
{
	struct snd_ctl_elem_value e = { 0 };

	e.id.iface = SNDRV_CTL_ELEM_IFACE_MIXER;
	strncpy((char *)e.id.name, name, sizeof(e.id.name) - 1);
	e.value.integer.value[0] = v;
	if (ioctl(fd, SNDRV_CTL_IOCTL_ELEM_WRITE, &e))
		perror(name);
}

/*
 * One step up or down on the 0..VOL_STEPS scale (0 = off, 1 = -48 dB, each
 * next 4 dB louder), from the left amp's level; a level between two steps
 * (set by amixer, say) goes to the nearer one in the button's direction.
 */
static void vol_step(int up)
{
	const long base = VOL_0DB - 96;
	int fd = sound_ctl(), k;
	long v;

	if (fd < 0)
		return;
	if (!vol_get(fd, vol_ctls[0], &v)) {
		if (v < base)
			k = up ? 1 : 0;
		else {
			k = (v - base) / 8 + 1;		/* the step at or below */
			if (up)
				k++;
			else if ((v - base) % 8 == 0)
				k--;
		}
		k = k < 0 ? 0 : k > VOL_STEPS ? VOL_STEPS : k;
		v = k ? base + 8 * (k - 1) : 0;
		for (unsigned i = 0; i < sizeof(vol_ctls) / sizeof(*vol_ctls); i++)
			vol_set(fd, vol_ctls[i], v);
		fprintf(stderr, "volume %s: step %d of %d (%ld)\n", up ? "up" : "down",
			k, VOL_STEPS, v);
	}
	close(fd);
}

int main(void)
{
	char path[300];
	struct input_event ev;
	unsigned long sw[BITS(SW_CNT)] = { 0 };
	int keys, touch = -1, max, level, off = 0, backlight_mode;
	long home = 0;		/* when Home went down, while it is held and has not acted */

	for (int tries = 0; (keys = find(is_buttons, path, sizeof(path))) < 0; tries++) {
		if (tries == 30) {
			fprintf(stderr, "p105-keys: no gpio-keys device\n");
			return 1;
		}
		sleep(1);
	}
	ioctl(keys, EVIOCGRAB, 1);
	ioctl(keys, EVIOCGSW(sizeof(sw)), sw);
	backlight_mode = has_bit(sw, SW_MUTE_DEVICE) == SWITCH_BACKLIGHT;
	fprintf(stderr, "side switch %d: the buttons set the %s\n",
		has_bit(sw, SW_MUTE_DEVICE), backlight_mode ? "backlight" : "volume");
	max = rd_int(BL "max_brightness");
	level = rd_int(BL "brightness");
	if (max <= 0 || level < 0)
		fprintf(stderr, "p105-keys: no backlight at " BL "\n");

	for (;;) {
		if (home) {
			struct pollfd p = { .fd = keys, .events = POLLIN };
			long left = home + HOME_HOLD_MS - now_ms();
			int r = left > 0 ? poll(&p, 1, left) : 0;

			if (r < 0)
				continue;
			if (r == 0) {
				rotation_lock(off, max);
				home = 0;
				continue;
			}
		}
		if (read(keys, &ev, sizeof(ev)) != sizeof(ev))
			break;
		if (ev.type == EV_KEY && ev.value && (ev.code == KEY_VOLUMEUP || ev.code == KEY_VOLUMEDOWN) &&
		    !backlight_mode) {
			vol_step(ev.code == KEY_VOLUMEUP);
		} else if (ev.type == EV_KEY && ev.value && (ev.code == KEY_VOLUMEUP || ev.code == KEY_VOLUMEDOWN)) {
			if (off || max <= 0)
				continue;
			level = rd_int(BL "brightness");	/* someone else may have set it */
			level += ev.code == KEY_VOLUMEUP ? STEP : -STEP;
			level = level < STEP ? STEP : level > max ? max : level;
			wr_int(BL "brightness", level);
			fprintf(stderr, "backlight %s: %d\n",
				ev.code == KEY_VOLUMEUP ? "up" : "down", level);
		} else if (ev.type == EV_KEY && ev.value == 1 && ev.code == KEY_POWER) {
			off = !off;
			wr_int(BL "bl_power", off ? 4 : 0);	/* FB_BLANK_POWERDOWN / UNBLANK */
			if (off) {
				char tpath[300];

				touch = find(is_touch, tpath, sizeof(tpath));
				if (touch >= 0)
					ioctl(touch, EVIOCGRAB, 1);
			} else if (touch >= 0) {
				ioctl(touch, EVIOCGRAB, 0);
				close(touch);
				touch = -1;
			}
		} else if (ev.type == EV_SW && ev.code == SW_MUTE_DEVICE) {
			backlight_mode = ev.value == SWITCH_BACKLIGHT;
			fprintf(stderr, "side switch %d: the buttons set the %s\n", ev.value,
				backlight_mode ? "backlight" : "volume");
		} else if (ev.type == EV_KEY && ev.code == KEY_HOMEPAGE && ev.value != 2) {
			home = ev.value ? now_ms() : 0;
		}
	}
	perror("p105-keys: read");
	return 1;
}
