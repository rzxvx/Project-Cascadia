/* usbwatch -- log the USB OTG controller's and PHY's state as it changes, and
 * the battery every 10 s, and serve the log on TCP 5555, so that it can be
 * read over Wi-Fi while the USB link -- and an NFS root with it -- is gone.
 * Reads only.  Static, and run from RAM so that a hung NFS root cannot stop it:
 *
 *   cp usbwatch /run/ && cd /run && setsid ./usbwatch </dev/null >/dev/null 2>&1 &
 *   host:  bash -c 'exec 3<>/dev/tcp/IPAD/5555; cat <&3'
 *
 * Built with the cross compiler in the build image (tools/sgx/lib/cross.sh's
 * sysroot), linked -static.  It caught dwc2's partial power down on unplug
 * (GitHub issue #1, 2026-10-02). */
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/klog.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static volatile uint32_t *otg, *phy, *cplx, *pmgr;
static const char *LOG = "/run/usbwatch.log";

static long rd(const char *f)
{
	char b[32] = "";
	int fd = open(f, O_RDONLY), n;
	if (fd < 0) return -1;
	n = read(fd, b, sizeof b - 1);
	close(fd);
	if (n > 0 && (b[0] < '0' || b[0] > '9') && b[0] != '-') return b[0];	/* status: first letter */
	return n > 0 ? atol(b) : -1;
}

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }
static volatile uint32_t *map(int fd, unsigned long pa, size_t n)
{
	void *p = mmap(0, n, PROT_READ, MAP_SHARED, fd, pa & ~0xfffUL);
	if (p == MAP_FAILED) { perror("mmap"); exit(1); }
	return (volatile uint32_t *)((char *)p + (pa & 0xfff));
}

#define NREG 18
static void snap(uint32_t *v)
{
	v[0] = otg[0x000 / 4];			/* GOTGCTL */
	v[1] = otg[0x004 / 4];			/* GOTGINT */
	v[2] = otg[0x00c / 4];			/* GUSBCFG */
	v[3] = otg[0x014 / 4] & 0xc0803c04;	/* GINTSTS: wkup sessreq resetdet enumdone usbrst usbsusp erlysusp otgint */
	v[4] = otg[0x018 / 4];			/* GINTMSK */
	v[5] = otg[0x800 / 4];			/* DCFG */
	v[6] = otg[0x804 / 4];			/* DCTL */
	v[7] = otg[0x808 / 4] & 0xf;		/* DSTS: suspsts, enumspd, errticerr */
	v[8] = otg[0xe00 / 4];			/* PCGCTL */
	v[9] = phy[0x00 / 4];			/* OPHYPWR */
	v[10] = phy[0x04 / 4];			/* OPHYCLK */
	v[11] = phy[0x08 / 4];			/* ORSTCON */
	v[12] = phy[0x1c / 4];
	v[13] = phy[0x30 / 4];			/* UOTGTUNE1? */
	v[14] = phy[0x34 / 4];			/* UOTGTUNE2? */
	v[15] = phy[0x44 / 4];
	v[16] = cplx[0];			/* usb-complex */
	v[17] = pmgr[0];			/* PMGR USB-OTG */
}
static const char *names[NREG] = { "GOTGCTL", "GOTGINT", "GUSBCFG", "GINTSTS*", "GINTMSK", "DCFG", "DCTL",
	"DSTS*", "PCGCTL", "OPHYPWR", "OPHYCLK", "ORSTCON", "PHY1C", "PHY30", "PHY34", "PHY44", "USBCPLX", "PMGR_OTG" };

static void *server(void *arg)
{
	int s = socket(AF_INET, SOCK_STREAM, 0), one = 1;
	struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(5555) };
	setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
	if (bind(s, (void *)&a, sizeof a) || listen(s, 2)) { perror("server"); return 0; }
	for (;;) {
		int c = accept(s, 0, 0), n;
		char buf[65536];
		if (c < 0) continue;
		int f = open(LOG, O_RDONLY);
		while (f >= 0 && (n = read(f, buf, sizeof buf)) > 0) write(c, buf, n);
		if (f >= 0) close(f);
		write(c, "\n--- dmesg (tail) ---\n", 22);
		n = klogctl(3, buf, sizeof buf);		/* SYSLOG_ACTION_READ_ALL */
		if (n > 0) { int off = n > 16384 ? n - 16384 : 0; write(c, buf + off, n - off); }
		close(c);
	}
}

int main(void)
{
	int fd = open("/dev/mem", O_RDONLY | O_SYNC);
	uint32_t v[NREG], last[NREG];
	double t0 = now();
	pthread_t th;
	FILE *lg;

	if (fd < 0) { perror("/dev/mem"); return 1; }
	otg = map(fd, 0x36100000, 0x1000);
	phy = map(fd, 0x36000000, 0x1000);
	cplx = map(fd, 0x3f108000, 0x10);
	pmgr = map(fd, 0x3f101084, 0x4);
	lg = fopen(LOG, "w");
	setvbuf(lg, 0, _IOLBF, 0);
	pthread_create(&th, 0, server, 0);
	snap(last);
	fprintf(lg, "%8.3f start:", 0.0);
	for (int i = 0; i < NREG; i++) fprintf(lg, " %s=%08x", names[i], last[i]);
	fprintf(lg, "\n");
	for (int tick = 0;; tick++) {
		usleep(20000);
		if (tick % 500 == 0) {			/* every 10 s */
			const char *B = "/sys/class/power_supply/battery/";
			char f[96];
			long cur, mv, cap, st;
			snprintf(f, sizeof f, "%scurrent_now", B); cur = rd(f);
			snprintf(f, sizeof f, "%svoltage_now", B); mv = rd(f);
			snprintf(f, sizeof f, "%scapacity", B); cap = rd(f);
			snprintf(f, sizeof f, "%sstatus", B); st = rd(f);
			fprintf(lg, "%8.3f battery %ld mA %ld mV %ld%% %c VBUS %s\n", now() - t0,
				cur / 1000, mv / 1000, cap, (int)st, (otg[0] & 0x80000) ? "on" : "off");
		}
		snap(v);
		if (memcmp(v, last, sizeof v)) {
			fprintf(lg, "%8.3f", now() - t0);
			for (int i = 0; i < NREG; i++)
				if (v[i] != last[i]) fprintf(lg, " %s %08x->%08x", names[i], last[i], v[i]);
			fprintf(lg, "\n");
			memcpy(last, v, sizeof v);
		}
	}
}
