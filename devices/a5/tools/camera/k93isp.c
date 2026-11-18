/*
 * k93isp.c -- the iPad 2's camera ISP from user space: its own CPU booted.
 *
 *   k93isp boot FIRMWARE   power the ISP, program its IOMMU, load the firmware,
 *                          start the ISP's CPU and run iOS's handshake up to
 *                          the IPC channel table (docs/research/k93-camera.md)
 *   k93isp off             the ISP's CPU stopped, the block's clock gated
 *   k93isp regs            the registers that matter, as they are
 *
 * The firmware is iOS 8.4.1's (H4ISPServices' H4ISPFIRMWARE section, the
 * research note says how to get it): Apple's, so it is not in the
 * repository.  Its memory is the DT's isp reserved region (no-map), reached
 * through /dev/mem and so uncached: what the CPU writes is what the ISP reads.
 *
 * Every step is iOS's AppleH4CamIn (ISP_Init 0x807defc0) and
 * AppleS5L8930XDART, iOS 8.4.1's K93 kernelcache, unless it says otherwise.
 * Build: arm-linux-gnueabihf-gcc -O2 -static -o k93isp k93isp.c
 *
 * This is how the cameras were brought up (milestones 1-3); the kernel's
 * driver, drivers/media/platform/apple/apple-h4isp.c, does the same now, and
 * dts/k93ap.dts no longer reserves the region this needs.  To run it again:
 * a DT with that reservation back (isp@9d600000, 32 MB, no-map) and the
 * driver's node disabled, so that the two never drive the ISP at once.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

typedef uint32_t u32;

#define PMGR		0x3f100000
#define PS_HPERF_ISP	0x1048		/* power state of the ISP and its IOMMU (ADT gate 45) */
#define ISP_BASE	0x3a000000	/* 1 MB */
#define DART_BASE	0x3a400000	/* dart-rt, 8 KB */
#define GPIO_BASE	0x3fa00000	/* one register a pin: bit 0 level, 3:1 mode, 5 peripheral */
#define PIN_REAR_EN	192		/* function-sensor_shutdown (0x1800): active high */
#define PIN_REAR_CLK	193		/* function-sensor_extclk (0x1801) */
#define PIN_FRONT_EN	194		/* function-sensor_shutdown2 (0x1802): active low */
#define PIN_FRONT_CLK	195		/* function-sensor_extclk2 (0x1803) */
#define MEM_BASE	0x9d600000	/* the DT's isp reserved region */
#define MEM_SIZE	0x2000000

/* the region's layout; an offset in it is also its IOMMU address */
#define FW_VA		0x000000	/* the firmware's buffer: iOS's camFWBufSize */
#define FW_SIZE		0x200000
#define POOL_VA		0x200000	/* iOS's 64 KB shared region: IPC queues, the descriptor */
#define POOL_SIZE	0x10000
#define HEAP_VA		0x400000	/* the heap the firmware asks for, at most 4 MB */
#define HEAP_MAX	0x400000
#define MAP_SIZE	0x1e00000	/* what the IOMMU maps: 30 MB from address 0 */
#define PT_OFF		0x1f00000	/* its page tables: one 4 KB table per 4 MB */
#define DUMMY_OFF	0x1ff0000	/* where a faulting access goes (iOS: 0x30000000, our SDIO) */

/* ISP registers */
#define ISP_RUN		0x40004		/* 1: the ISP's CPU runs */
#define ISP_BOOT	0x40008		/* its firmware's address | 0x80000000 */
#define ISP_IRQ_STATUS	0x41000		/* bits 7:4 from the ISP's CPU */
#define ISP_IRQ_ENABLE	0x41004
#define ISP_IRQ_ACK2	0x41020
#define ISP_IRQ_CLEAR	0x41024
#define ISP_MIPI0	0xc0000		/* per camera channel, iOS's 0x1201 for these sensors */
#define ISP_MIPI1	0xc0004
#define ISP_MBOX	0xc3000		/* eight words, both ways */

#define SENSOR_TYPE	6		/* the ADT's sensor-type, handed to the firmware */
#define MAGIC_READY	0x8042006

static volatile u32 *pmgr, *isp, *dart, *gpio;
static volatile uint8_t *mem;

static void *map(int fd, off_t pa, size_t len)
{
	void *p = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, pa);

	if (p == MAP_FAILED) {
		fprintf(stderr, "mmap %#lx: %s\n", (unsigned long)pa, strerror(errno));
		exit(1);
	}
	return p;
}

static u32 rd(volatile u32 *b, u32 off) { return b[off / 4]; }
static void wr(volatile u32 *b, u32 off, u32 v) { b[off / 4] = v; }
static u32 mrd(u32 off) { return *(volatile u32 *)(mem + off); }
static void mwr(u32 off, u32 v) { *(volatile u32 *)(mem + off) = v; }

static void msleep(int ms)
{
	struct timespec t = { ms / 1000, (ms % 1000) * 1000000L };

	nanosleep(&t, NULL);
}

static void mzero(u32 off, u32 len)
{
	for (u32 i = 0; i < len; i += 4)
		mwr(off + i, 0);
}

/* --- power -------------------------------------------------------------- */

/* the PMGR's power-state switch (AppleS5L8940XIO::clock_gate_switch) */
static int ps_set(u32 reg, int on)
{
	u32 v = rd(pmgr, reg);
	int i;

	wr(pmgr, reg, (v & ~0x10fu) | (on ? 0xf : 0));
	for (i = 0; i < 1000; i++) {
		v = rd(pmgr, reg);
		if (((v >> 4) & 0xf) == (v & 0xf))
			return 0;
		msleep(1);
	}
	fprintf(stderr, "power state %#x stuck at %#x\n", PMGR + reg, v);
	return -1;
}

