/* k93-z2-boot -- bring up the iPad2,1 (K93) digitizer the way iOS 8.4.1 does.
 *
 * Port of z2-boot.c (P105).  The HBPP transport, firmware and calibration
 * downloads, EXECUTE packet and post-boot dialogue are unchanged; what differs
 * is K93's bootloader class and addresses.
 *
 * K93 binds "AppleMultitouchSPI - Z2,K94" (IOClass AppleMultitouchZ2SPI,
 * mt-merge-personality Z2F13,2).  The personality sets only cal-dl-addr
 * 0x1000c000 and fll-mval 0x18da; every other address is the
 * MTSPIBootloader_Z2 default.  From the 12H321 K93 kernelcache, multitouch
 * kext at VA 0x806e5000, Z2 vtable at 0x806f4328:
 *
 *   defaults (slot +0x74, 0x806ef7e4)   fll-mval 0x16e4, fll-mval-addr
 *                                        0x10001c04, cal-dl-addr 0x00400200,
 *                                        fw-execute-addr 0x10001800,
 *                                        prox-cal-addr 0x00400180
 *   bootloadDevice (slot +0x60, 0x806ee978), in this order:
 *     firmware      (+0x80) the Constructed Firmware in one transfer, ATN_ACK
 *     prox cal      (+0x90) skipped when absent -- K93's ADT has none
 *     calibration   (+0x8c) "Calibration Data" to cal-dl-addr, DATA packets of
 *                           at most 0x3f0 bytes, each acked 4bc1
 *     performCalibSeq (+0x7c, 0x806eeaa0):
 *                   read  0x10008ffc                       version
 *                   unless version == 0x5a020028, write (addr <- value, mask):
 *                         fll-mval-addr <- fll-mval        mask 0x1fff
 *                         0x10001c08    <- 0x00840000      mask 0x00ff0000
 *                         0x1000300c    <- 5               mask 0x85
 *                         0x1000304c    <- 0x20            mask 0xffffffff
 *                   each must be acked 4ad1
 *                   send  1f 01 (request calibration), sleep 65 ms, ATN_ACK
 *     execute       (+0x4c, 0x806ef46c) 1d 53 <fw-execute-addr> <1> csum
 *     sleep 40 ms
 *
 * MTSPIBootloader_N1's sequence (P105: 0x10003060/5c/58/3000/3518, EXECUTE at
 * 0x10003400) does not apply to K93.
 *
 * HBPP (bootloader) packets, from AppleMultitouchZ2SPI:
 *   a 32-bit field is two big-endian halves, low half first; csum is the
 *   16-bit byte sum of the fields, big-endian
 *   ATN_ACK       1a a1                    -> 2-byte status, big-endian
 *   long ATN_ACK  1a a1 18 e1 18 e1 18 e1  -> 8 bytes, value in [2..5]
 *   HBPP check    1a a1 (18 e1) x7         -> first two words are status words
 *   reg read      1c 73 addr csum          -> then long ATN_ACK
 *   reg write     1e 33 addr mask val csum -> then ATN_ACK, 4ad1 = done
 *   data          18 e1 30 01 nwords(BE16) addr hcsum(BE16) words dcsum
 *                                          -> then ATN_ACK, 4bc1 = done
 *
 * Everything above and after EXECUTE is confirmed against iOS's own driver
 * trace on this K93 (tools/mtdump/mtlog through the user client, lock/unlock
 * re-bootload; touch/ios-mtlog-k93-1.txt): chip version 0x5a02002a, every
 * write acked 4ad1, calibration ack 4969 (not checked by iOS), EXECUTE
 * 1d 53 18 00 10 00 00 01 00 00 00 29.  Post-boot, iOS on K93 sends: e2 00
 * device info twice (e2 13 01 e2 0f: family 0x13, max packet 0xfe2), GET
 * d1 (short 1), d3 (long 12), d0 (short 8), a1 (short 6), d9 (short 8), then
 * SET bf = 9f 03 0b 02 and af = 00, twice.  Unlike P105 there is no ee 00 wake,
 * d9 is short, and bf differs (P105: 9b 0b 0b 02).  Frames match P105's
 * container and the kernel parser: [16] touch count, 30 bytes a touch from
 * [24], [1] state 3 down, 4 moving, 7 lift-off.
 *
 * Kernel side: /dev/hx-touch from drivers/input/touchscreen/apple-z2.c, with
 * K93's pins (reset 141, CS 159, ATTN 21) and supplies (LDO0, PMU GPIO0) in
 * dts/k93ap.dts.  The analog rail LDO15 (PMU 0x22 bit 0x02) is not in the
 * DTS; on P105 it was required to scan, and -M 22:02:02 switches it here.
 *
 *   k93-z2-boot [-c cal] [-P proxcal] [-F fll-mval] [-G pmu-gpio0] [-R] [-o m,m..] [-B] [-n] [-A] [-W] [-E] [-p secs] [-g usec] [-a] [-t secs] [-S] [-D] [-M reg:mask:val].. [-N] [mtprops]
 *     -c  calibration blob (iOS "Calibration Data", 1024 bytes) to 0x1000c000
 *     -P  prox calibration blob to 0x00400180 (K93 has none)
 *     -B  leave out the bf/af reports (the scan does not start)
 *     -n  skip performCalibSeq (control run)
 *     -A  also set report ab (rOPERATING_MODE) = 00
 *     -W  send REQ_WAKEUP (19 c1) after the boot ATTN
 *     -E  firmware download mode (download-gpios, HXT_IOC_SET_DL) from the
 *         HBPP check to EXECUTE, as iOS does; released at exit
 *     -R  after boot, list every report ID the chip answers (e3 id)
 *     -o  operating modes to try, e.g. -o 0,1: set report ab, read back, poll
 *     -F  fll-mval instead of the personality's 0x18da
 *     -G  write PMU 0x61 (GPIO0) over /dev/i2c-0 right after open
 *     -p  before READY, read frames for this many seconds (touch the glass)
 *     -a  poll: no transfer while ATTN is idle
 *     -g  CS settle in microseconds either side of each poll transfer (default 1000)
 *     -t  hold the device this many seconds after READY (default: forever)
 *     -S  mask ATTN's interrupt in the GPIO block and poll the pad instead
 *     -D  dump PMU 0x00..0x7f at each stage mark
 *     -M  reg:mask:val, hex, repeatable: PMU writes right after open
 *     -N  no READY: close the device after -p
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <linux/i2c.h>
#include <linux/i2c-dev.h>

#define HXT_IOC_SET_CS          _IOW('h', 1, uint32_t)
#define HXT_IOC_RESET           _IO('h', 2)
#define HXT_IOC_READY           _IO('h', 3)
#define HXT_IOC_SETUP_IRQ       _IO('h', 4)
#define HXT_IOC_WAIT_IRQ        _IOW('h', 5, uint32_t)
struct hxt_metrics { int left, right, top, bottom; };
#define HXT_IOC_METRICS         _IOW('h', 6, struct hxt_metrics)
#define HXT_IOC_SET_DL          _IOW('h', 7, uint32_t)

#define CHUNK   4096    /* one transfer stays under the SPI driver's 1 s timeout at 100 kHz */

