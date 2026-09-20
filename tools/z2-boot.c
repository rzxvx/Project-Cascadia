/* z2-boot -- bring up the P105 digitizer the way iOS 8.4.1 does.
 *
 * Replaces hx-touchd on this device.  hx-touchd (Corellium, written for the
 * A10's digitizer) uploads the "Constructed Firmware" and then speaks its own
 * post-boot dialect.  On P105 that leaves the chip answering commands -- a
 * dozen ATTNs during the handshake -- and then silent, finger on the glass or
 * not.  iOS does more, and the values it uses live in the kext personality
 * "AppleMultitouchSPI - C1,P105" (IOClass AppleMultitouchN1SPI), not in the
 * mtprops, which for C1F14,1 holds only the firmware blob.
 *
 * MTSPIBootloader_N1 (12H321) between the firmware download and EXECUTE:
 *
 *   performCalibSeq   read  0x10008ffc                version
 *                     write 0x10003060 <- 0x17d3      fll-mval
 *                     write ref-clk-div, const-cal    (keys absent for P105:
 *                                                      iOS writes 0 to address
 *                                                      0; skipped unless -Z)
 *                     write 0x10003058 <- 6
 *                     write 0x10003518 <- 1           clk32-clock-enable
 *                     read  0x10003800                SPI_APU_EN, logged
 *                     send  1f 01                     request calibration
 *                     sleep 65 ms, ATN_ACK
 *   execute           1d 53 <0x10003400> <1> csum     fw-execute-addr
 *
 * then AppleMultitouchSPI sets the operating mode, report 0xAB = 0x00
 * (AutomaticallySetOperatingMode).
 *
 * HBPP (bootloader) packets, from AppleMultitouchZ2SPI:
 *   a 32-bit field is two big-endian halves, low half first; csum is the
 *   16-bit byte sum of the fields, big-endian
 *   ATN_ACK       1a a1                    -> 2-byte status, big-endian
 *   long ATN_ACK  1a a1 18 e1 18 e1 18 e1  -> 8 bytes, value in [2..5]
 *   HBPP check    1a a1 (18 e1) x7         -> first two words are status words
 *   reg read      1c 73 addr csum          -> then long ATN_ACK
 *   reg write     1e 33 addr mask val csum -> then ATN_ACK, 4ad1 = done
 *   firmware      the whole blob in one CS window, then ATN_ACK, 4bc1 = done
 *
 * The post-boot commands (16 bytes, byte-sum of [0..13] little-endian in
 * [14..15]) are pipelined: what comes back during a transfer answers the
 * previous command.  That dialect is hx-touchd's, kept as it is because the
 * chip accepted it.
 *
 * Kernel side: /dev/hx-touch from drivers/input/touchscreen/apple-z2.c.
 *
 *   z2-boot [-m] [-n] [-O] [-Z] [-t secs] [mtprops]
 *     -m  also send hx-touchd's "mode 1" feature reports 9d/bf/af
 *     -n  skip performCalibSeq (control run)
 *     -O  skip the operating-mode report
 *     -Z  do iOS's writes to address 0 as well
 *     -t  hold the device this many seconds after READY (default: forever)
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>

#define HXT_IOC_SET_CS          _IOW('h', 1, uint32_t)
#define HXT_IOC_RESET           _IO('h', 2)
#define HXT_IOC_READY           _IO('h', 3)
#define HXT_IOC_SETUP_IRQ       _IO('h', 4)
#define HXT_IOC_WAIT_IRQ        _IOW('h', 5, uint32_t)
struct hxt_metrics { int left, right, top, bottom; };
#define HXT_IOC_METRICS         _IOW('h', 6, struct hxt_metrics)

#define CHUNK   16384

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

/* One chip-select window: n bytes out, n bytes in. */
static int xfer(const uint8_t *tx, uint8_t *rx, size_t n)
{
    size_t off = 0, c;

    cs(1);
    usleep(1000);
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
    usleep(1000);
    return 0;
}

static void arm_attn(void)
{
    ioctl(fd, HXT_IOC_SETUP_IRQ);
}