/* the device reset: a pulse on bit 31 of the same register */
static void ps_reset(u32 reg)
{
	u32 v = rd(pmgr, reg);

	wr(pmgr, reg, v | 0x80000000u);
	msleep(1);
	wr(pmgr, reg, v & ~0x80000000u);
	msleep(1);
}

/*
 * The sensors (iOS's EnableSensor, 0x807de96c, for both): their clock pins to
 * the peripheral function, 1 ms, then each sensor's enable to its active
 * level (the ADT's flags: rear active high, front inverted), 5 ms.  Their
 * supply, PMU LDO 10 (0x21 bit 0x80, 2.85 V), is on as iBoot leaves it.
 * Off is the state iBoot leaves: both disabled, the clock pins driven low.
 */
static void pin_periph(int pin)
{
	u32 v = rd(gpio, pin * 4);

	wr(gpio, pin * 4, (v & ~0x2fu) | 0x220);
}

static void pin_out(int pin, int level)
{
	u32 v = rd(gpio, pin * 4);

	wr(gpio, pin * 4, (v & ~0x2fu) | 0x202 | (level & 1));
}

static void sensors(int on)
{
	if (on) {
		pin_periph(PIN_REAR_CLK);
		pin_periph(PIN_FRONT_CLK);
		msleep(1);
		pin_out(PIN_REAR_EN, 1);
		pin_out(PIN_FRONT_EN, 0);
		msleep(5);
	} else {
		pin_out(PIN_REAR_EN, 0);
		pin_out(PIN_FRONT_EN, 1);
		pin_out(PIN_REAR_CLK, 0);
		pin_out(PIN_FRONT_CLK, 0);
	}
	printf("sensors %s: pins 192-195 %#x %#x %#x %#x\n", on ? "on" : "off",
	       rd(gpio, PIN_REAR_EN * 4), rd(gpio, PIN_REAR_CLK * 4),
	       rd(gpio, PIN_FRONT_EN * 4), rd(gpio, PIN_FRONT_CLK * 4));
}

/* --- the IOMMU ------------------------------------------------------------ */

static int dart_wait(void)
{
	for (int i = 0; i < 1000; i++) {
		if (!(rd(dart, 0) & 8))
			return 0;
		msleep(1);
	}
	fprintf(stderr, "IOMMU busy: %#x\n", rd(dart, 0));
	return -1;
}

static int dart_set_ste(int stream, int idx, u32 v)
{
	if (dart_wait())
		return -1;
	wr(dart, 0x08, v);
	wr(dart, 0x00, stream << 8 | idx << 22 | 5);
	return dart_wait();
}

static u32 dart_get_ste(int stream, int idx)
{
	dart_wait();
	wr(dart, 0x00, stream << 8 | idx << 22 | 4);
	dart_wait();
	return rd(dart, 0x08);
}

/* [0, MAP_SIZE) of every stream onto the region, page for page; the rest invalid */
static int dart_init(void)
{
	int s, i, j, tables = MAP_SIZE >> 22;

	wr(dart, 0x10, 0x3f);
	mzero(DUMMY_OFF, 0x1000);
	wr(dart, 0x18, MEM_BASE + DUMMY_OFF);
	for (i = 0; i < tables; i++)
		for (j = 0; j < 1024; j++) {
			u32 va = (u32)i << 22 | (u32)j << 12;

			mwr(PT_OFF + i * 0x1000 + j * 4, va < MAP_SIZE ? (MEM_BASE + va) | 1 : 0);
		}
	for (s = 0; s < 4; s++)
		for (i = 0; i < 64; i++)
			if (dart_set_ste(s, i, i < tables ? (MEM_BASE + PT_OFF + i * 0x1000) | 1 : 0))
				return -1;
	/* streams 0-2 translated, 3 (no-fault-clients = 8) as iOS sets it */
	wr(dart, 0x0c, 0xbb808080);
	wr(dart, 0x1000, 0x7fffffff);
	wr(dart, 0x1004, 0);
	wr(dart, 0x1008, 0);
	wr(dart, 0x100c, 0);
	if (dart_wait())
		return -1;
	wr(dart, 0x00, 0xfffff02);		/* every TLB flushed */
	if (dart_wait())
		return -1;
	printf("IOMMU: %d MB mapped on streams 0-3 (STE 0 = %#x, config %#x)\n",
	       MAP_SIZE >> 20, dart_get_ste(0, 0), rd(dart, 0x0c));
	return 0;
}

/* --- the ISP's CPU ---------------------------------------------------------- */

static int wait_irq(const char *what, int tries)
{
	for (int i = 0; i < tries; i++) {
		if (rd(isp, ISP_IRQ_STATUS) & 0xf0)
			return 0;
		msleep(10);
	}
	fprintf(stderr, "no %s from the ISP's CPU (status %#x)\n", what, rd(isp, ISP_IRQ_STATUS));
	return -1;
}

static void mbox_dump(const char *when)
{
	printf("mailbox %s:", when);
	for (int i = 0; i < 8; i++)
		printf(" %08x", rd(isp, ISP_MBOX + 4 * i));
	printf("\n");
}

/* an address the firmware gives, relative to its own base, in the region */
static u32 fw_off(u32 rel)
{
	return FW_VA + rel;
}