/* MTSPIBootloader_Z2 defaults and the Z2,K94 personality (see header). */
#define FLL_MVAL        0x18da          /* personality */
#define FLL_MVAL_ADDR   0x10001c04
#define CAL_DL_ADDR     0x1000c000      /* personality */
#define FW_EXECUTE_ADDR 0x10001800
#define PROX_CAL_ADDR   0x00400180
#define Z2_VER_NO_WRITES 0x5a020028     /* performCalibSeq skips its writes */

static int fd = -1;
static uint8_t scratch[CHUNK];

static void hex(const char *tag, const uint8_t *p, size_t n)
{
    size_t i;
    printf("  %-16s", tag);
    for(i = 0; i < n && i < 16; i++)
        printf(" %02x", p[i]);
    printf("%s\n", n > 16 ? " ..." : "");
}

static void cs(int on)
{
    if(ioctl(fd, HXT_IOC_SET_CS, on) < 0)
        perror("SET_CS");
}

/* One chip-select window: n bytes out, n bytes in, cs_gap_us of CS settle on
 * either side.  The frame poll uses poll_gap_us instead (-g; iOS reads a
 * frame's data ~0.3 ms after its length); the bootload keeps 1 ms. */
static unsigned cs_gap_us = 1000, poll_gap_us = 1000;
/* -a: the poll sends nothing while ATTN is idle, as iOS reads only after
 * "host interrupt occurred"; a query between frames can put the chip's
 * output out of step with the host. */
static int attn_only;

static int xfer(const uint8_t *tx, uint8_t *rx, size_t n)
{
    size_t off = 0, c;

    cs(1);
    if(cs_gap_us)
        usleep(cs_gap_us);
    while(off < n) {
        c = n - off > CHUNK ? CHUNK : n - off;
        if(write(fd, tx + off, c) != (ssize_t)c) {
            perror("write");
            cs(0);
            return -1;
        }
        if(read(fd, rx ? rx + off : scratch, c) != (ssize_t)c) {
            perror("read");
            cs(0);
            return -1;
        }
        off += c;
    }
    cs(0);
    if(cs_gap_us)
        usleep(cs_gap_us);
    return 0;
}

static int safe, seen_high;
static volatile uint32_t *gpio;
static char attn_level(void);

#define GPIO_DISABLE0   (0x800 / 4)     /* pins 0..31, write 1 to disable */
#define GPIO_STATUS0    (0x880 / 4)     /* pins 0..31, write 1 to clear */
#define ATTN_PIN        21              /* K93 ADT multi-touch interrupt 0x15 */
#define ATTN_BIT        (1u << ATTN_PIN)

static void arm_attn(void)
{
    if(safe) {
        gpio[GPIO_STATUS0] = ATTN_BIT;
        seen_high = gpio[ATTN_PIN] & 1;
        return;
    }
    ioctl(fd, HXT_IOC_SETUP_IRQ);
}

/* ATTN's level straight from the pad: bit 0 of its pin config register
 * (0x3fa00000 + 4*ATTN_PIN), a plain read with no side effects.  '1' idle, '0'
 * asserted, '?' without /dev/mem. */
static volatile uint32_t *gpio;

static char attn_level(void)
{
    int m;

    if(!gpio) {
        m = open("/dev/mem", O_RDWR | O_SYNC);
        if(m < 0)
            return '?';
        gpio = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, m, 0x3fa00000);
        close(m);
        if(gpio == MAP_FAILED) {
            gpio = NULL;
            return '?';
        }
    }
    return gpio[ATTN_PIN] & 1 ? '1' : '0';
}

