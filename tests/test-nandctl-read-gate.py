#!/usr/bin/env python3
"""Actual nandctl main with fake PPN/files/stdout APIs; no MMIO or files opened."""
from pathlib import Path
import argparse
import hashlib
import json
import os
import subprocess
import tempfile


BASE_BLOB = '27001e89faaae57551cb6f697c29fae175c11a10'
BASE_SHA256 = '31ffd55cc469a5bbf0948ebff736957eca8ac86808c9af52ebf04163491ac891'


def run_harness(root, source, historical):
    c = r'''
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include PPN_HEADER
volatile uint32_t *ppn_fmi[2],*ppn_fmc[2],*ppn_ecc[2],*ppn_pmgr;
static char output[65536];static size_t output_len;
static unsigned failmask,claimmask,fileopens,filewrites,decodes,open_calls,recover_calls;
static unsigned hexrows,idbytes,feature_calls[2];
static uint8_t filebytes[0x4400];static size_t filelen;
static int token;
static int fake_printf(const char *fmt,...)
{
 if(!strcmp(fmt,"  %04x:"))hexrows++;
 if(!strcmp(fmt," %02x"))idbytes++;
 va_list ap;va_start(ap,fmt);
 int n=vsnprintf(output+output_len,sizeof(output)-output_len,fmt,ap);va_end(ap);
 assert(n>=0&&(size_t)n<sizeof(output)-output_len);output_len+=n;return n;
}
static int fake_putchar(int v)
{assert(output_len+1<sizeof(output));output[output_len++]=v;output[output_len]=0;return v;}
static int fake_fprintf(FILE *f,const char *fmt,...)
{(void)fmt;assert(f==stderr);return 0;}
static void fake_perror(const char *s){(void)s;abort();}
static FILE *fake_fopen(const char *path,const char *mode)
{assert(!strcmp(path,"fixture.out")&&!strcmp(mode,"wb"));fileopens++;filelen=0;return(FILE*)&token;}
static size_t fake_fwrite(const void *ptr,size_t size,size_t count,FILE *file)
{assert(file==(FILE*)&token&&size*count<=sizeof(filebytes));filewrites++;filelen=size*count;memcpy(filebytes,ptr,filelen);return count;}
static int fake_fclose(FILE *file){assert(file==(FILE*)&token);return 0;}
static int fake_open(const char *path,int flags,...)
{assert(flags==(O_WRONLY|O_CREAT|O_TRUNC));assert(!strcmp(path,"fixture.out")||!strcmp(path,"fixture.out.st"));open_calls++;return !strcmp(path,"fixture.out")?100:101;}
static ssize_t fake_write(int fd,const void *buf,size_t size)
{assert(fd==100||fd==101);for(size_t i=0;i<size;i++)assert(((const uint8_t*)buf)[i]==0);return size;}
static int fake_close(int fd){assert(fd==100||fd==101);return 0;}
static void *fake_memcpy(void *to,const void *from,size_t len)
{assert(len==512);decodes++;return memcpy(to,from,len);}
static uint32_t fake_reg_read(volatile uint32_t *b,uint32_t off){(void)b;(void)off;abort();}
static void fake_reg_write(volatile uint32_t *b,uint32_t off,uint32_t v){(void)b;(void)off;(void)v;abort();}
#undef PPN_R
#undef PPN_W
#define PPN_R fake_reg_read
#define PPN_W fake_reg_write
static void fill(int bus,uint8_t *buf,uint32_t len,uint8_t *st)
{
 assert(bus==0||bus==1);*st=0x40;memset(buf,'Z',len);
 const char *marker=bus?"SYNTH_B1_SECRET!":"SYNTH_B0_SECRET!";
 memcpy(buf,marker,len<16?len:16);
}
int ppn_open(void){return 0;}
int ppn_ce_ok(int ce){assert(ce==0);return 1;}
int ppn_claim(int bus){assert(bus==0||bus==1);return !(claimmask&(1u<<bus));}
int ppn_gates_on(int bus){(void)bus;abort();}
int ppn_read_id(int bus,int ce,uint8_t address,uint8_t id[8])
{assert(bus==0&&ce==0&&address==0);uint8_t st;fill(bus,id,8,&st);return failmask&1?-1:0;}
int ppn_get_feature(int bus,int ce,uint16_t feat,uint8_t *buf,uint32_t len,uint8_t *st)
{assert(ce==0&&feat==0x9080&&len<=4096);feature_calls[bus]++;fill(bus,buf,len,st);return failmask&(1u<<bus)?-1:0;}
int ppn_params(int bus,int ce,uint8_t *buf,uint8_t *st)
{assert(bus==0&&ce==0);fill(bus,buf,512,st);return failmask&1?-1:0;}
int ppn_read_page(int bus,int ce,uint32_t row,uint8_t *buf,uint32_t len,uint8_t *st)
{assert(bus==0&&ce==0&&row>=256&&row<512&&len<=0x4400);fill(bus,buf,len,st);return failmask&1?-1:0;}
void ppn_recover(int bus){assert(bus==0);recover_calls++;}
int ppn_nand_reset(int bus,int ce){(void)bus;(void)ce;abort();}
int ppn_set_power_state(int bus,int ce,uint32_t ps,uint8_t *st)
{(void)bus;(void)ce;(void)ps;(void)st;abort();}
#define printf fake_printf
#define putchar fake_putchar
#define fprintf fake_fprintf
#define perror fake_perror
#define fopen fake_fopen
#define fwrite fake_fwrite
#define fclose fake_fclose
#define open fake_open
#define write fake_write
#define close fake_close
#define memcpy fake_memcpy
#define main nandctl_test_main
'''.replace('PPN_HEADER', json.dumps(str(root / 'tools/nand/ppn.h')))
    c += '#define HISTORICAL_CONTROL ' + str(int(historical)) + '\n'
    c += source.replace('#include "ppn.h"', '#include ' + json.dumps(str(root / 'tools/nand/ppn.h')))
    c += r'''
#undef main
#undef printf
#undef putchar
#undef fprintf
#undef perror
#undef fopen
#undef fwrite
#undef fclose
#undef open
#undef write
#undef close
#undef memcpy
static unsigned checks;
static void reset(unsigned failed,unsigned claimed)
{
 output_len=0;output[0]=0;failmask=failed;claimmask=claimed;
 fileopens=filewrites=decodes=open_calls=recover_calls=hexrows=idbytes=0;
 feature_calls[0]=feature_calls[1]=0;filelen=16;memcpy(filebytes,"PREVIOUS_CONTENT",16);
}
static void single(char **args,int argc,unsigned failure,unsigned claim,int file,int boot,int hex,int id,int recovery)
{
 reset(failure,claim);int result=nandctl_test_main(argc,args);
 assert(result==((failure||claim)?1:0));
 int publish=!claim&&(!failure||HISTORICAL_CONTROL);
 assert(fileopens==(unsigned)(publish&&file));assert(filewrites==fileopens);
 assert((decodes>0)==(publish&&boot));
 assert((hexrows>0)==(publish&&hex));assert((idbytes>0)==(publish&&(hex||id)));
 if(recovery)assert(recover_calls==!claim);
 if(!publish) {
  assert(!strstr(output,"SYNTH_B0"));
  assert(filelen==16&&!memcmp(filebytes,"PREVIOUS_CONTENT",16));
 }
 if(publish&&file)assert(filelen==(boot?1536:1029));
 checks++;
}
static void info(unsigned failed,unsigned claimed)
{
 reset(failed,claimed);char *args[]={"nandctl","ppninfo"};
 assert(nandctl_test_main(2,args)==(HISTORICAL_CONTROL?0:(failed||claimed?1:0)));
 for(unsigned bus=0;bus<2;bus++) {
  assert(feature_calls[bus]==!(claimed&(1u<<bus)));
  int publish=!(claimed&(1u<<bus))&&(!(failed&(1u<<bus))||HISTORICAL_CONTROL);
  assert((strstr(output,bus?"SYNTH_B1_SECRET!":"SYNTH_B0_SECRET!")!=NULL)==publish);
 }
 checks++;
}
int main(void)
{
 char *id[]={"nandctl","readid","0","0"};
 char *get[]={"nandctl","getfeat","0","0","0x9080","1029"};
 char *params[]={"nandctl","params","0","0"};
 char *page[]={"nandctl","page","0","0","256","fixture.out","1029"};
 char *boot[]={"nandctl","bootpage","0","0","256","fixture.out"};
 char *boothex[]={"nandctl","bootpage","0","0","256"};
 char *recover[]={"nandctl","recover","0"};
 for(unsigned scenario=0;scenario<3;scenario++) {
  unsigned failed=scenario==1,claim=scenario==2;
  single(id,4,failed,claim,0,0,0,1,0);
  single(get,6,failed,claim,0,0,1,0,0);
  single(params,4,failed,claim,0,0,1,0,0);
  single(page,7,failed,claim,1,0,0,0,0);
  single(boot,6,failed,claim,1,1,0,0,0);
  single(boothex,5,failed,claim,0,1,1,0,0);
  single(recover,3,failed,claim,0,0,0,0,1);
 }
 for(unsigned failures=0;failures<4;failures++)
  for(unsigned claims=0;claims<4;claims++)info(failures,claims);
 /* Dump already replaces failed bytes with explicit zero+status0 entries;
  * no failed read payload reaches these writes. Preserve that dump schema. */
 reset(1,0);char *dump[]={"nandctl","dump","0","0","0","1","1","fixture.out"};
 assert(nandctl_test_main(8,dump)==0&&open_calls==2);checks++;
 printf("PASS: %u actual NANDctl read/failure-publication cases (%s); all APIs fake\n",checks,
        HISTORICAL_CONTROL?"historical failed-buffer publication control":"current failure gates");
}
'''
    with tempfile.TemporaryDirectory(prefix='cascadia-nandctl-read-gate-') as work:
        src = Path(work) / 'harness.c'
        binary = Path(work) / 'harness'
        src.write_text(c)
        subprocess.run([os.environ.get('CC', 'gcc'), '-std=gnu11', '-O1', '-g',
                        '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                        str(src), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True, timeout=30)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument('--source', type=Path)
    args = parser.parse_args()
    root = args.root.resolve()
    source = (args.source or root / 'tools/nand/nandctl.c').read_text()
    blob = subprocess.run(['git', '-c', 'protocol.allow=never', '-C', str(root),
                           'cat-file', 'blob', BASE_BLOB], stdout=subprocess.PIPE,
                          check=True).stdout
    assert hashlib.sha256(blob).hexdigest() == BASE_SHA256
    run_harness(root, blob.decode('utf-8'), True)
    run_harness(root, source, False)


if __name__ == '__main__':
    main()