/* --- the IPC channels (AppleH4CamIn's ring library, 0x807e4e14..0x807e5210) ---
 *
 * Every channel is a ring of 0x40-byte entries; bit 0 of word 0 says whose
 * the entry is.  Host-to-target channels (type 0) belong to the host while
 * it is 1: the host writes words 1-2 and then word 0 = an address with bit 0
 * clear, and the firmware hands the entry back (bit 0 set) when done.  The
 * firmware's channels (types 1 and 2) are the reverse: an entry with bit 0
 * clear is a message for the host, answered in place (type 1: words 1-2,
 * then word 0 = the answer | 1) or just released (type 2, TERMINAL).  A
 * doorbell, ISP 0x41020 = 1 << (source + 4), tells the firmware; it raises
 * the same bit in 0x41000 when it has posted something.
 */
struct chan {
	char name[17];
	u32 type, source, n, q;
	u32 idx;			/* next entry to read (theirs) or write (ours) */
};
static struct chan ch[32];
static u32 nchan;
static int c_terminal = -1, c_io = -1, c_sharedmalloc = -1, c_io_t2h = -1, c_buf_t2h = -1;
static int c_buf_h2t = -1;
/* the frames the firmware has handed back (BUF_T2H), newest last */
static u32 frames, frame_tag, frame_addr, frame_pool;

#define SHM_VA		0x800000	/* the firmware's SHAREDMALLOC requests, upwards */
#define CMD_OFF		0xc000		/* command packets, in the shared region */
static u32 shm_next = SHM_VA;

static u32 ent(struct chan *c, u32 i, u32 w) { return c->q + i * 0x40 + w * 4; }

static void doorbell(struct chan *c)
{
	wr(isp, ISP_IRQ_ACK2, 1u << (c->source + 4));
}

static void print_fw_string(u32 rel)
{
	u32 off = fw_off(rel);
	char s[256];
	int i;

	if (off >= MAP_SIZE)
		return;
	for (i = 0; i < 255 && off + i < MAP_SIZE; i++) {
		s[i] = *(volatile char *)(mem + off + i);
		if (!s[i])
			break;
	}
	s[i] = 0;
	while (i > 0 && (s[i - 1] == '\n' || s[i - 1] == '\r'))
		s[--i] = 0;
	printf("ISPCPU: %s\n", s);
}

/* a SHAREDMALLOC request: address 0 and a size is an allocation, else a free */
static void shared_malloc(u32 addr, u32 size, u32 tag, u32 *r)
{
	char t[5] = { tag, tag >> 8, tag >> 16, tag >> 24, 0 };

	if (addr) {
		printf("  firmware frees %#x\n", addr);
		*r = 0;
		return;
	}
	if (size < 0x1000)
		size = 0x1000;
	size = (size + 0xfff) & ~0xfffu;
	if (shm_next + size > MAP_SIZE) {
		printf("  firmware asks for %u bytes (%s): out of memory\n", size, t);
		*r = 0;
		return;
	}
	mzero(shm_next, size);
	*r = shm_next - FW_VA;
	printf("  firmware asks for %u bytes (%s): given at %#x\n", size, t, shm_next);
	shm_next += size;
}

/* everything the firmware has posted, answered */
static int pump(void)
{
	u32 st = rd(isp, ISP_IRQ_STATUS), handled = 0;

	if (st)
		wr(isp, ISP_IRQ_CLEAR, st);
	for (u32 i = 0; i < nchan; i++) {
		struct chan *c = &ch[i];

		if (c->type == 0)
			continue;
		while (!(mrd(ent(c, c->idx, 0)) & 1)) {
			u32 w0 = mrd(ent(c, c->idx, 0)), a1 = mrd(ent(c, c->idx, 1));
			u32 a2 = mrd(ent(c, c->idx, 2)), addr = w0 & ~3u, r = addr;

			handled++;
			if (c->type == 2) {			/* TERMINAL: print, release */
				print_fw_string(addr);
				mwr(ent(c, c->idx, 0), 1);
			} else {
				if ((int)i == c_sharedmalloc) {
					shared_malloc(addr, a1, a2, &r);
					a1 = a2 = 0;
				} else if ((int)i == c_buf_t2h && fw_off(addr) + 8 <= MAP_SIZE) {
					/* buffers back (iOS 0x807de324): type, count, then 10
					 * words a buffer -- planes 0-2, 0 x3, plane count,
					 * pool, our tag, 0 */
					u32 m = fw_off(addr), type = mrd(m), cnt = mrd(m + 4);

					printf("  buffers back: type %u, %u of them\n", type, cnt);
					for (u32 k = 0; k < cnt && k < 16; k++) {
						u32 b = m + 8 + k * 40;

						printf("    pool %u tag %#x plane0 %#x planes %u\n",
						       mrd(b + 28) & 0xfffffff, mrd(b + 32), mrd(b), mrd(b + 24));
						if (type == 1 || type == 2) {
							frames++;
							frame_tag = mrd(b + 32);
							frame_addr = mrd(b);
							frame_pool = mrd(b + 28) & 0xfffffff;
						}
					}
				} else if ((int)i == c_io_t2h) {
					printf("  firmware notification at %#x: opcode %#x\n", addr,
					       addr + 8 <= MAP_SIZE ? mrd(fw_off(addr) + 4) & 0xffff : 0);
				} else {
					printf("  %s message: %#x %#x %#x\n", c->name, addr, a1, a2);
				}
				mwr(ent(c, c->idx, 1), a1);
				mwr(ent(c, c->idx, 2), a2);
				mwr(ent(c, c->idx, 0), r | 1);
				doorbell(c);
			}
			c->idx = (c->idx + 1) % c->n;
		}
	}
	return handled;
}