/* Firmware download mode, ADT multi-touch function-enable_download: GPIO
 * 0x1104 = pin 140, active low, the touch node's download-gpios.  iOS enables
 * it around the HBPP check and the bootload and disables it after EXECUTE;
 * apple-z2.c drives it through HXT_IOC_SET_DL (1 asserts) and releases it at
 * READY.  Released at exit in case the tool stops before EXECUTE. */
static int dl_active;

static void download_mode(int on)
{
    if(ioctl(fd, HXT_IOC_SET_DL, on) < 0) {
        perror("  download mode: SET_DL");
        return;
    }
    dl_active = on;
    printf("  download mode %s\n", on ? "on" : "off");
}

static void download_restore(void)
{
    if(dl_active)
        download_mode(0);
}

/* PMU (D1946, 0x3c on I2C0) register over /dev/i2c-0: the driver owns the
 * address, hence I2C_SLAVE_FORCE; a read is register write + read with a
 * repeated start, which is how this PMU wants it (i2c-s5l8940x.c). */
static int pmu_reg(unsigned reg, int val)
{
    struct i2c_rdwr_ioctl_data rw;
    struct i2c_msg m[2];
    uint8_t w[2] = { reg, val }, r = 0;
    int f = open("/dev/i2c-0", O_RDWR);

    if(f < 0 || ioctl(f, I2C_SLAVE_FORCE, 0x3c) < 0) {
        perror("/dev/i2c-0");
        if(f >= 0)
            close(f);
        return -1;
    }
    if(val >= 0) {
        m[0].addr = 0x3c; m[0].flags = 0; m[0].len = 2; m[0].buf = w;
        rw.msgs = m; rw.nmsgs = 1;
        if(ioctl(f, I2C_RDWR, &rw) < 0)
            perror("PMU write");
    }
    m[0].addr = 0x3c; m[0].flags = 0; m[0].len = 1; m[0].buf = w;
    m[1].addr = 0x3c; m[1].flags = I2C_M_RD; m[1].len = 1; m[1].buf = &r;
    rw.msgs = m; rw.nmsgs = 2;
    if(ioctl(f, I2C_RDWR, &rw) < 0) {
        perror("PMU read");
        close(f);
        return -1;
    }
    close(f);
    return r;
}

/* Seconds since start, and the PMU's switch registers, at each stage -- to
 * line up with a ping running on the Mac when something takes the network. */
static struct timespec t_start;
static int pmu_full;
static unsigned pmu_w[16][3], npmu_w;

static volatile uint32_t *map_ro(unsigned long pa)
{
    volatile uint32_t *p;
    int m = open("/dev/mem", O_RDONLY | O_SYNC);

    if(m < 0)
        return NULL;
    p = mmap(NULL, 4096, PROT_READ, MAP_SHARED, m, pa);
    close(m);
    return p == MAP_FAILED ? NULL : p;
}

/* The USB side (on P105 the scan start once stopped USB through an AIC
 * fault, since fixed): dwc2's own view (GOTGCTL, GINTSTS, GINTMSK, DSTS --
 * plain reads), the AIC's mask words, and what /proc/interrupts counts. */
static void usb_irq_report(void)
{
    static volatile uint32_t *otg, *aic;
    char line[256];
    FILE *f;

    if(!otg)
        otg = map_ro(0x36100000);
    if(!aic)
        aic = map_ro(0x3f204000);
    if(otg)
        printf("   dwc2 otgctl %08x gintsts %08x gintmsk %08x dsts %08x\n",
               (unsigned)otg[0x000 / 4], (unsigned)otg[0x014 / 4], (unsigned)otg[0x018 / 4],
               (unsigned)otg[0x808 / 4]);
    if(aic)
        printf("   aic mask %08x %08x %08x %08x\n", (unsigned)aic[0x100 / 4], (unsigned)aic[0x104 / 4],
               (unsigned)aic[0x108 / 4], (unsigned)aic[0x10c / 4]);
    f = fopen("/proc/interrupts", "r");
    if(!f)
        return;
    printf("   irq:");
    while(fgets(line, sizeof(line), f)) {
        char *name;
        unsigned n, cnt;
        if(sscanf(line, " %u: %u", &n, &cnt) != 2)
            continue;
        name = strrchr(line, ' ');
        if(!strstr(line, "usb") && !strstr(line, "spi") && !strstr(line, "gpio") &&
           !strstr(line, "z2") && !strstr(line, "i2c"))
            continue;
        if(name)
            name[strcspn(name, "\n")] = 0;
        printf(" %u=%u%s", n, cnt, name ? name : "");
    }
    printf("\n");
    fclose(f);
}

static void stamp(const char *what)
{
    struct timespec t;
    unsigned r;
    int v;

    clock_gettime(CLOCK_MONOTONIC, &t);
    printf("== [t+%.2f s] %s  PMU 20..27:", (t.tv_sec - t_start.tv_sec) + (t.tv_nsec - t_start.tv_nsec) / 1e9, what);
    for(r = 0x20; r < 0x28; r++) {
        v = pmu_reg(r, -1);
        if(v < 0)
            printf(" --");
        else
            printf(" %02x", v);
    }
    printf("\n");
    if(pmu_full && strcmp(what, "polling")) {
        for(r = 0; r < 0x80; r++) {
            if(r % 16 == 0)
                printf("   PMU %02x:", r);
            v = pmu_reg(r, -1);
            printf(v < 0 ? " --" : " %02x", v);
            if(r % 16 == 15)
                printf("\n");
        }
    }
    usb_irq_report();
}

