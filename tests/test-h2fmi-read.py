#!/usr/bin/env python3
"""Actual H2FMI OOB + Linux MEMREAD loops with fake pages; no hardware access."""
from pathlib import Path
import os
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
DRIVER = ROOT / 'patches/files/drivers/mtd/devices/mtd-apple-h2fmi.c'
KERNEL = ROOT / 'build/linux'


def function(source, name, external=False):
    pattern = r'\nint ' + re.escape(name) + r'\(' if external else r'\nstatic[^\n]*(?:\n)?' + re.escape(name) + r'\('
    match = re.search(pattern, source)
    if not match:
        raise ValueError('function definition missing: ' + name)
    start = match.start() + 1
    pos = source.index(name + '(', start)
    brace = source.index('{', pos)
    depth, end = 1, brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


def main():
    if not (KERNEL / 'drivers/mtd/mtdchar.c').is_file():
        print('SKIP: needs local pinned Linux v6.12 sources for real MEMREAD loop')
        return 0
    driver = DRIVER.read_text()
    core = (KERNEL / 'drivers/mtd/mtdcore.c').read_text()
    char = (KERNEL / 'drivers/mtd/mtdchar.c').read_text()
    c = r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>
#define __user
/* PINNED_MTD_ABI */
typedef uint8_t u8;
typedef uint32_t u32;
typedef uint64_t u64;
#define noinline_for_stack
#define GFP_KERNEL 0
#define unlikely(x) (x)
#define min_t(t,a,b) ((t)(a) < (t)(b) ? (t)(a) : (t)(b))
#define max(a,b) ((a) > (b) ? (a) : (b))
#define dev_err_ratelimited(...) ((void)0)
/* DRIVER_GEOMETRY */
struct mutex { int locked; };
static void mutex_lock(struct mutex *m) { assert(!m->locked); m->locked=1; }
static void mutex_unlock(struct mutex *m) { assert(m->locked); m->locked=0; }
struct h2fmi_bus { void *fmi, *fmc; u8 *page; struct mutex lock; };
struct h2fmi { struct h2fmi_bus bus[H2FMI_BUSES]; };
static int raw_cmd_error, raw_status_error, raw_data_error, raw_end_error;
static unsigned raw_end_calls, raw_resets;
static u32 readl(void *p) { return *(u32 *)p; }
static void writel(u32 v, void *p) { *(u32 *)p=v; }
static void h2fmi_clear_irqs(struct h2fmi_bus *b) { }
static int h2fmi_cmd_addr_cmd(struct h2fmi *f, struct h2fmi_bus *b, u8 a, u8 z, u32 row, int n) {
 assert(a==0x0a && z==0x37 && n==3); return raw_cmd_error;
}
static int h2fmi_status(struct h2fmi *f, struct h2fmi_bus *b, u8 *st) { *st=0x40; return raw_status_error; }
static int h2fmi_data_out(struct h2fmi *f, struct h2fmi_bus *b, u8 *buf, u32 n) {
 if (!raw_data_error) memset(buf,0x5a,n); return raw_data_error;
}
static int h2fmi_cmd1(struct h2fmi *f, struct h2fmi_bus *b, u8 cmd) { assert(cmd==0x77); raw_end_calls++; return raw_end_error; }
static void h2fmi_device_reset(struct h2fmi_bus *b, u32 on, u32 ctrl) {
 raw_resets++; assert(on==5 && ctrl==0x100001);
}
struct mtd_req_stats { unsigned uncorrectable_errors, corrected_bitflips, max_bitflips; };
struct mtd_oob_ops {
 unsigned mode; size_t len, retlen, ooblen, oobretlen; u32 ooboffs;
 u8 *datbuf, *oobbuf; struct mtd_req_stats *stats;
};
typedef struct { int locked; } spinlock_t;
struct h2fmi_die { struct h2fmi *fmi; u8 bus, cau; spinlock_t ecc_lock; };
static void spin_lock(spinlock_t *l) { assert(!l->locked); l->locked=1; }
static void spin_unlock(spinlock_t *l) { assert(l->locked); l->locked=0; }
struct mtd_info {
 void *priv;
 u64 size; u32 erasesize, writesize, flags, ecc_strength, bitflip_threshold;
 const char *name; struct { void *parent; } dev;
 struct mtd_ecc_stats ecc_stats;
 void *_read;
 int (*_read_oob)(struct mtd_info *, loff_t, struct mtd_oob_ops *);
};
static void *kmalloc(size_t n, int flags) { return malloc(n); }
static void kfree(void *p) { free(p); }
static void *kvmalloc(size_t n, int flags) { return malloc(n); }
static void kvfree(void *p) { free(p); }
static size_t mtd_oobavail(struct mtd_info *m, struct mtd_oob_ops *o) { return 64; }
static struct mtd_info *mtd_get_master(struct mtd_info *m) { return m; }
static u64 mtd_mod_by_ws(u64 ofs, struct mtd_info *m) { return ofs % m->writesize; }
static int mtd_is_bitflip_or_eccerr(int ret) { return ret == -EUCLEAN || ret == -EBADMSG; }
static int mtd_is_bitflip(int ret) { return ret == -EUCLEAN; }
static int copy_from_user(void *to, const void *from, size_t n) { memcpy(to, from, n); return 0; }
static int copy_to_user(void *to, const void *from, size_t n) {
 if (n) { assert(to && from); memcpy(to, from, n); } return 0;
}
static int mtd_check_oob_ops(struct mtd_info *m, loff_t from, struct mtd_oob_ops *o) {
 assert(from >= 0 && from + o->len <= m->size && o->ooboffs < 64); return 0;
}
static void ledtrig_mtd_activity(void) { }
static int mtd_read_oob_std(struct mtd_info *m, loff_t from, struct mtd_oob_ops *o) {
 return m->_read_oob(m, from, o);
}
static int mtd_io_emulated_slc(struct mtd_info *m, loff_t from, bool r, struct mtd_oob_ops *o) { abort(); }
static void mtd_update_ecc_stats(struct mtd_info *m, struct mtd_info *master, const struct mtd_ecc_stats *old) { }
static int page_status[4];
static unsigned seen[4], calls;
static int h2fmi_read_page_at(struct mtd_info *m, loff_t from, u8 *data, u8 *oob, u8 *st) {
 unsigned pg = from / H2FMI_PAGE_DATA;
 assert(pg < 4);
 calls++;
 if (++seen[pg] > 1) return -ELOOP; /* progress watchdog: never hang this test */
 if (page_status[pg] < 0) return page_status[pg];
 if (data) memset(data, 0x80 + pg, H2FMI_PAGE_DATA);
 if (oob) memset(oob, 0xa0 + pg, H2FMI_PAGE_OOB);
 *st = page_status[pg];
 return 0;
}
'''
    c = c.replace('/* PINNED_MTD_ABI */', '#include "' + str(KERNEL / 'include/uapi/mtd/mtd-abi.h') + '"')
    geometry = driver[driver.index('#define H2FMI_BUSES'):driver.index('/* The ADT')]
    registers = driver[driver.index('#define H2FMI_CE'):driver.index('/* The allowlist is')]
    c = c.replace('/* DRIVER_GEOMETRY */', geometry + registers)
    c += function(driver, 'h2fmi_read_raw_page') + '\n'
    c += function(driver, 'h2fmi_split_page') + '\n'
    c += '#define h2fmi_read_page_at actual_h2fmi_read_page_at\n'
    c += function(driver, 'h2fmi_read_page_at') + '\n'
    c += '#undef h2fmi_read_page_at\n'
    c += function(driver, 'h2fmi_bitflips') + '\n'
    c += function(driver, 'h2fmi_account_ecc') + '\n'
    c += function(driver, 'h2fmi_mtd_read_oob') + '\n'
    c += function(core, 'mtd_read_oob', external=True) + '\n'
    c += function(char, 'mtdchar_read_ioctl') + '\n'
    c += '#undef PPN_ROW\n#include "' + str(ROOT / 'tools/nand/ppn.h') + '"\n'
    c += '#define IOSNAND_IOCTL kernel_ioctl\nstatic int kernel_ioctl(int, unsigned long, void *);\n'
    c += '#include "' + str(ROOT / 'tools/nand/iosnand-mtd.h') + '"\n'
    c += r'''