/*
 * A command (iOS's ispCmdSend, 0x807e01a0): the packet -- u16 opcode at +4,
 * u16 result at +6, parameters from +8 -- copied into the shared region, its
 * address and length posted on IO, the doorbell rung; the firmware answers in
 * the packet and hands the entry back.
 */
static int send_cmd(const char *what, uint8_t *pkt, u32 len, int timeout_ms)
{
	struct chan *c = &ch[c_io];
	u32 e = c->idx, off = POOL_VA + CMD_OFF, res;
	int t;

	if (c_io < 0 || !(mrd(ent(c, e, 0)) & 1)) {
		fprintf(stderr, "%s: no free IO entry\n", what);
		return -1;
	}
	for (u32 i = 0; i < len; i += 4)
		mwr(off + i, *(u32 *)(pkt + i));
	mwr(ent(c, e, 1), len);
	mwr(ent(c, e, 2), len);
	mwr(ent(c, e, 0), off - FW_VA);
	doorbell(c);
	c->idx = (c->idx + 1) % c->n;
	for (t = 0; t < timeout_ms; t += 10) {
		pump();
		if (mrd(ent(c, e, 0)) & 1)
			break;
		msleep(10);
	}
	if (t >= timeout_ms) {
		fprintf(stderr, "%s: no answer in %d ms\n", what, timeout_ms);
		return -1;
	}
	for (u32 i = 0; i < len; i += 4)
		*(u32 *)(pkt + i) = mrd(off + i);
	res = *(uint16_t *)(pkt + 6);
	printf("%s (opcode %#x): %s (%#x), %d ms\n", what, *(uint16_t *)(pkt + 4),
	       res ? "error" : "done", res, t);
	return res ? -1 : 0;
}

/*
 * Buffers to the firmware (iOS's ISP_SendBuffers, 0x807e05a0): a 0x180-byte
 * message in the shared region -- type 1, the count, then 10 words a buffer:
 * plane addresses 0-2 (the firmware's), 0 x3, the plane count, the pool,
 * a tag of ours, 0 -- posted on BUF_H2T as (address, 0x180, 0x30000000).
 */
#define MSG_OFF		0xd000
static int send_buffers(u32 pool, const u32 *addr, u32 n, u32 tag0)
{
	struct chan *c = &ch[c_buf_h2t];
	u32 m = POOL_VA + MSG_OFF, e, t;

	if (c_buf_h2t < 0 || n > 9)
		return -1;
	e = c->idx;
	if (!(mrd(ent(c, e, 0)) & 1)) {
		fprintf(stderr, "no free BUF_H2T entry\n");
		return -1;
	}
	mzero(m, 0x180);
	mwr(m, 1);
	mwr(m + 4, n);
	for (u32 i = 0; i < n; i++) {
		u32 b = m + 8 + i * 40;

		mwr(b, addr[i] - FW_VA);
		mwr(b + 24, 1);
		mwr(b + 28, pool);
		mwr(b + 32, tag0 + i);
	}
	mwr(ent(c, e, 1), 0x180);
	mwr(ent(c, e, 2), 0x30000000);
	mwr(ent(c, e, 0), m - FW_VA);
	doorbell(c);
	c->idx = (c->idx + 1) % c->n;
	for (t = 0; t < 2000; t += 10) {
		pump();
		if (mrd(ent(c, e, 0)) & 1)
			break;
		msleep(10);
	}
	printf("buffers sent: %u to pool %u, %s\n", n, pool, t < 2000 ? "taken" : "not taken in 2 s");
	return t < 2000 ? 0 : -1;
}

static void hexdump(const uint8_t *p, u32 len)
{
	for (u32 i = 0; i < len; i += 16) {
		printf("    %03x:", i);
		for (u32 k = i; k < i + 16 && k < len; k += 4)
			printf(" %08x", *(u32 *)(p + k));
		printf("\n");
	}
}