/* 1 if ATTN arrived within ms.  -S: the pad's latch, or a high-to-low seen
 * by polling -- the chip holds ATTN low for milliseconds, polling is ~100 us. */
static int wait_attn(unsigned ms)
{
    struct timespec t0, t1;

    if(!safe)
        return ioctl(fd, HXT_IOC_WAIT_IRQ, ms) == 0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for(;;) {
        if(gpio[GPIO_STATUS0] & ATTN_BIT) {
            gpio[GPIO_STATUS0] = ATTN_BIT;
            return 1;
        }
        if(gpio[ATTN_PIN] & 1)
            seen_high = 1;
        else if(seen_high)
            return 1;
        clock_gettime(CLOCK_MONOTONIC, &t1);
        if((t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000 >= (long)ms)
            return 0;
        usleep(100);
    }
}

/* ---- HBPP ------------------------------------------------------------ */

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = v >> 8; p[1] = v; p[2] = v >> 24; p[3] = v >> 16;
}

static uint16_t sum16(const uint8_t *p, size_t n)
{
    uint16_t s = 0;
    while(n--)
        s += *p++;
    return s;
}

static int atn_ack(uint16_t *st)
{
    uint8_t tx[2] = { 0x1a, 0xa1 }, rx[2];
    if(xfer(tx, rx, 2))
        return -1;
    *st = rx[0] << 8 | rx[1];
    return 0;
}

static int long_atn_ack(uint32_t *v)
{
    uint8_t tx[8] = { 0x1a, 0xa1, 0x18, 0xe1, 0x18, 0xe1, 0x18, 0xe1 }, rx[8];
    if(xfer(tx, rx, 8))
        return -1;
    *v = (uint32_t)rx[4] << 24 | (uint32_t)rx[5] << 16 | (uint32_t)rx[2] << 8 | rx[3];
    return 0;
}

static int is_status_word(uint16_t w)
{
    return w == 0x18e1 || w == 0x1aa1 || w == 0x1f01 || w == 0x4879 ||
           w == 0x4969 || w == 0x4ad1 || w == 0x4bc1;
}

static int hbpp_check(void)
{
    uint8_t tx[16] = { 0x1a, 0xa1 }, rx[16];
    int i;
    for(i = 2; i < 16; i += 2) {
        tx[i] = 0x18;
        tx[i + 1] = 0xe1;
    }
    if(xfer(tx, rx, 16))
        return 0;
    hex("HBPP check ->", rx, 16);
    return is_status_word(rx[0] << 8 | rx[1]) && is_status_word(rx[2] << 8 | rx[3]);
}

static int reg_read(uint32_t addr, uint32_t *v)
{
    uint8_t p[8] = { 0x1c, 0x73 };
    uint16_t c;
    int attn;

    put32(p + 2, addr);
    c = sum16(p + 2, 4);
    p[6] = c >> 8; p[7] = c;
    arm_attn();
    if(xfer(p, NULL, 8))
        return -1;
    attn = wait_attn(20);
    if(long_atn_ack(v))
        return -1;
    printf("  read  %08x = %08x%s\n", addr, *v, attn ? "" : "   (no ATTN)");
    return 0;
}

static int reg_write(uint32_t addr, uint32_t val, uint32_t mask)
{
    uint8_t p[16] = { 0x1e, 0x33 };
    uint16_t c, st;
    int attn;

    put32(p + 2, addr);
    put32(p + 6, mask);
    put32(p + 10, val);
    c = sum16(p + 2, 12);
    p[14] = c >> 8; p[15] = c;
    arm_attn();
    if(xfer(p, NULL, 16))
        return -1;
    attn = wait_attn(20);
    if(atn_ack(&st))
        return -1;
    printf("  write %08x <- %08x (mask %08x): ack %04x%s%s\n", addr, val, mask, st,
           st == 0x4ad1 ? "" : "   NOT 4ad1", attn ? "" : "   (no ATTN)");
    return st == 0x4ad1 ? 0 : -1;
}

/* A blob into the chip's memory the way MTSPIBootloader_Z2 sends calibration
 * (0x80607f04): DATA packets of at most 0x3f0 bytes, each answered by
 * ATN_ACK 4bc1, up to five tries per packet.  The length is padded to whole
 * words with zeros, as iOS rounds it up. */
static int download(const char *what, uint32_t addr, const uint8_t *src, size_t len)
{
    static uint8_t pkt[2 + 10 + 0x3f0 + 4];
    size_t alen = (len + 3) & ~(size_t)3, off, chunk, i;
    uint8_t *o, w[4];
    uint32_t dsum;
    uint16_t st = 0;
    int tries, attn;

    for(off = 0; off < alen; off += chunk) {
        chunk = alen - off > 0x3f0 ? 0x3f0 : alen - off;
        pkt[0] = 0x18; pkt[1] = 0xe1;
        o = pkt + 2;
        o[0] = 0x30; o[1] = 0x01;
        o[2] = chunk >> 10; o[3] = chunk >> 2;
        put32(o + 4, addr + off);
        o[8] = sum16(o + 2, 6) >> 8; o[9] = sum16(o + 2, 6);
        dsum = 0;
        for(i = 0; i < chunk; i += 4) {
            memset(w, 0, 4);
            memcpy(w, src + off + i, off + i + 4 <= len ? 4 : len > off + i ? len - off - i : 0);
            put32(o + 10 + i, (uint32_t)w[3] << 24 | (uint32_t)w[2] << 16 | w[1] << 8 | w[0]);
            dsum += o[10 + i] + o[11 + i] + o[12 + i] + o[13 + i];
        }
        put32(o + 10 + chunk, dsum);
        for(tries = 0; tries < 5; tries++) {
            arm_attn();
            if(xfer(pkt, NULL, chunk + 16))
                return -1;
            attn = wait_attn(200);
            if(atn_ack(&st))
                return -1;
            if(st == 0x4bc1)
                break;
            printf("  %s @ %08x: ack %04x%s, retrying\n", what, addr + (uint32_t)off, st,
                   attn ? "" : " (no ATTN)");
        }
        if(st != 0x4bc1) {
            printf("  %s @ %08x: gave up after 5 tries\n", what, addr + (uint32_t)off);
            return -1;
        }
    }
    printf("  %s: %zu bytes to %08x, every packet 4bc1\n", what, len, addr);
    return 0;
}

