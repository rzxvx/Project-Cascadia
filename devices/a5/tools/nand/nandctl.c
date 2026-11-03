/* nandctl -- READ-ONLY poking at the A5's NAND through the H2FMI controllers,
 * a command-line front end to ppn.c (which holds the opcode allowlist and says
 * where every sequence comes from).
 *
 *   nandctl regs                    dump both controllers (skips the data FIFO)
 *   nandctl readid BUS CE [ADDR]    READ ID (90h) at ADDR (default 0), 8 bytes
 *   nandctl reset BUS CE            NAND RESET (FFh) -- touches no data
 *   nandctl getfeat BUS CE FEAT LEN PPN get feature (EE FEAT E7, 77 7D, 7A)
 *   nandctl params BUS CE           PPN device parameters (92 00 97), 512 bytes
 *   nandctl ppninfo                 firmware version (feature 0x9080), both buses
 *   nandctl page BUS CE ROW F [LEN] page read (0A ROW 37, 7A), raw: LEN bytes as
 *                                   the bus delivers them (default 16448 = 16 KB
 *                                   data + 64 metadata) into file F
 *   nandctl bootpage BUS CE ROW [F] boot-page format (LLB, flash partition
 *                                   table): 3 x (512 data + 53 BCH bytes), the
 *                                   BCH bytes dropped, NOT corrected
 *   nandctl recover BUS             un-wedge a bus: PMGR block reset (as iBEC's
 *                                   h2fmi_device_reset), FMC config put back
 *   nandctl sdr BUS                 FMC to SDR (FMC_ON 1, slow timing): what a
 *                                   PPN needs after a NAND reset
 *   nandctl ddr BUS                 PPN power state -> DDR (set-feature 0x180 =
 *                                   0x0a, iBEC's transitionWorldToDDR), FMC to DDR
 *   nandctl dump BUS CE CAU BLK N F N blocks x 256 pages from CAU/BLK on, raw,
 *                                   into F ("-" = stdout); a status byte per page
 *                                   into F.st
 *
 * ROW = page | block << 8 | cau << 19 (| slc << 23), 3 address bytes.  CE is
 * the chip select on that bus; on p105 only CE0 has a chip.  Every command
 * that drives a bus takes its lock first and gives up if something else
 * (another nandctl, iosnand) holds it.  Build:
 *   arm-linux-gnueabihf-gcc -static -Os -D_FILE_OFFSET_BITS=64 -o nandctl \
 *       tools/nand/nandctl.c tools/nand/ppn.c
 */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "ppn.h"

#define R PPN_R
#define W PPN_W
#define PAGE_RAW PPN_PAGE_RAW

static uint64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000000ull + ts.tv_nsec / 1000;
}

static void hexdump(const uint8_t *p, uint32_t n)
{
    for (uint32_t o = 0; o < n; o += 16) {
        printf("  %04x:", o);
        for (uint32_t k = o; k < o + 16 && k < n; k++)
            printf(" %02x", p[k]);
        printf("  ");
        for (uint32_t k = o; k < o + 16 && k < n; k++)
            putchar(p[k] >= 0x20 && p[k] < 0x7f ? p[k] : '.');
        printf("\n");
    }
}

static void dump(const char *name, volatile uint32_t *b, uint32_t pa, uint32_t n, int skip_fifo)
{
    printf("%s @ 0x%08x\n", name, pa);
    for (uint32_t o = 0; o < n; o += 16) {
        printf("  +%03x:", o);
        for (uint32_t k = o; k < o + 16; k += 4)
            if (skip_fifo && k == FMI_DATA)
                printf(" --------");
            else
                printf(" %08x", R(b, k));
        printf("\n");
    }
}

static void print_id(int bus, int ce, uint8_t addr, const uint8_t id[8])
{
    printf("FMI%d CE%d id@%02x:", bus, ce, addr);
    for (int i = 0; i < 8; i++)
        printf(" %02x", id[i]);
    printf("  \"");
    for (int i = 0; i < 8; i++)
        putchar(id[i] >= 0x20 && id[i] < 0x7f ? id[i] : '.');
    printf("\"\n");
}