static int command(const char *arg)
{
	uint8_t pkt[0x100];

	memset(pkt, 0, sizeof(pkt));
	if (!strcmp(arg, "start")) {			/* CISP_CMD_START */
		*(uint16_t *)(pkt + 4) = 0;
		return send_cmd("START", pkt, 8, 2000);
	}
	if (!strcmp(arg, "print")) {			/* CISP_CMD_PRINT_ENABLE */
		*(uint16_t *)(pkt + 4) = 4;
		*(u32 *)(pkt + 8) = 1;
		return send_cmd("PRINT_ENABLE", pkt, 0xc, 2000);
	}
	if (!strncmp(arg, "info:", 5)) {		/* the sensor query of ISP_Init */
		*(uint16_t *)(pkt + 4) = 0x10d;
		*(u32 *)(pkt + 8) = atoi(arg + 5);
		if (send_cmd("CH_INFO", pkt, 0xa8, 5000) == 0)
			hexdump(pkt, 0xa8);
		return 0;
	}
	if (!strncmp(arg, "grab:", 5)) {
		/*
		 * One frame, iOS's sequence (H4ISPDevice 0x2928af80, StartCamera
		 * 0x2928b200, StopCamera 0x2928b2b0): CONFIG_SELECT, CROP_SET,
		 * OUTPUT_CONFIG_SET (format 1: one plane, 2 bytes a pixel), buffer
		 * recycling on, buffers, CH_START; the first frame back is saved.
		 */
		u32 chn = 0, mode = 0, w = 640, h = 480, pool = 0, want = 1, got = 0, sw, sh, stride, size;
		u32 buf[4];
		char path[64];
		FILE *f;
		int t;

		sscanf(arg + 5, "%u:%u:%u:%u:%u:%u", &chn, &mode, &w, &h, &pool, &want);
		*(uint16_t *)(pkt + 4) = 0x106;
		*(u32 *)(pkt + 8) = chn;
		*(u32 *)(pkt + 0xc) = mode;
		if (send_cmd("CAMERA_CONFIG_GET", pkt, 0x60, 5000))
			return -1;
		sw = *(uint16_t *)(pkt + 0x10);
		sh = *(uint16_t *)(pkt + 0x12);
		if (w > sw || h > sh)
			w = sw & ~15u, h = sh & ~1u;
		stride = (w * 2 + 15) & ~15u;
		size = (stride * h + 0xfff) & ~0xfffu;
		printf("channel %u mode %u: sensor %ux%u, output %ux%u, stride %u\n", chn, mode, sw, sh,
		       w, h, stride);

		memset(pkt, 0, sizeof(pkt));
		*(uint16_t *)(pkt + 4) = 0x107;			/* CONFIG_SELECT */
		*(u32 *)(pkt + 8) = chn;
		*(u32 *)(pkt + 0xc) = mode;
		if (send_cmd("CONFIG_SELECT", pkt, 0x10, 5000))
			return -1;
		memset(pkt, 0, sizeof(pkt));
		*(uint16_t *)(pkt + 4) = 0x801;			/* CROP_SET: x, y, w, h */
		*(u32 *)(pkt + 8) = chn;
		*(u32 *)(pkt + 0xc) = (sw - w) / 2;
		*(u32 *)(pkt + 0x10) = (sh - h) / 2;
		*(u32 *)(pkt + 0x14) = w;
		*(u32 *)(pkt + 0x18) = h;
		send_cmd("CROP_SET", pkt, 0x1c, 5000);
		memset(pkt, 0, sizeof(pkt));
		*(uint16_t *)(pkt + 4) = 0xb01;			/* OUTPUT_CONFIG_SET */
		*(u32 *)(pkt + 8) = chn;
		*(u32 *)(pkt + 0xc) = w;
		*(u32 *)(pkt + 0x10) = h;
		*(u32 *)(pkt + 0x14) = 1;			/* range, as iOS's ^1 of a 0 flag */
		*(u32 *)(pkt + 0x18) = 1;			/* format 1 */
		*(u32 *)(pkt + 0x1c) = stride;
		if (send_cmd("OUTPUT_CONFIG_SET", pkt, 0x28, 5000))
			return -1;
		memset(pkt, 0, sizeof(pkt));
		*(uint16_t *)(pkt + 4) = 0x10e;			/* BUFFER_RECYCLE_MODE_SET 1 */
		*(u32 *)(pkt + 8) = chn;
		*(u32 *)(pkt + 0xc) = 1;
		send_cmd("BUFFER_RECYCLE_MODE_SET", pkt, 0x10, 5000);
		memset(pkt, 0, sizeof(pkt));
		*(uint16_t *)(pkt + 4) = 0x10f;			/* BUFFER_RECYCLE_START */
		*(u32 *)(pkt + 8) = chn;
		send_cmd("BUFFER_RECYCLE_START", pkt, 0xc, 5000);

		for (int i = 0; i < 4; i++) {
			if (shm_next + size > MAP_SIZE)
				return -1;
			buf[i] = shm_next;
			mzero(buf[i], size);
			shm_next += size;
		}
		if (send_buffers(pool, buf, 4, 0x1000))
			return -1;
		frames = 0;
		memset(pkt, 0, sizeof(pkt));
		*(uint16_t *)(pkt + 4) = 0x100;			/* CH_START */
		*(u32 *)(pkt + 8) = chn;
		if (send_cmd("CH_START", pkt, 0xc, 5000))
			return -1;
		/* frames: each one handed straight back until the last, so the
		 * firmware's exposure settles; the last is kept */
		for (t = 0; t < 500 + 10 * (int)want && got < want; t++) {
			pump();
			if (frames) {
				got += frames;
				frames = 0;
				if (got < want) {
					u32 again = fw_off(frame_addr);

					send_buffers(pool, &again, 1, frame_tag);
				}
			}
			msleep(10);
		}
		frames = got;
		printf("%u frames in %d ms\n", got, t * 10);
		if (frames) {
			u32 off = fw_off(frame_addr);

			printf("frame: tag %#x pool %u at %#x after %d ms\n", frame_tag, frame_pool, off,
			       t * 10);
			snprintf(path, sizeof(path), "/root/isp/frame-ch%u.yuv", chn);
			f = fopen(path, "wb");
			if (f && off + stride * h <= MAP_SIZE) {
				for (u32 y = 0; y < h; y++) {
					uint8_t line[4096];

					for (u32 x = 0; x < stride && x < sizeof(line); x += 4)
						*(u32 *)(line + x) = mrd(off + y * stride + x);
					fwrite(line, 1, w * 2, f);
				}
				fclose(f);
				printf("saved %ux%u, 2 bytes a pixel, to %s\n", w, h, path);
			}
		} else {
			printf("no frame in 5 s\n");
			/* what the camera's receiver saw meanwhile */
			memset(pkt, 0, sizeof(pkt));
			*(uint16_t *)(pkt + 4) = 0x121;		/* CAMERA_ERR_COUNT_GET */
			*(u32 *)(pkt + 8) = chn;
			if (send_cmd("CAMERA_ERR_COUNT_GET", pkt, 0x40, 5000) == 0)
				hexdump(pkt, 0x40);
		}
		memset(pkt, 0, sizeof(pkt));
		*(uint16_t *)(pkt + 4) = 0x101;			/* CH_STOP */
		*(u32 *)(pkt + 8) = chn;
		send_cmd("CH_STOP", pkt, 0xc, 5000);
		memset(pkt, 0, sizeof(pkt));
		*(uint16_t *)(pkt + 4) = 0x104;			/* CH_BUFFER_RETURN */
		*(u32 *)(pkt + 8) = chn;
		send_cmd("CH_BUFFER_RETURN", pkt, 0xc, 5000);
		return frames ? 0 : -1;
	}
	if (!strncmp(arg, "setfile:", 8)) {
		/* a sensor's set-file (iOS's ISP_Init: CH_SET_FILE_LOAD 0x111 -- the
		 * channel, its address, its size), loaded into the region */
		u32 chn = atoi(arg + 8), size, at;
		const char *path = strchr(arg + 8, ':');
		static uint8_t sf[0x40000];
		FILE *f;

		if (!path || !(f = fopen(path + 1, "rb"))) {
			fprintf(stderr, "setfile: no file\n");
			return -1;
		}
		size = fread(sf, 1, sizeof(sf), f);
		fclose(f);
		at = shm_next;
		if (at + ((size + 0xfff) & ~0xfffu) > MAP_SIZE)
			return -1;
		shm_next += (size + 0xfff) & ~0xfffu;
		for (u32 i = 0; i < size; i += 4)
			mwr(at + i, *(u32 *)(sf + i));
		*(uint16_t *)(pkt + 4) = 0x111;
		*(u32 *)(pkt + 8) = chn;
		*(u32 *)(pkt + 0xc) = at - FW_VA;
		*(u32 *)(pkt + 0x10) = size;
		printf("set-file for channel %u: %u bytes at %#x\n", chn, size, at);
		return send_cmd("CH_SET_FILE_LOAD", pkt, 0x14, 5000);
	}
	if (!strncmp(arg, "cmd:", 4)) {		/* cmd:OPCODE:LEN:W2:W3:W4:W5 (hex), the answer dumped */
		u32 op = 0, len = 0x10, wv[4] = { 0, 0, 0, 0 };

		sscanf(arg + 4, "%x:%x:%x:%x:%x:%x", &op, &len, &wv[0], &wv[1], &wv[2], &wv[3]);
		if (len < 8 || len > sizeof(pkt))
			len = 0x10;
		*(uint16_t *)(pkt + 4) = op;
		for (int i = 0; i < 4; i++)
			if (8 + 4 * (u32)i + 4 <= len)
				*(u32 *)(pkt + 8 + 4 * i) = wv[i];
		if (send_cmd("cmd", pkt, len, 5000) == 0)
			hexdump(pkt, len);
		return 0;
	}
	if (!strncmp(arg, "modes:", 6)) {		/* every mode of a channel (iOS: 0x106 each) */
		u32 chn = atoi(arg + 6), n;

		*(uint16_t *)(pkt + 4) = 0x10d;
		*(u32 *)(pkt + 8) = chn;
		if (send_cmd("CH_INFO", pkt, 0xa8, 5000))
			return -1;
		n = *(u32 *)(pkt + 0x60);
		printf("channel %u: sensor %#x, %u modes\n", chn, *(u32 *)(pkt + 0x20), n);
		for (u32 m = 0; m < n && m < 16; m++) {
			memset(pkt, 0, sizeof(pkt));
			*(uint16_t *)(pkt + 4) = 0x106;
			*(u32 *)(pkt + 8) = chn;
			*(u32 *)(pkt + 0xc) = m;
			if (send_cmd("CAMERA_CONFIG_GET", pkt, 0x60, 5000) == 0)
				hexdump(pkt, 0x60);
		}
		return 0;
	}
	if (!strncmp(arg, "wait:", 5)) {
		for (int t = 0; t < atoi(arg + 5) * 100; t++) {
			pump();
			msleep(10);
		}
		return 0;
	}
	fprintf(stderr, "unknown ISP command %s\n", arg);
	return -1;
}