static uint8_t *load_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    uint8_t *p;
    long sz;

    if(!f) {
        perror(path);
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    rewind(f);
    p = malloc(sz > 0 ? sz : 1);
    if(!p || fread(p, 1, sz, f) != (size_t)sz) {
        fprintf(stderr, "%s: short read\n", path);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *len = sz;
    return p;
}

static int execute(uint32_t addr)
{
    uint8_t p[12] = { 0x1d, 0x53 };
    uint16_t c;

    put32(p + 2, addr);
    put32(p + 6, 1);
    c = sum16(p + 2, 8);
    p[10] = c >> 8; p[11] = c;
    hex("EXECUTE", p, 12);
    return xfer(p, NULL, 12);
}

/* ---- the firmware, out of the mtprops -------------------------------- */

static int b64(int ch)
{
    if(ch >= 'A' && ch <= 'Z') return ch - 'A';
    if(ch >= 'a' && ch <= 'z') return ch - 'a' + 26;
    if(ch >= '0' && ch <= '9') return ch - '0' + 52;
    if(ch == '+') return 62;
    if(ch == '/') return 63;
    return -1;
}

static uint8_t *load_firmware(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    char *txt, *p, *e;
    long sz;
    uint8_t *out;
    unsigned acc = 0, bits = 0;
    size_t n = 0;

    if(!f) {
        perror(path);
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    rewind(f);
    txt = malloc(sz + 1);
    if(!txt || fread(txt, 1, sz, f) != (size_t)sz) {
        fprintf(stderr, "%s: short read\n", path);
        return NULL;
    }
    fclose(f);
    txt[sz] = 0;

    p = strstr(txt, "<key>PreconstructedBootloadPacketType</key>");
    if(p && (p = strstr(p, "<string>")) && (e = strchr(p + 8, '<')))
        printf("  packet type %.*s\n", (int)(e - p - 8), p + 8);
    p = strstr(txt, "<key>Constructed Firmware</key>");
    if(!p || !(p = strstr(p, "<data>")) || !(e = strstr(p, "</data>"))) {
        fprintf(stderr, "%s: no Constructed Firmware\n", path);
        return NULL;
    }
    out = malloc(e - p);
    for(p += 6; p < e; p++) {
        int v = b64(*p);
        if(v < 0)
            continue;
        acc = acc << 6 | v;
        bits += 6;
        if(bits >= 8) {
            bits -= 8;
            out[n++] = acc >> bits;
        }
    }
    free(txt);
    *len = n;
    return out;
}

/* ---- post-boot, as iOS does it ----------------------------------------- */

static int z2_quiet(const uint8_t *in, uint8_t *r)
{
    uint8_t b[16];
    uint16_t s;

    memcpy(b, in, 16);
    s = sum16(b, 14);
    b[14] = s; b[15] = s >> 8;
    return xfer(b, r, 16);
}

static int z2_cmd(const uint8_t *in, uint8_t *res)
{
    uint8_t r[16];

    if(z2_quiet(in, r))
        return -1;
    printf("  cmd %02x %02x ->", in[0], in[1]);
    hex("", r, 16);
    if(res)
        memcpy(res, r, 16);
    return 0;
}

/* e3 id, twice: the answer to a command comes back during the next
 * transfer.  1 and the 16 answer bytes -- e3 id status len(LE16) data[9]
 * csum -- when it is that report's, with a good sum. */
static int get_report(uint8_t id, uint8_t *r)
{
    uint8_t c[16] = { 0xe3, id };

    if(z2_quiet(c, r) || z2_quiet(c, r))
        return 0;
    return r[0] == 0xe3 && r[1] == id && (r[14] | r[15] << 8) == sum16(r, 14);
}

static void list_reports(void)
{
    uint8_t r[16];
    unsigned id, n = 0;

    printf("== reports the chip answers (e3 id): status, length, first bytes\n");
    char other[256 * 6] = "", *o = other;

    for(id = 0; id < 256; id++) {
        if(!get_report(id, r))
            continue;
        if(r[2] != 0) {
            o += sprintf(o, " %02x:%02x", id, r[2]);
            continue;
        }
        printf("  %02x: len %-4u", id, r[3] | r[4] << 8);
        hex("", r + 5, 9);
        n++;
    }
    printf("  %u reports; answered with another status (id:status):%s\n", n, other);
}

/* GET report the way AppleMultitouchZ2SPI's control read does it: a stage-0
 * command, then stage 1 which carries the answer -- 16 bytes for a short
 * read (e6), len + 5 for a long one (e7), the csum at the very end. */
static uint8_t d9[64];          /* the last answer to GET d9 */

static void ios_get(uint8_t id, unsigned len, int lng)
{
    uint8_t c[16] = { lng ? 0xe7 : 0xe6, id, 0, len }, r[16], b[64], a[64];
    uint16_t s = c[0] + id + len;
    unsigned n = lng ? len + 5 : 16;

    c[14] = s; c[15] = s >> 8;
    if(xfer(c, r, 16) || n > sizeof(b))
        return;
    memset(b, 0, n);
    memcpy(b, c, 4);
    b[2] = 1;
    b[n - 2] = s; b[n - 1] = s >> 8;
    if(xfer(b, a, n))
        return;
    if(id == 0xd9)
        memcpy(d9, a, n);
    printf("  GET %02x ->", id);
    hex("", a, n);
    if(n > 16)
        hex("     ...", a + 16, n - 16);
}

static void set_report(uint8_t id, const uint8_t *data, uint8_t len)
{
    uint8_t c[16] = { 0xe4, id, len };
    memcpy(c + 3, data, len);
    z2_cmd(c, NULL);
    memset(c, 0, 16);           /* a clean e1 00, as iOS sends it */
    c[0] = 0xe1;
    z2_cmd(c, NULL);
}

/* Frames, read here instead of by the kernel, so nothing depends on how
 * the driver's READY path treats them.  AppleMultitouchZ2SPI, with the flag
 * finishStarting sets once it has seen HBPP (so 0xeb, not 0xea):
 *   result length (0x80605be0): {eb, seq, 0.., sum16 of [0..13] LE}; the
 *     answer is good if [0] & 0xf0 == 0xe0 and [14..15] is its own sum --
 *     any 0xeX, not only ea/eb -- and [1..2] is the length
 *   result data (0x80605d5c), only for a non-zero length: {eb, seq, 1, 0..}
 *     over length + 5 bytes, sum16 of [0..13] in the last two; the answer
 *     starts ea (or eb), its first five bytes sum to 0 mod 256, [2..3] is
 *     the payload length and the payload's sum16 is in the last two bytes
 *   seq goes 1, 2, 1, ... after each good read. */
static void poll_frames(int secs)
{
    uint8_t q[16], r[16], last[16] = { 0 }, lastfr[32] = { 0 }, pkt[2048], fr[2048];
    unsigned seq = 1, nattn = 0, nframes = 0, nres = 0, nlow = 0, nsamp = 0, lines = 0, len, n;
    time_t end = time(NULL) + secs;
    uint16_t s;
    int attn, good;
    char lvl;

    time_t next = time(NULL) + 2;

    cs_gap_us = poll_gap_us;
    printf("== poll %d s before READY, CS gap %u us -- touch the glass now (L = ATTN pin level)\n", secs, cs_gap_us);
    while(time(NULL) < end) {
        if(time(NULL) >= next) {
            stamp("polling");
            next = time(NULL) + 2;
        }
        arm_attn();
        attn = wait_attn(50);
        nattn += attn;
        lvl = attn_level();
        nsamp++;
        nlow += lvl == '0';
        if(attn_only && !attn && lvl != '0')
            continue;
        memset(q, 0, 16);
        q[0] = 0xeb; q[1] = seq;
        s = sum16(q, 14); q[14] = s; q[15] = s >> 8;
        if(xfer(q, r, 16))
            break;
        len = r[1] | r[2] << 8;
        good = (r[0] & 0xf0) == 0xe0 && (r[14] | r[15] << 8) == sum16(r, 14);
        if(good && len) {
            nres++;
            n = len + 5 > sizeof(pkt) ? sizeof(pkt) : len + 5;
            if(n < 16)
                n = 16;
            memset(pkt, 0, n);
            pkt[0] = 0xeb; pkt[1] = seq; pkt[2] = 1;
            s = sum16(pkt, 14); pkt[n - 2] = s; pkt[n - 1] = s >> 8;
            if(xfer(pkt, fr, n))
                break;
            if(fr[0] == 0xea || fr[0] == 0xeb) {
                nframes++;
                seq = seq == 1 ? 2 : 1;
            }
            if((attn || memcmp(fr, lastfr, n < 32 ? n : 32)) && lines++ < 60) {
                printf("  %s L%c len %-4u", attn ? "ATTN" : "    ", lvl, len);
                hex("", r, 16);
                printf("            data");
                hex("", fr, n);
                if(n > 16)
                    hex("        ...", fr + 16, n - 16);
            }
            memcpy(lastfr, fr, n < 32 ? n : 32);
        } else if(attn || memcmp(r, last, 16)) {
            if(lines++ < 60) {
                printf("  %s L%c %s", attn ? "ATTN" : "    ", lvl, good ? "no data " : "bad hdr ");
                hex("", r, 16);
            }
        }
        memcpy(last, r, 16);
    }
    printf("  %u ATTN, %u results read, %u of them frames (ea/eb), in %d s\n", nattn, nres, nframes, secs);
    printf("  ATTN pin low in %u of %u samples, now %c\n", nlow, nsamp, attn_level());
}

int main(int argc, char **argv)
{
    const char *mtprops = "/lib/firmware/K93.mtprops", *calpath = NULL, *proxpath = NULL;
    char *modes = NULL;
    int opt, dlmode = 0, noscan = 0, nocal = 0, opmode = 0, pmugpio = -1, listrep = 0, hold = -1, noready = 0, poll = 0, wake = 0;
    uint8_t *fw, *cal = NULL, *prox = NULL, c[16], res[16];
    size_t fwlen, callen = 0, proxlen = 0;
    uint32_t ver, fll = FLL_MVAL;
    uint16_t st;
    struct hxt_metrics m = { 0, 0, 0, 0 };
    unsigned i;

    while((opt = getopt(argc, argv, "c:F:G:P:Ro:BnAWENSDM:p:t:g:a")) != -1) {
        switch(opt) {
        case 'c': calpath = optarg; break;
        case 'P': proxpath = optarg; break;
        case 'B': noscan = 1; break;
        case 'n': nocal = 1; break;
        case 'A': opmode = 1; break;
        case 'N': noready = 1; break;
        case 'S': safe = 1; break;
        case 'D': pmu_full = 1; break;
        case 'M':
            if(npmu_w < 16 && sscanf(optarg, "%x:%x:%x", &pmu_w[npmu_w][0], &pmu_w[npmu_w][1], &pmu_w[npmu_w][2]) == 3)
                npmu_w++;
            else
                fprintf(stderr, "-M wants reg:mask:val in hex\n");
            break;
        case 'W': wake = 1; break;
        case 'E': dlmode = 1; break;
        case 'p': poll = atoi(optarg); break;
        case 'g': poll_gap_us = strtoul(optarg, NULL, 0); break;
        case 'a': attn_only = 1; break;
        case 't': hold = atoi(optarg); break;
        case 'F': fll = strtoul(optarg, NULL, 0); break;
        case 'G': pmugpio = strtoul(optarg, NULL, 0) & 0xff; break;
        case 'R': listrep = 1; break;
        case 'o': modes = optarg; break;
        default:
            fprintf(stderr, "usage: %s [-c cal] [-P proxcal] [-F fll-mval] [-G pmu-gpio0] [-R] [-o m,m..] [-B] [-n] [-A] [-W] [-E] [-p secs] [-g usec] [-a] [-t secs] [-S] [-D] [-M reg:mask:val].. [-N] [mtprops]\n", argv[0]);
            return 2;
        }
    }
    if(optind < argc)
        mtprops = argv[optind];
    setvbuf(stdout, NULL, _IOLBF, 0);
    clock_gettime(CLOCK_MONOTONIC, &t_start);

    printf("== firmware\n");
    fw = load_firmware(mtprops, &fwlen);
    if(!fw)
        return 1;
    printf("  %zu bytes, starts", fwlen);
    hex("", fw, 16);
    if(calpath) {
        cal = load_file(calpath, &callen);
        if(!cal)
            return 1;
        printf("  calibration %s, %zu bytes, starts", calpath, callen);
        hex("", cal, callen);
    }
    if(proxpath) {
        prox = load_file(proxpath, &proxlen);
        if(!prox)
            return 1;
        printf("  prox calibration %s, %zu bytes, starts", proxpath, proxlen);
        hex("", prox, proxlen);
    }

    fd = open("/dev/hx-touch", O_RDWR);
    if(fd < 0) {
        perror("/dev/hx-touch");
        return 1;
    }

    if(safe) {
        if(attn_level() == '?') {
            printf("-S needs /dev/mem\n");
            return 1;
        }
        gpio[GPIO_DISABLE0] = ATTN_BIT;
        gpio[GPIO_STATUS0] = ATTN_BIT;
        printf("== -S: ATTN's interrupt masked in the GPIO block, watching the pad (cfg %08x)\n",
               (unsigned)gpio[ATTN_PIN]);
    }

    stamp("opened");
    for(i = 0; i < npmu_w; i++) {
        int was = pmu_reg(pmu_w[i][0], -1), now;
        if(was < 0)
            continue;
        now = pmu_reg(pmu_w[i][0], (was & ~pmu_w[i][1]) | (pmu_w[i][2] & pmu_w[i][1]));
        printf("== -M PMU %02x: %02x -> %02x (mask %02x val %02x)\n", pmu_w[i][0], was, now,
               pmu_w[i][1], pmu_w[i][2]);
    }
    if(npmu_w)
        usleep(10000);
    printf("== PMU GPIO0 (digitizer clock enable) after open\n");
    printf("  PMU 0x61 = %02x\n", pmu_reg(0x61, -1));
    if(pmugpio >= 0) {
        printf("  PMU 0x61 <- %02x, reads %02x\n", pmugpio, pmu_reg(0x61, pmugpio));
    }

    printf("== reset\n");
    /* hx-touchd's order, which the chip has answered: a reset, one transfer
     * with chip select off so the controller has driven the clock to its idle
     * level, then the reset that counts -- iOS's resetDevice likewise starts
     * with "ensuring S_CLK is high". */
    if(ioctl(fd, HXT_IOC_RESET) < 0)
        perror("RESET");
    memset(c, 0, 4);
    if(write(fd, c, 4) == 4 && read(fd, res, 4) == 4)
        hex("idle transfer ->", res, 4);
    usleep(1000);
    arm_attn();
    if(ioctl(fd, HXT_IOC_RESET) < 0)
        perror("RESET");
    printf("  boot ATTN: %s\n", wait_attn(500) ? "yes" : "NO (500 ms)");
    if(wake) {
        c[0] = 0x19; c[1] = 0xc1;
        xfer(c, res, 2);
        hex("REQ_WAKEUP ->", res, 2);
        usleep(10000);
    }
    if(dlmode) {
        atexit(download_restore);
        download_mode(1);
    }
    if(!hbpp_check())
        printf("  not in HBPP -- carrying on to see what the chip does\n");

    printf("== download\n");
    arm_attn();
    if(xfer(fw, NULL, fwlen))
        return 1;
    printf("  ATTN after download: %s\n", wait_attn(200) ? "yes" : "no");
    if(atn_ack(&st))
        return 1;
    printf("  ack %04x%s\n", st, st == 0x4bc1 ? " (firmware accepted)" : "   NOT 4bc1");

    if(prox || cal) {
        printf("== calibration (MTSPIBootloader_Z2: prox first, then the panel's)\n");
        if(prox && download("prox calibration", PROX_CAL_ADDR, prox, proxlen))
            return 1;
        if(cal && download("calibration", CAL_DL_ADDR, cal, callen))
            return 1;
    }

    if(!nocal) {
        printf("== performCalibSeq (MTSPIBootloader_Z2)\n");
        if(reg_read(0x10008ffc, &ver))
            return 1;
        if(ver == Z2_VER_NO_WRITES) {
            printf("  version %08x: no register writes (as iOS)\n", ver);
        } else if(reg_write(FLL_MVAL_ADDR, fll, 0x1fff) ||
                  reg_write(0x10001c08, 0x00840000, 0x00ff0000) ||
                  reg_write(0x1000300c, 5, 0x85) ||
                  reg_write(0x1000304c, 0x20, 0xffffffff)) {
            printf("  a write was not acked 4ad1: iOS fails the bootload here; stopping before EXECUTE\n");
            return 1;
        }
        c[0] = 0x1f; c[1] = 0x01;
        arm_attn();
        xfer(c, res, 2);
        hex("calib request ->", res, 2);
        usleep(65000);
        printf("  ATTN during calibration: %s\n", wait_attn(100) ? "yes" : "no");
        if(atn_ack(&st) == 0)
            printf("  calibration ack %04x\n", st);
    }

    printf("== execute\n");
    arm_attn();
    execute(FW_EXECUTE_ADDR);
    if(dlmode)
        download_mode(0);
    usleep(40000);
    printf("  ATTN after execute: %s\n", wait_attn(200) ? "yes" : "no");

    stamp("executed");
    printf("== post-boot (K93 iOS trace, touch/ios-mtlog-k93-1.txt)\n");
    usleep(2000);               /* K93's iOS sends no ee 00 wake here */
    memset(c, 0, 16); c[0] = 0xe2;
    z2_cmd(c, res);
    z2_cmd(c, res);
    printf("  device info: family %02x, max packet %u\n", res[1], res[3] | res[4] << 8);
    ios_get(0xd1, 1, 0);
    ios_get(0xd3, 12, 1);
    ios_get(0xd0, 8, 0);
    ios_get(0xa1, 6, 0);
    ios_get(0xd9, 8, 0);        /* K93: short, 8 bytes (P105: long, 16) */
    if(opmode) {
        static const uint8_t op[1] = { 0 };
        printf("  operating mode: report ab = 00 (the driver, once, at start)\n");
        set_report(0xab, op, 1);
    }
    if(!noscan) {
        static const uint8_t bf[4] = { 0x9f, 0x03, 0x0b, 0x02 }, af[1] = { 0 };
        printf("  bf = 9f 03 0b 02, af = 00, twice (userspace, what starts the scan)\n");
        set_report(0xbf, bf, 4);
        set_report(0xaf, af, 1);
        set_report(0xbf, bf, 4);
        set_report(0xaf, af, 1);
        stamp("bf/af sent");
    }

    if(listrep)
        list_reports();

    if(modes) {
        char *p = modes;
        uint8_t op[1];
        while(*p) {
            op[0] = strtoul(p, &p, 0);
            printf("== operating mode %02x\n", op[0]);
            set_report(0xab, op, 1);
            if(get_report(0xab, res))
                hex("  ab reads", res, 16);
            else
                hex("  ab: no answer", res, 16);
            poll_frames(poll > 0 ? poll : 5);
            while(*p == ',' || *p == ' ')
                p++;
        }
    } else if(poll > 0) {
        stamp("poll starts");
        poll_frames(poll);
        stamp("poll done");
    }

    if(noready) {
        stamp("closing (-N)");
        close(fd);
        stamp("closed");
        return 0;
    }
    /* The surface, from report d9 (e6 d9 00, then width and height as LE32,
     * 0.01 mm): 0x3999 x 0x4ccc on K93 = 147.45 x 196.60 mm.  The touches'
     * coordinates come in that range (iOS trace: x <= 0x2e24, y <= 0x2dfc). */
    if(d9[0] == 0xe6 && d9[1] == 0xd9 && d9[2] == 0) {
        m.right = (int)(d9[3] | d9[4] << 8 | d9[5] << 16 | (uint32_t)d9[6] << 24);
        m.bottom = (int)(d9[7] | d9[8] << 8 | d9[9] << 16 | (uint32_t)d9[10] << 24);
    }
    printf("  metrics x %d..%d y %d..%d\n", m.left, m.right, m.top, m.bottom);
    if(ioctl(fd, HXT_IOC_METRICS, &m) < 0)
        perror("METRICS");
    printf("== READY -- touch the glass; reports go to the kernel\n");
    if(ioctl(fd, HXT_IOC_READY) < 0)
        perror("READY");
    if(hold < 0)
        for(;;)
            sleep(60);
    sleep(hold);
    return 0;
}
