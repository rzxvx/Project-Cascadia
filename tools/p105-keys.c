/*
 * p105-keys -- what the iPad's buttons do.
 *
 *   volume up / down   backlight brighter / dimmer (held: keeps going), in
 *                      32 steps -- the PMU's current DAC is exponential
 *                      already, so equal steps look equal
 *   hold               screen off / on; while it is off the touchscreen is
 *                      grabbed, so a finger on the dark glass types nothing
 *
 * It takes the buttons (gpio-keys) for itself with EVIOCGRAB, so nothing else
 * acts on them -- an XFCE session would otherwise offer to shut down on Hold.
 * The side switch is read but not used yet (orientation lock is the plan).
 * Started by stage 2; the backlight is /sys/class/backlight/apple-pmu-wled.
 *
 * Built static by ./cascadia build (like z2-boot): one C file, no libraries.
 */
#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
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

int main(void)
{
	char path[300];
	struct input_event ev;
	int keys, touch = -1, max, level, off = 0;

	for (int tries = 0; (keys = find(is_buttons, path, sizeof(path))) < 0; tries++) {
		if (tries == 30) {
			fprintf(stderr, "p105-keys: no gpio-keys device\n");
			return 1;
		}
		sleep(1);
	}
	ioctl(keys, EVIOCGRAB, 1);
	max = rd_int(BL "max_brightness");
	level = rd_int(BL "brightness");
	if (max <= 0 || level < 0)
		fprintf(stderr, "p105-keys: no backlight at " BL "\n");

	while (read(keys, &ev, sizeof(ev)) == sizeof(ev)) {
		if (ev.type == EV_KEY && ev.value && (ev.code == KEY_VOLUMEUP || ev.code == KEY_VOLUMEDOWN)) {
			if (off || max <= 0)
				continue;
			level = rd_int(BL "brightness");	/* someone else may have set it */
			level += ev.code == KEY_VOLUMEUP ? STEP : -STEP;
			level = level < STEP ? STEP : level > max ? max : level;
			wr_int(BL "brightness", level);
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
			/* the side switch: orientation lock / what the buttons do, later */
		}
	}
	perror("p105-keys: read");
	return 1;
}