static int load_firmware(const char *path)
{
	FILE *f = fopen(path, "rb");
	static uint8_t buf[FW_SIZE];
	size_t n;
	u32 sum = 0;

	if (!f) {
		perror(path);
		return -1;
	}
	n = fread(buf, 1, sizeof(buf), f);
	fclose(f);
	if (n < 0x2000 || memcmp(buf + 0x1040, "adc", 3)) {
		fprintf(stderr, "%s: %zu bytes, no 'adc' at 0x1040: not the H4ISP firmware?\n", path, n);
		return -1;
	}
	memset(buf + n, 0, sizeof(buf) - n);
	for (size_t i = 0; i < n; i++)
		sum += buf[i];
	for (u32 i = 0; i < FW_SIZE; i += 4)
		mwr(FW_VA + i, *(u32 *)(buf + i));
	for (u32 i = 0; i < FW_SIZE; i += 4)
		if (mrd(FW_VA + i) != *(u32 *)(buf + i)) {
			fprintf(stderr, "firmware copy differs at %#x\n", i);
			return -1;
		}
	printf("firmware: %zu bytes, checksum %#x (iOS logs the same sum)\n", n, sum);
	return 0;
}

static int boot(const char *fwpath)
{
	u32 nch, qsize, heap, tbl, q_off, d_off;

	/* from scratch every time: a running ISP CPU stopped and powered down first */
	if ((rd(pmgr, PS_HPERF_ISP) & 0xf) == 0xf) {
		wr(isp, ISP_RUN, 0);
		if (ps_set(PS_HPERF_ISP, 0))
			return 1;
	}
	if (ps_set(PS_HPERF_ISP, 1))
		return 1;
	ps_reset(PS_HPERF_ISP);
	sensors(1);
	printf("power: HPERF-ISP %#x, ISP %#x %#x\n", rd(pmgr, PS_HPERF_ISP),
	       rd(isp, 0x40000), rd(isp, ISP_RUN));
	wr(isp, ISP_RUN, 0);
	if (dart_init())
		return 1;
	if (load_firmware(fwpath))
		return 1;
	mzero(POOL_VA, POOL_SIZE);

	for (int i = 0; i < 8; i++)
		wr(isp, ISP_MBOX + 4 * i, 0);
	wr(isp, ISP_IRQ_CLEAR, 0xffffffff);
	wr(isp, ISP_MIPI0, 0x1201);
	wr(isp, ISP_MIPI1, 0x1201);
	wr(isp, ISP_BOOT, FW_VA | 0x80000000u);
	wr(isp, ISP_RUN, 1);
	printf("started: boot %#x, run %#x\n", rd(isp, ISP_BOOT), rd(isp, ISP_RUN));

	if (wait_irq("wake signal", 1000))
		goto fail;
	wr(isp, ISP_IRQ_CLEAR, 0xffffffff);
	mbox_dump("at wake");
	nch = rd(isp, ISP_MBOX);
	qsize = rd(isp, ISP_MBOX + 4) + 1;
	heap = rd(isp, ISP_MBOX + 12);
	printf("the ISP's CPU is up: %u IPC channels, queue memory %u bytes, heap wanted %u bytes\n",
	       nch, qsize, heap);
	if (nch > 32 || qsize > POOL_SIZE - 0x100 || heap > HEAP_MAX) {
		fprintf(stderr, "out of iOS's limits; stopping here\n");
		goto fail;
	}

	/* iOS's answer: the queues and a descriptor in the shared region, the heap */
	q_off = 0;
	d_off = (qsize + 0xff) & ~0xffu;
	mzero(HEAP_VA, HEAP_MAX);
	wr(isp, ISP_MBOX + 0x00, FW_VA);
	wr(isp, ISP_MBOX + 0x04, (POOL_VA - FW_VA) + q_off);
	wr(isp, ISP_MBOX + 0x08, FW_SIZE);
	wr(isp, ISP_MBOX + 0x0c, 0x10000000 - FW_SIZE);
	wr(isp, ISP_MBOX + 0x10, heap ? HEAP_VA - FW_VA : 0);
	wr(isp, ISP_MBOX + 0x14, heap ? (heap < 0x1000 ? 0x1000 : heap) : 0);
	wr(isp, ISP_MBOX + 0x18, SENSOR_TYPE);
	mwr(POOL_VA + d_off + 0x00, 5);
	for (int i = 4; i < 0x18; i += 4)
		mwr(POOL_VA + d_off + i, 0);	/* boot arg, flags, no log buffers */
	wr(isp, ISP_MBOX + 0x1c, (POOL_VA + d_off) - FW_VA);
	mbox_dump("answered");
	wr(isp, ISP_IRQ_ACK2, 0x10);

	if (wait_irq("second interrupt (channel table)", 1000))
		goto fail;
	wr(isp, ISP_IRQ_CLEAR, 0xffffffff);
	mbox_dump("table ready");
	tbl = fw_off(rd(isp, ISP_MBOX));
	if (tbl + nch * 0x100 > MAP_SIZE) {
		fprintf(stderr, "channel table at %#x, outside the region\n", tbl);
		goto fail;
	}
	for (u32 i = 0; i < nch; i++) {
		u32 e = tbl + i * 0x100, q = fw_off(mrd(e + 0x4c)), n = mrd(e + 0x48);
		char name[0x41];

		for (int k = 0; k < 0x40; k++)
			name[k] = *(volatile char *)(mem + e + k);
		name[0x40] = 0;
		printf("  channel %u: %-14s type %u source %u size %u queue at %#x\n", i, name,
		       mrd(e + 0x40), mrd(e + 0x44), n, q);
		if (i < 32) {
			struct chan *c = &ch[i];

			snprintf(c->name, sizeof(c->name), "%.16s", name);
			c->type = mrd(e + 0x40);
			c->source = mrd(e + 0x44);
			c->n = n;
			c->q = q;
			c->idx = 0;
			if (!strcmp(name, "TERMINAL")) c_terminal = i;
			if (!strcmp(name, "IO")) c_io = i;
			if (!strcmp(name, "SHAREDMALLOC")) c_sharedmalloc = i;
			if (!strcmp(name, "IO_T2H")) c_io_t2h = i;
			if (!strcmp(name, "BUF_T2H")) c_buf_t2h = i;
			if (!strcmp(name, "BUF_H2T")) c_buf_h2t = i;
			nchan = i + 1;
		}
		/* the host's own queues (type 0): every 64-byte entry state 1, as
		 * iOS's channel set-up (0x807e4e14) leaves them; the firmware sets
		 * up the others */
		if (mrd(e + 0x40) == 0 && q + n * 0x40 <= MAP_SIZE)
			for (u32 k = 0; k < n; k++) {
				mwr(q + k * 0x40, 1);
				mwr(q + k * 0x40 + 4, 0);
				mwr(q + k * 0x40 + 8, 0);
			}
	}

	wr(isp, ISP_MBOX + 0x0c, MAGIC_READY);
	for (int i = 0; i < 1000 && rd(isp, ISP_MBOX + 0x0c); i++)
		msleep(10);
	if (rd(isp, ISP_MBOX + 0x0c)) {
		fprintf(stderr, "the firmware did not take the ready magic (%#x)\n",
			rd(isp, ISP_MBOX + 0x0c));
		goto fail;
	}
	wr(isp, ISP_IRQ_ENABLE, 0xf8);
	printf("ISP ready: the firmware runs and took the handshake (irq status %#x)\n",
	       rd(isp, ISP_IRQ_STATUS));
	return 0;
fail:
	mbox_dump("at failure");
	printf("firmware's first words: %08x %08x %08x %08x\n", mrd(FW_VA), mrd(FW_VA + 4),
	       mrd(FW_VA + 8), mrd(FW_VA + 12));
	printf("IOMMU: cmd %#x err %#x %#x\n", rd(dart, 0), rd(dart, 0x10), rd(dart, 0x14));
	return 1;
}