static void reset(void) {
 memset(seen, 0, sizeof(seen)); calls=0;
 for (unsigned p=0; p<4; p++) page_status[p]=PPN_ST_GOOD;
}
static struct h2fmi_die fake_die;
static struct mtd_info mtd(void) {
 assert(!fake_die.ecc_lock.locked);
 struct mtd_info m = { .priv=&fake_die, .size=4*H2FMI_PAGE_DATA, .erasesize=4*H2FMI_PAGE_DATA,
  .writesize=H2FMI_PAGE_DATA, .name="fake-nand", .ecc_strength=1,
  .bitflip_threshold=1, ._read_oob=h2fmi_mtd_read_oob };
 return m;
}
static struct mtd_info ioctl_mtd;
static int kernel_ioctl(int fd, unsigned long request, void *arg) {
 assert(fd==1000);
 if (request==MEMGETBADBLOCK) return page_status[0]==PPN_ST_RETIRED;
 assert(request==IOSNAND_MEMREAD);
 int ret=mtdchar_read_ioctl(&ioctl_mtd,(struct mtd_read_req *)arg);
 if (ret<0) { errno=-ret; return -1; }
 return ret;
}
int main(void) {
 unsigned checks=0;
 u32 registers[32]={0}; registers[FMC_ON/4]=5; registers[FMC_IF_CTRL/4]=0x100001;
 u8 raw_page[H2FMI_PAGE_RAW];
 struct h2fmi raw_fmi={0}; struct h2fmi_bus bus={.fmi=registers,.fmc=registers,.page=raw_page};
 u8 raw_st;
 for (unsigned kind=0; kind<4; kind++) {
  raw_end_calls=raw_resets=0; raw_cmd_error=raw_status_error=raw_data_error=raw_end_error=0;
  if (kind==1) raw_end_error=-ETIMEDOUT;
  if (kind==2) { raw_data_error=-EIO; raw_end_error=-ETIMEDOUT; }
  if (kind==3) { raw_cmd_error=-EPERM; raw_end_error=-ETIMEDOUT; }
  int ret=h2fmi_read_raw_page(&raw_fmi,&bus,0,&raw_st);
  assert(ret==(kind==0 ? 0 : kind==1 ? -ETIMEDOUT : kind==2 ? -EIO : -EPERM));
  assert(raw_end_calls==1 && raw_resets==1 && registers[FMC_CE_CTRL/4]==0);
  checks++;
 }
 uint8_t out[4*H2FMI_PAGE_DATA], oob[4*H2FMI_PAGE_OOB];
 for (unsigned kind=0; kind<4; kind++) {
  reset(); struct mtd_info m=mtd();
  const int st[]={PPN_ST_GOOD,PPN_ST_REFRESH,PPN_ST_RETIRED,0x41};
  page_status[0]=st[kind]; memset(out,0xdd,sizeof(out)); memset(oob,0xee,sizeof(oob));
  struct mtd_read_req r={.start=0,.len=H2FMI_PAGE_DATA,.ooblen=64,
   .usr_data=(uintptr_t)out,.usr_oob=(uintptr_t)oob,.mode=MTD_OPS_PLACE_OOB};
  int ret=mtdchar_read_ioctl(&m,&r);
  assert(calls==1 && r.len==H2FMI_PAGE_DATA && r.ooblen==64);
  assert(out[0]==0x80 && out[H2FMI_PAGE_DATA-1]==0x80 && out[H2FMI_PAGE_DATA]==0xdd);
  assert(oob[0]==0xa0 && oob[63]==0xa0 && oob[64]==0xee);
  assert(ret==(kind>=2 ? -EBADMSG : kind==1 ? -EUCLEAN : 0));
  assert(r.ecc_stats.uncorrectable_errors==(kind>=2));
  if (kind==1) assert(r.ecc_stats.max_bitflips==1 && r.ecc_stats.corrected_bitflips==1 && m.ecc_stats.corrected==1);
  if (kind>=2) assert(m.ecc_stats.failed==1);
  checks++;
 }
 /* Cleanup failure after bytes arrived must not split the stale raw buffer
  * into a new caller page; exercise the real per-page lock/split wrapper. */
 struct h2fmi physical_fmi={0}; physical_fmi.bus[0]=bus;
 struct h2fmi_die physical_die={.fmi=&physical_fmi,.bus=0,.cau=0};
 struct mtd_info physical_mtd=mtd(); physical_mtd.priv=&physical_die;
 raw_cmd_error=raw_status_error=raw_data_error=0; raw_end_error=-ETIMEDOUT;
 memset(out,0xdd,sizeof(out)); memset(oob,0xee,sizeof(oob));
 assert(actual_h2fmi_read_page_at(&physical_mtd,0,out,oob,&raw_st)==-ETIMEDOUT);
 assert(out[0]==0xdd && out[H2FMI_PAGE_DATA-1]==0xdd && oob[0]==0xee);
 assert(!physical_fmi.bus[0].lock.locked);
 raw_end_error=0;
 assert(!actual_h2fmi_read_page_at(&physical_mtd,0,out,oob,&raw_st));
 assert(out[0]==0x5a && out[H2FMI_PAGE_DATA-1]==0x5a && oob[0]==0x5a);
 assert(!physical_fmi.bus[0].lock.locked);
 checks+=2;
 /* Bad page followed by good/refresh pages must preserve failure and consume
  * all requested bytes. Actual mtdchar loop sees both stats and final errno. */
 reset(); struct mtd_info m=mtd(); page_status[0]=PPN_ST_RETIRED; page_status[2]=PPN_ST_REFRESH;
 struct mtd_read_req r={.start=0,.len=3*H2FMI_PAGE_DATA,.ooblen=3*64,
  .usr_data=(uintptr_t)out,.usr_oob=(uintptr_t)oob,.mode=MTD_OPS_PLACE_OOB};
 assert(mtdchar_read_ioctl(&m,&r)==-EBADMSG && calls==3);
 assert(r.len==3*H2FMI_PAGE_DATA && r.ooblen==3*64 && r.ecc_stats.uncorrectable_errors==1);
 assert(out[H2FMI_PAGE_DATA]==0x81 && out[2*H2FMI_PAGE_DATA]==0x82);
 assert(r.ecc_stats.corrected_bitflips==1 && r.ecc_stats.max_bitflips==1);
 assert(m.ecc_stats.failed==1 && m.ecc_stats.corrected==1);
 checks++;
 /* Transport failure is fatal without pretending stale bytes were read. */
 reset(); m=mtd(); page_status[0]=-ETIMEDOUT; memset(out,0xdd,sizeof(out));
 r=(struct mtd_read_req){.len=H2FMI_PAGE_DATA,.ooblen=64,.usr_data=(uintptr_t)out,.usr_oob=(uintptr_t)oob};
 assert(mtdchar_read_ioctl(&m,&r)==-ETIMEDOUT && calls==1 && r.len==0 && r.ooblen==0 && out[0]==0xdd);
 checks++;
 reset(); m=mtd(); page_status[1]=PPN_ST_RETIRED; page_status[2]=PPN_ST_REFRESH; page_status[3]=0x41;
 r=(struct mtd_read_req){.len=4*H2FMI_PAGE_DATA,.ooblen=4*64,.usr_data=(uintptr_t)out,.usr_oob=(uintptr_t)oob};
 assert(mtdchar_read_ioctl(&m,&r)==-EBADMSG && calls==4 && r.len==4*H2FMI_PAGE_DATA);
 assert(r.ecc_stats.uncorrectable_errors==2 && r.ecc_stats.max_bitflips==1 && r.ecc_stats.corrected_bitflips==1);
 assert(m.ecc_stats.failed==2 && m.ecc_stats.corrected==1);
 checks++;
 /* OOB-only ECC failure also consumes metadata, so MEMREAD makes progress. */
 reset(); m=mtd(); page_status[0]=PPN_ST_RETIRED;
 r=(struct mtd_read_req){.ooblen=2*64,.usr_oob=(uintptr_t)oob};
 assert(mtdchar_read_ioctl(&m,&r)==-EBADMSG && calls==2 && r.len==0 && r.ooblen==128);
 assert(r.ecc_stats.uncorrectable_errors==1);
 checks++;
 /* Partial page data follows the same finite/error-signalling path. */
 reset(); m=mtd(); page_status[1]=0x41;
 r=(struct mtd_read_req){.start=123,.len=H2FMI_PAGE_DATA,.usr_data=(uintptr_t)out};
 assert(mtdchar_read_ioctl(&m,&r)==-EBADMSG && calls==2 && r.len==H2FMI_PAGE_DATA);
 assert(out[0]==0x80 && out[H2FMI_PAGE_DATA-124]==0x80 && out[H2FMI_PAGE_DATA-123]==0x81);
 checks++;
 /* Fatal second-page timeout: mtdchar must not publish bounce-buffer bytes
  * from the incomplete call, even though the driver read a valid prefix. */
 reset(); m=mtd(); page_status[1]=-ETIMEDOUT; memset(out,0xdd,sizeof(out));
 r=(struct mtd_read_req){.len=2*H2FMI_PAGE_DATA,.usr_data=(uintptr_t)out};
 assert(mtdchar_read_ioctl(&m,&r)==-ETIMEDOUT && calls==2 && r.len==0);
 assert(out[0]==0xdd && out[2*H2FMI_PAGE_DATA-1]==0xdd);
 checks++;
 reset(); m=mtd(); page_status[0]=0x41;
 struct mtd_oob_ops op={.len=H2FMI_PAGE_DATA,.datbuf=out};
 assert(h2fmi_mtd_read_oob(&m,0,&op)==-EBADMSG && op.retlen==H2FMI_PAGE_DATA && m.ecc_stats.failed==1);
 checks++;
 /* Cross-layer real MEMREAD -> actual iosnand backend: unreadable bytes must
  * never be copied into the caller, whereas retired status can be skipped. */
 for (unsigned kind=0; kind<4; kind++) {
  reset(); ioctl_mtd=mtd(); mtd_fds[0]=1000;
  const int statuses[]={PPN_ST_GOOD,PPN_ST_REFRESH,PPN_ST_RETIRED,0x41};
  page_status[0]=statuses[kind];
  uint8_t raw[PPN_PAGE_RAW], status;
  memset(raw,0xdd,sizeof(raw));
  int ret=mtd_backend_read(0,0,0,raw,sizeof(raw),&status);
  assert(calls==1);
  if (kind==3) assert(ret==-1 && errno==EBADMSG && status==0 && raw[0]==0xdd && raw[PPN_PAGE_RAW-1]==0xdd);
  else if (kind==2) assert(ret==0 && status==0x45 && raw[0]==0xff && raw[PPN_PAGE_RAW-1]==0xff);
  else assert(ret==0 && status==(kind==1 ? 0x42 : 0x40) && raw[0]==0x80 && raw[PPN_META_OFF]==0xa0);
  checks++;
 }
 printf("h2fmi: %u actual driver/MEMREAD loop cases passed\n",checks);
 return 0;
}
'''
    with tempfile.TemporaryDirectory(prefix='cascadia-h2fmi-read-') as td:
        source, binary = Path(td) / 'read.c', Path(td) / 'read'
        source.write_text(c)
        subprocess.run([os.environ.get('CC', 'gcc'), '-std=gnu11', '-O1', '-g',
                        '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                        str(source), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)
    return 0


if __name__ == '__main__':
    sys.exit(main())