int main(int argc, char **argv)
{
    ppn_open();

    if (argc >= 2 && !strcmp(argv[1], "regs")) {
        for (int b = 0; b < 2; b++) {
            printf("FMI%d gates: 0x%08x 0x%08x\n", b, R(ppn_pmgr, 0xc4 + b * 8), R(ppn_pmgr, 0xc8 + b * 8));
            if (!ppn_gates_on(b))
                continue;
            dump("  FMI", ppn_fmi[b], FMI_BASE(b), 0x40, 1);
            dump("  FMC", ppn_fmc[b], FMI_BASE(b) + FMC_OFF, 0x80, 0);
            dump("  ECC", ppn_ecc[b], FMI_BASE(b) + ECC_OFF, 0x40, 0);
        }
        return 0;
    }
    if (argc >= 4 && !strcmp(argv[1], "readid")) {
        int bus = atoi(argv[2]) & 1, ce = atoi(argv[3]);
        if (!ppn_ce_ok(ce))
            return 1;
        uint8_t addr = argc > 4 ? strtoul(argv[4], NULL, 0) : 0, id[8];
        if (!ppn_claim(bus))
            return 1;
        int r = ppn_read_id(bus, ce, addr, id);
        print_id(bus, ce, addr, id);
        return r ? 1 : 0;
    }
    if (argc >= 4 && !strcmp(argv[1], "reset")) {
        int bus = atoi(argv[2]) & 1, ce = atoi(argv[3]);
        if (!ppn_ce_ok(ce))
            return 1;
        if (!ppn_claim(bus))
            return 1;
        int r = ppn_nand_reset(bus, ce);
        printf("FMI%d CE%d reset: %s\n", bus, ce, r ? "timeout" : "ok");
        return r ? 1 : 0;
    }
    if (argc >= 6 && !strcmp(argv[1], "getfeat")) {
        int bus = atoi(argv[2]) & 1, ce = atoi(argv[3]);
        if (!ppn_ce_ok(ce))
            return 1;
        uint16_t feat = strtoul(argv[4], NULL, 0);
        uint32_t len = strtoul(argv[5], NULL, 0);
        uint8_t buf[4096] = { 0 }, st = 0;
        if (len > sizeof(buf) || !ppn_claim(bus))
            return 1;
        int r = ppn_get_feature(bus, ce, feat, buf, len, &st);
        printf("FMI%d CE%d feature 0x%04x: status 0x%02x%s\n", bus, ce, feat, st, r ? " (failed)" : "");
        hexdump(buf, len);
        return r ? 1 : 0;
    }
    if (argc >= 4 && !strcmp(argv[1], "params")) {
        int bus = atoi(argv[2]) & 1, ce = atoi(argv[3]);
        if (!ppn_ce_ok(ce))
            return 1;
        uint8_t buf[0x200] = { 0 }, st = 0;
        if (!ppn_claim(bus))
            return 1;
        int r = ppn_params(bus, ce, buf, &st);
        printf("FMI%d CE%d device parameters: status 0x%02x%s\n", bus, ce, st, r ? " (failed)" : "");
        hexdump(buf, sizeof(buf));
        return r ? 1 : 0;
    }
    if (argc >= 2 && !strcmp(argv[1], "ppninfo")) {
        for (int b = 0; b < 2; b++) {
            uint8_t buf[16] = { 0 }, st = 0;
            if (!ppn_claim(b))
                continue;
            int r = ppn_get_feature(b, 0, 0x9080, buf, 16, &st);
            printf("FMI%d CE0 firmware (0x9080): status 0x%02x%s \"%.16s\"\n", b, st,
                   r ? " (failed)" : "", (char *)buf);
        }
        return 0;
    }
    if (argc >= 5 && !strcmp(argv[1], "bootpage")) {
        int bus = atoi(argv[2]) & 1, ce = atoi(argv[3]);
        if (!ppn_ce_ok(ce))
            return 1;
        uint32_t row = strtoul(argv[4], NULL, 0);
        uint8_t raw[3 * 565 + 3] __attribute__((aligned(4))) = { 0 }, buf[0x600], st = 0;
        if (!ppn_claim(bus))
            return 1;
        int r = ppn_read_page(bus, ce, row, raw, sizeof(raw) & ~3u, &st);
        for (int i = 0; i < 3; i++)
            memcpy(buf + i * 512, raw + i * 565, 512);
        printf("FMI%d CE%d boot page 0x%06x: status 0x%02x%s\n", bus, ce, row, st, r ? " (failed)" : "");
        if (argc >= 6) {
            FILE *f = fopen(argv[5], "wb");
            if (!f || fwrite(buf, 1, sizeof(buf), f) != sizeof(buf)) {
                perror(argv[5]);
                return 1;
            }
            fclose(f);
        } else {
            hexdump(buf, 0x100);
        }
        return r ? 1 : 0;
    }
    if (argc >= 3 && !strcmp(argv[1], "recover")) {
        int bus = atoi(argv[2]) & 1;
        if (!ppn_claim(bus))
            return 1;
        ppn_recover(bus);
        uint8_t buf[16] = { 0 }, st = 0;
        int r = ppn_get_feature(bus, 0, 0x9080, buf, 16, &st);
        printf("FMI%d recovered: firmware query status 0x%02x%s \"%.16s\"\n", bus, st,
               r ? " (failed)" : "", (char *)buf);
        return r ? 1 : 0;
    }
    if (argc >= 3 && !strcmp(argv[1], "sdr")) {
        int bus = atoi(argv[2]) & 1;
        if (!ppn_claim(bus))
            return 1;
        W(ppn_fmc[bus], FMC_ON, 1);
        W(ppn_fmc[bus], FMC_IF_CTRL, 0xffff);
        printf("FMI%d: FMC in SDR, IF_CTRL 0xffff\n", bus);
        return 0;
    }
    if (argc >= 3 && !strcmp(argv[1], "ddr")) {
        int bus = atoi(argv[2]) & 1;
        uint8_t st = 0;
        if (!ppn_claim(bus))
            return 1;
        int r = ppn_set_power_state(bus, 0, PPN_PS_DDR, &st);
        printf("FMI%d CE0 power state -> DDR: status 0x%02x%s\n", bus, st, r ? " (failed)" : "");
        if (r)
            return 1;
        W(ppn_fmc[bus], FMC_ON, 5);
        W(ppn_fmc[bus], FMC_IF_CTRL, 0);
        return 0;
    }
    if (argc >= 6 && !strcmp(argv[1], "page")) {
        int bus = atoi(argv[2]) & 1, ce = atoi(argv[3]);
        if (!ppn_ce_ok(ce))
            return 1;
        uint32_t row = strtoul(argv[4], NULL, 0);
        uint32_t len = argc > 6 ? strtoul(argv[6], NULL, 0) : PAGE_RAW;
        static uint8_t buf[0x4400] __attribute__((aligned(4)));
        uint8_t st = 0;
        if (len > sizeof(buf) || !ppn_claim(bus))
            return 1;
        int r = ppn_read_page(bus, ce, row, buf, len, &st);
        printf("FMI%d CE%d page 0x%06x, %u bytes: status 0x%02x%s\n",
               bus, ce, row, len, st, r ? " (failed)" : "");
        FILE *f = fopen(argv[5], "wb");
        if (!f || fwrite(buf, 1, len, f) != len) {
            perror(argv[5]);
            return 1;
        }
        fclose(f);
        return r ? 1 : 0;
    }
    if (argc >= 8 && !strcmp(argv[1], "dump")) {
        int bus = atoi(argv[2]) & 1, ce = atoi(argv[3]);
        if (!ppn_ce_ok(ce))
            return 1;
        uint32_t cau = strtoul(argv[4], NULL, 0), blk = strtoul(argv[5], NULL, 0);
        uint32_t n = strtoul(argv[6], NULL, 0);
        static uint8_t buf[PAGE_RAW] __attribute__((aligned(4))), sts[256];
        char stname[256];
        if (cau > 1 || blk + n > 1064 || !ppn_claim(bus))
            return 1;
        int out = strcmp(argv[7], "-") ? open(argv[7], O_WRONLY | O_CREAT | O_TRUNC, 0644) : 1;
        snprintf(stname, sizeof(stname), "%s.st", strcmp(argv[7], "-") ? argv[7] : "/tmp/dump");
        int stf = open(stname, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (out < 0 || stf < 0) {
            perror("open");
            return 1;
        }
        uint64_t t0 = now_us();
        for (uint32_t b = blk; b < blk + n; b++) {
            int bad = 0;
            for (uint32_t pg = 0; pg < 256; pg++) {
                uint8_t st = 0;
                if (ppn_read_page(bus, ce, pg | b << 8 | cau << 19, buf, PAGE_RAW, &st)) {
                    memset(buf, 0, PAGE_RAW);
                    st = 0;
                }
                sts[pg] = st;
                bad += st != 0x40;
                if (write(out, buf, PAGE_RAW) != PAGE_RAW) {
                    perror("write");
                    return 1;
                }
            }
            if (write(stf, sts, 256) != 256) {
                perror("write");
                return 1;
            }
            fprintf(stderr, "FMI%d CE%d CAU%u block %4u: %3d pages not 0x40, %.1f MB/s\n",
                    bus, ce, cau, b, bad,
                    (b - blk + 1) * 256.0 * PAGE_RAW / (now_us() - t0 + 1));
        }
        close(stf);
        if (out != 1)
            close(out);
        return 0;
    }
    fprintf(stderr, "usage: nandctl regs | readid BUS CE [ADDR] | reset BUS CE |\n"
                    "               getfeat BUS CE FEAT LEN | params BUS CE | ppninfo |\n"
                    "               page BUS CE ROW FILE [LEN] | bootpage BUS CE ROW [FILE] |\n"
                    "               dump BUS CE CAU BLOCK NBLOCKS FILE | recover BUS | sdr BUS | ddr BUS\n");
    return 1;
}