static int off(void)
{
	sensors(0);
	if ((rd(pmgr, PS_HPERF_ISP) & 0xf) == 0xf)
		wr(isp, ISP_RUN, 0);
	return ps_set(PS_HPERF_ISP, 0) ? 1 : 0;
}

/* a stretch of the region: hex, and its text as the firmware writes it */
static int dump(u32 off, u32 len)
{
	if (off >= MEM_SIZE || len > MEM_SIZE - off)
		return 1;
	for (u32 i = 0; i < len; i += 16) {
		char txt[17];
		int blank = 1;

		for (int k = 0; k < 16; k++) {
			uint8_t c = *(volatile uint8_t *)(mem + off + i + k);

			txt[k] = c >= 0x20 && c < 0x7f ? c : '.';
			blank &= !c;
		}
		txt[16] = 0;
		if (blank)
			continue;
		printf("%07x:", off + i);
		for (int k = 0; k < 16; k += 4)
			printf(" %08x", mrd(off + i + k));
		printf("  %s\n", txt);
	}
	return 0;
}

static int regs(void)
{
	u32 ps = rd(pmgr, PS_HPERF_ISP);

	printf("HPERF-ISP %#x (%s)\n", ps, (ps & 0xf) == 0xf ? "on" : "off");
	if ((ps & 0xf) != 0xf)
		return 0;
	printf("ISP: %#x run %#x boot %#x irq %#x/%#x\n", rd(isp, 0x40000), rd(isp, ISP_RUN),
	       rd(isp, ISP_BOOT), rd(isp, ISP_IRQ_STATUS), rd(isp, ISP_IRQ_ENABLE));
	mbox_dump("now");
	printf("IOMMU: cmd %#x config %#x err %#x %#x target %#x\n", rd(dart, 0), rd(dart, 0x0c),
	       rd(dart, 0x10), rd(dart, 0x14), rd(dart, 0x18));
	return 0;
}

