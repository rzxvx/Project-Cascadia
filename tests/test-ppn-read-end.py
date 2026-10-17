#!/usr/bin/env python3
"""Production PIO read completions + iosnand cache; every register/API is fake."""
from pathlib import Path
import argparse
import json
import hashlib
import os
import re
import subprocess
import tempfile


def function(source, name):
    match = re.search(r'\n(?:static )?(?:int|void) ' + re.escape(name) + r'\(', source)
    if not match:
        raise ValueError('missing source function: ' + name)
    start = match.start() + 1
    brace = source.index('{', match.end())
    end, depth = brace + 1, 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


def run_harness(root, source, historical):
    c = r'''
#define main iosnand_device_main
#define IOSNAND_IOCTL fake_ioctl
static int fake_ioctl(int, unsigned long, void *);
#include IOSNAND_SOURCE
#undef main
#include <assert.h>
static int fake_ioctl(int fd, unsigned long op, void *arg) { (void)fd;(void)op;(void)arg;abort(); }
int ppn_open(void) { abort(); }
int ppn_claim(int bus) { (void)bus;abort(); }
int ppn_lock(int bus) { (void)bus;abort(); }

/* Tokens are never dereferenced. R/W are function fakes, not MMIO accesses. */
static uint32_t tokens[4];
static volatile uint32_t *fmi[2] = { &tokens[0], &tokens[1] };
static volatile uint32_t *fmc[2] = { &tokens[2], &tokens[3] };
static unsigned selected, deselected, stopped, end_calls, reset_calls, data_calls;
static int command_error, status_error, data_error, end_error;
static uint8_t page_status;
static uint32_t fake_read(volatile uint32_t *base, uint32_t off)
{
 assert(base==fmc[0]);
 if(off==FMC_ON) return 5;
 assert(off==FMC_IF_CTRL); return 0x100001;
}
static void fake_write(volatile uint32_t *base, uint32_t off, uint32_t value)
{
 if(base==fmc[0] && off==FMC_CE_CTRL) {
  if(value==1) selected++; else {assert(value==0);deselected++;}
 } else {assert(base==fmi[0]&&off==FMI_CONTROL&&value==6);stopped++;}
}
#define R fake_read
#define W fake_write
static void clear_irqs(int bus) { assert(bus==0); }
static void device_reset(int bus,uint32_t on,uint32_t ctrl)
{ assert(bus==0&&on==5&&ctrl==0x100001);reset_calls++; }
static int ppn_cmd1(int bus,uint8_t op)
{ assert(bus==0&&op==0x77);end_calls++;return end_error; }
static int ppn_cmd_addr_cmd(int bus,uint8_t op,uint8_t op2,uint32_t address,int naddr)
{
 assert(bus==0);
 if(op==0x0a) assert(op2==0x37&&address==256&&naddr==3);
 else if(op==0xee) assert(op2==0xe7&&address==0x9080&&naddr==2);
 else assert(op==0x92&&op2==0x97&&address==0&&naddr==1);
 return command_error;
}
static int ppn_status(int bus,uint8_t *status)
{assert(bus==0);*status=page_status;return status_error;}
static int ppn_data_out(int bus,uint8_t *buf,uint32_t length)
{
 assert(bus==0);data_calls++;
 /* Deliver valid page bytes even when the subsequent end command fails. */
 memset(buf,0x5a,length);
 if(length==PPN_PAGE_RAW) {
  uint8_t *meta=buf+PPN_META_OFF;memset(meta,0,16);meta[0]=1;meta[2]=1;
 }
 return data_error;
}
'''.replace('IOSNAND_SOURCE', json.dumps(str(root / 'tools/nand/iosnand.c')))
    c = '#define HISTORICAL_CONTROL ' + str(int(historical)) + '\n' + c
    for name in ('ppn_end', 'ppn_get_feature', 'ppn_params', 'ppn_read_page'):
        c += '\n' + function(source, name) + '\n'
    c += r'''
static unsigned checks;
static void reset(void)
{
 selected=deselected=stopped=end_calls=reset_calls=data_calls=0;
 command_error=status_error=data_error=end_error=0;page_status=0x40;
}
static void run(unsigned method,int expected)
{
 uint8_t buf[PPN_PAGE_RAW],status=0xa5;int ret;
 if(method==0) ret=ppn_read_page(0,0,256,buf,16,&status);
 else if(method==1) ret=ppn_get_feature(0,0,0x9080,buf,16,&status);
 else ret=ppn_params(0,0,buf,&status);
 if(ret!=expected) fprintf(stderr,"method%u ret%d expected%d end%d data%d\n",method,ret,expected,end_error,data_error);
 assert(ret==expected&&selected==1&&deselected==1&&end_calls==1);
 assert(reset_calls==(method==0)&&stopped==(method!=0));
 if(!command_error && !status_error && (method!=2||page_status==0x40))assert(data_calls==1);
 else assert(!data_calls);
 checks++;
}
static void cache_case(int fail)
{
 reset();use_mtd=0;end_error=fail?-ETIMEDOUT:0;
 memset(pcache,0,sizeof(pcache));pcache_next=0;
 uint8_t out[4096];memset(out,0xa5,sizeof(out));
 int ret=read_lba(0,out);
 assert(selected==1&&deselected==1&&end_calls==1&&reset_calls==1);
 if(fail && !HISTORICAL_CONTROL) {
  assert(ret==-1&&pcache[0].key==0);
  for(unsigned i=0;i<sizeof(out);i++) assert(out[i]==0xa5);
 } else {
  assert(ret==0&&pcache[0].key==PPN_PAGES+1);
  for(unsigned i=0;i<sizeof(out);i++)assert(out[i]==0x5a);
 }
 checks++;
}
int main(void)
{
 for(unsigned method=0;method<3;method++) {
  reset();run(method,0);
  reset();end_error=-ETIMEDOUT;run(method,HISTORICAL_CONTROL?0:-ETIMEDOUT);
  reset();command_error=-EIO;end_error=-ETIMEDOUT;run(method,-1);
  reset();status_error=-EIO;end_error=-ETIMEDOUT;run(method,-1);
  reset();data_error=-EIO;run(method,-EIO);
  reset();data_error=-EIO;end_error=-ETIMEDOUT;run(method,-EIO);
 }
 reset();page_status=0x42;run(0,0);
 reset();page_status=0x42;run(1,0);
 reset();page_status=0x42;end_error=-ETIMEDOUT;run(2,-1);
 l2p=calloc(1,sizeof(*l2p));pages=calloc(PPN_DIES,sizeof(*pages));assert(l2p&&pages);
 l2p[0]=PPN_PAGES*4;pages[0][1][0].seq[0][0]=1;
 cache_case(1);cache_case(0);
 free(pages);pages=NULL;free(l2p);l2p=NULL;
 printf("PASS: %u actual PIO end/cleanup/cache cases (%s); only fake register/APIs\n",checks,
        HISTORICAL_CONTROL?"historical ignore-end/cache-publication control":"current failure propagation");
 return 0;
}
'''
    with tempfile.TemporaryDirectory(prefix='cascadia-ppn-read-end-') as tmp:
        src = Path(tmp) / 'harness.c'
        binary = Path(tmp) / 'harness'
        src.write_text(c)
        subprocess.run([os.environ.get('CC', 'gcc'), '-std=gnu11', '-O1', '-g',
                        '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections',
                        '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                        str(src), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True, timeout=20)


# Original pre-fix local Git object; never fetch or accept a replacement.
BASE_BLOB = '9eecbefcfe82e0d304741fb66339af8fc30b681b'
BASE_SHA256 = '70642b8f03ace401971c748ab942e8aa7bbb3f25660d964fcf04a1ab69abf3a5'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument('--source', type=Path)
    args = parser.parse_args()
    root = args.root.resolve()
    source = (args.source or root / 'tools/nand/ppn.c').read_text()
    blob = subprocess.run(['git', '-c', 'protocol.allow=never', '-C', str(root),
                           'cat-file', 'blob', BASE_BLOB], check=True,
                          stdout=subprocess.PIPE).stdout
    assert hashlib.sha256(blob).hexdigest() == BASE_SHA256
    baseline = blob.decode('utf-8')
    def allowlist(text):
        start = text.index('static const uint8_t READ_ONLY_OPS')
        return text[start:text.index('};', start) + 2]
    assert allowlist(source) == allowlist(baseline), 'read-only allowlist changed'
    for name in ('ppn_set_power_state', 'ppn_cmd_e7', 'nand_cmd2'):
        assert function(source, name) == function(baseline, name), name + ' changed'
    print('PASS: pinned local historical Git blob and unchanged setter/allowlist', flush=True)
    run_harness(root, baseline, True)
    run_harness(root, source, False)


if __name__ == '__main__':
    main()