/* 1 if ATTN arrived within ms. */
static int wait_attn(unsigned ms)
{
    return ioctl(fd, HXT_IOC_WAIT_IRQ, ms) == 0;
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

/* ---- post-boot, hx-touchd's dialect ---------------------------------- */

static int z2_cmd(const uint8_t *in, uint8_t *res)
{
    uint8_t b[16], r[16];
    uint16_t s;

    memcpy(b, in, 16);
    s = sum16(b, 14);
    b[14] = s; b[15] = s >> 8;
    if(xfer(b, r, 16))
        return -1;
    printf("  cmd %02x %02x ->", b[0], b[1]);
    hex("", r, 16);
    if(res)
        memcpy(res, r, 16);
    return 0;
}

static void set_report(uint8_t id, const uint8_t *data, uint8_t len)
{
    uint8_t c[16] = { 0xe4, id, len };
    memcpy(c + 3, data, len);
    z2_cmd(c, NULL);
    c[0] = 0xe1; c[1] = 0; c[2] = 0;
    z2_cmd(c, NULL);
}

int main(int argc, char **argv)
{
    const char *mtprops = "/lib/firmware/P105.mtprops";
    int opt, mode1 = 0, nocal = 0, noopmode = 0, zero = 0, hold = -1;
    uint8_t *fw, c[16], res[16];
    size_t fwlen;
    uint32_t ver, v;
    uint16_t st;
    unsigned len;
    struct hxt_metrics m = { 0, 0, 0, 0 };

    while((opt = getopt(argc, argv, "mnOZt:")) != -1) {
        switch(opt) {
        case 'm': mode1 = 1; break;
        case 'n': nocal = 1; break;
        case 'O': noopmode = 1; break;
        case 'Z': zero = 1; break;
        case 't': hold = atoi(optarg); break;
        default:
            fprintf(stderr, "usage: %s [-m] [-n] [-O] [-Z] [-t secs] [mtprops]\n", argv[0]);
            return 2;
        }
    }
    if(optind < argc)
        mtprops = argv[optind];
    setvbuf(stdout, NULL, _IOLBF, 0);

    printf("== firmware\n");
    fw = load_firmware(mtprops, &fwlen);
    if(!fw)
        return 1;
    printf("  %zu bytes, starts", fwlen);
    hex("", fw, 16);

    fd = open("/dev/hx-touch", O_RDWR);
    if(fd < 0) {
        perror("/dev/hx-touch");
        return 1;
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

    if(!nocal) {
        printf("== performCalibSeq (MTSPIBootloader_N1)\n");
        reg_read(0x10008ffc, &ver);
        reg_write(0x10003060, 0x17d3, 0xffffffff);          /* fll-mval */
        if(zero)
            reg_write(0x00000000, 0, 0xffffffff);           /* ref-clk-div, absent */
        reg_write(0x10003058, 6, 0xffffffff);
        if(zero)
            reg_write(0x00000000, ver == 0x434d11a0 ? 2 : 0, 0xffffffff); /* const-cal, absent */
        reg_write(0x10003518, 1, 0xffffffff);               /* clk32-clock-enable */
        reg_read(0x10003800, &v);                           /* SPI_APU_EN */
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
    execute(0x10003400);
    usleep(40000);
    printf("  ATTN after execute: %s\n", wait_attn(200) ? "yes" : "no");

    printf("== post-boot\n");
    memset(c, 0, 16); c[0] = 0xee;
    z2_cmd(c, res);
    memset(c, 0, 16); c[0] = 0xe3; c[1] = 0xd9;
    z2_cmd(c, res);
    z2_cmd(c, res);
    len = res[3] | res[4] << 8;
    printf("  report d9: status %02x, %u bytes\n", res[2], len);
    if(res[2] == 0 && len && len <= 11) {
        c[0] = 0xe6;
        z2_cmd(c, NULL);
        memset(c, 0, 16); c[0] = 0xe1;
        z2_cmd(c, res);
    }
    if(mode1) {
        static const uint8_t r9d[8] = { 1 }, rbf[4] = { 0x9d, 0x81, 0x09, 0x00 }, raf[1] = { 0 };
        printf("  hx-touchd mode-1 reports\n");
        set_report(0x9d, r9d, 8);
        set_report(0xbf, rbf, 4);
        set_report(0xaf, raf, 1);
    }
    if(!noopmode) {
        static const uint8_t op[1] = { 0 };
        printf("  operating mode: report ab = 00 (iOS)\n");
        set_report(0xab, op, 1);
    }

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