/*
 * The region must be the DT's reserved one: without it, MEM_BASE is the
 * kernel's own RAM.  /proc/iomem lists RAM the kernel uses as "System RAM";
 * a no-map reservation is left out of it.
 */
static int region_reserved(void)
{
	FILE *f = fopen("/proc/iomem", "r");
	char line[256];
	unsigned long a, b;
	int ok = 1;

	if (!f) {
		perror("/proc/iomem");
		return 0;
	}
	while (fgets(line, sizeof(line), f))
		if (line[0] != ' ' && strstr(line, "System RAM") &&
		    sscanf(line, "%lx-%lx", &a, &b) == 2 &&
		    a < MEM_BASE + MEM_SIZE && b >= MEM_BASE)
			ok = 0;
	fclose(f);
	if (!ok)
		fprintf(stderr, "%#x-%#x is the kernel's RAM here: this kernel's DT has no isp "
			"reserved region\n", MEM_BASE, MEM_BASE + MEM_SIZE - 1);
	return ok;
}

int main(int argc, char **argv)
{
	int fd;

	if (argc < 2 || (!strcmp(argv[1], "boot") && argc < 3)) {
		fprintf(stderr, "usage: k93isp boot FIRMWARE | run FIRMWARE [start print info:N wait:S off]... | off | regs | dump OFF LEN\n");
		return 2;
	}
	if (!region_reserved())
		return 1;
	fd = open("/dev/mem", O_RDWR | O_SYNC);
	if (fd < 0) {
		perror("/dev/mem");
		return 1;
	}
	pmgr = map(fd, PMGR, 0x7000);
	isp = map(fd, ISP_BASE, 0x100000);
	dart = map(fd, DART_BASE, 0x2000);
	gpio = map(fd, GPIO_BASE, 0x1000);
	mem = map(fd, MEM_BASE, MEM_SIZE);
	if (!strcmp(argv[1], "boot"))
		return boot(argv[2]);
	if (!strcmp(argv[1], "run") && argc > 2) {
		if (boot(argv[2]))
			return 1;
		for (int i = 3; i < argc; i++) {
			if (!strcmp(argv[i], "off"))
				return off();
			if (command(argv[i]))
				printf("(command %s failed; going on)\n", argv[i]);
		}
		pump();
		return 0;
	}
	if (!strcmp(argv[1], "off"))
		return off();
	if (!strcmp(argv[1], "regs"))
		return regs();
	if (!strcmp(argv[1], "dump") && argc > 3)
		return dump(strtoul(argv[2], NULL, 16), strtoul(argv[3], NULL, 16));
	fprintf(stderr, "unknown command %s\n", argv[1]);
	return 2;
}
