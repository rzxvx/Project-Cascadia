#!/usr/bin/env python3
"""Real PIO data-out lengths/partial-error bytes; register/API fakes only."""
from pathlib import Path
import argparse
import json
import hashlib
import os
import re
import subprocess
import tempfile


def function(source, name):
    match = re.search(r'\nstatic int ' + name + r'\(', source)
    if not match:
        raise ValueError('missing ' + name)
    start = match.start() + 1
    brace = source.index('{', match.end())
    end, depth = brace + 1, 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


def run_harness(root, source, historical):
    c = r'''
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include PPN_HEADER
static uint32_t token;
static volatile uint32_t *fmi[2]={&token,NULL};
static unsigned command_calls, pio_calls, fail_call, transferred;
static int ppn_cmd1(int bus,uint8_t command)
{ assert(bus==0&&command==0x7a);command_calls++;return 0; }
static void fake_write(volatile uint32_t *base,uint32_t off,uint32_t value)
{
 assert(base==fmi[0]);
 if(off==FMI_CONFIG) assert(value==5);
 else if(off==FMI_PIO_CONFIG) assert((value&255)==1&&value>>8<=1024);
 else assert(off==FMI_CONTROL&&value==3);
}
#define W fake_write
static uint8_t expected(unsigned index) {return index%251;}
static int pio_read(int bus,uint32_t *dst,uint32_t bytes)
{
 assert(bus==0&&bytes>=4&&bytes<=1024&&!(bytes&3));
 if(++pio_calls==fail_call)return -ETIMEDOUT;
 /* Write ONLY the bytes this real PIO transfer delivered. Leftover stack
  * words naturally retain a previous full-chunk transfer, no poison fake. */
 for(unsigned i=0;i<bytes;i++)((uint8_t *)dst)[i]=expected(transferred+i);
 transferred+=bytes;return 0;
}
'''.replace('PPN_HEADER', json.dumps(str(root / 'tools/nand/ppn.h')))
    c = '#define HISTORICAL_CONTROL ' + str(int(historical)) + '\n' + c
    c += '\n' + function(source, 'ppn_data_out') + '\n'
    c += r'''
#define MAX_BUFFER 0x4400
static unsigned checks;
static void reset(void)
{ command_calls=pio_calls=transferred=fail_call=0; }
static void success(unsigned len)
{
 uint8_t guarded[MAX_BUFFER+16];memset(guarded,0xa5,sizeof(guarded));reset();
 assert(!ppn_data_out(0,guarded+8,len)&&command_calls==1);
 assert(transferred==((len+3)&~3u));
 for(unsigned i=0;i<8;i++)assert(guarded[i]==0xa5);
 for(unsigned i=0;i<len;i++)assert(guarded[8+i]==expected(i));
 for(unsigned i=8+len;i<sizeof(guarded);i++)assert(guarded[i]==0xa5);
 checks++;
}
static void failed_tail(unsigned len,unsigned fail,int old_stale)
{
 uint8_t guarded[MAX_BUFFER+16];memset(guarded,0xa5,sizeof(guarded));reset();fail_call=fail;
 assert(ppn_data_out(0,guarded+8,len)==-1&&command_calls==1&&pio_calls==fail);
 for(unsigned i=0;i<8;i++)assert(guarded[i]==0xa5);
 for(unsigned i=0;i<transferred;i++)assert(guarded[8+i]==expected(i));
 unsigned stale=0;
 for(unsigned i=transferred;i<len;i++) {
  if(HISTORICAL_CONTROL&&old_stale){stale+=guarded[8+i]!=0xa5;continue;}
  if(guarded[8+i]!=0xa5)fprintf(stderr,"len%u failedPIO%u: byte%u=%02x beyond delivered%u (expected untoucheda5)\n",len,fail,i,guarded[8+i],transferred);
  assert(guarded[8+i]==0xa5);
 }
 if(HISTORICAL_CONTROL&&old_stale)assert(stale==len-transferred);
 for(unsigned i=8+len;i<sizeof(guarded);i++)assert(guarded[i]==0xa5);
 checks++;
}
int main(void)
{
 /* Exhaust every accepted nandctl getfeat/page length, not hypothetical
  * oversized API inputs. iosnand/driver lengths are included in this range. */
 for(unsigned len=0;len<=MAX_BUFFER;len++)success(len);
 failed_tail(1029,3,1);failed_tail(1030,3,1);failed_tail(1031,3,1);
 failed_tail(2045,3,1);failed_tail(2046,3,1);failed_tail(2047,3,1);
 failed_tail(4095,5,1);failed_tail(MAX_BUFFER-1,18,1);
 failed_tail(1024,1,0);failed_tail(1025,2,0);
 printf("PASS: %u real data-out length/buffer/failure cases (%s); fake PIO and no MMIO\n",checks,
        HISTORICAL_CONTROL?"historical stale-tail control":"current exact-delivery copy");
}
'''
    with tempfile.TemporaryDirectory(prefix='cascadia-ppn-tail-') as work:
        src = Path(work) / 'harness.c'
        binary = Path(work) / 'harness'
        src.write_text(c)
        subprocess.run([os.environ.get('CC', 'gcc'), '-std=gnu11', '-O1', '-g',
                        '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                        str(src), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True, timeout=30)


BASE_BLOB = '9eecbefcfe82e0d304741fb66339af8fc30b681b'
BASE_SHA256 = '70642b8f03ace401971c748ab942e8aa7bbb3f25660d964fcf04a1ab69abf3a5'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument('--source', type=Path)
    args = parser.parse_args()
    root = args.root.resolve()
    current = (args.source or root / 'tools/nand/ppn.c').read_text()
    blob = subprocess.run(['git', '-c', 'protocol.allow=never', '-C', str(root),
                           'cat-file', 'blob', BASE_BLOB], stdout=subprocess.PIPE,
                          check=True).stdout
    assert hashlib.sha256(blob).hexdigest() == BASE_SHA256
    run_harness(root, blob.decode('utf-8'), True)
    run_harness(root, current, False)


if __name__ == '__main__':
    main()
