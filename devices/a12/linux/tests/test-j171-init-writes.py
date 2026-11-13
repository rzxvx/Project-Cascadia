#!/usr/bin/env python3
"""Exercise actual initialization sequencing with a mocked report transport."""
from pathlib import Path
import subprocess
import tempfile
s=(Path(__file__).resolve().parents[1]/'extras/apple_z2_j171.c').read_text()
a=s.index('static int apple_z2_j172_init_writes_locked(')
b=s.index('static irqreturn_t apple_z2_irq(',a)
h=r'''
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
typedef uint8_t u8;typedef uint16_t u16;
#define APPLE_Z2_CMD_WAKE 0xee
#define APPLE_Z2_STATE_RUNTIME 8
#define APPLE_Z2_STATE_REPORTS_READY 9
#define dev_info(...) ((void)0)
#define dev_dbg(...) ((void)0)
struct apple_z2{bool j171;u8 rx_buf[16];void*spidev;};
static unsigned bf_reads,bf_writes,n9d;static int fail_read;static u8 saved_bf[4];
static int apple_z2_z2_cmd_locked(struct apple_z2*z,u8 c,u8 r,const char*t)
{(void)z;(void)r;(void)t;assert(c==0xee);return 0;}
static void usleep_range(unsigned a,unsigned b){assert(a<=b);}
static int apple_z2_read_report_info_locked(struct apple_z2*z,u8 r,u8*status,u16*len,const char*t)
{(void)z;(void)t;*status=r==0xaf?2:0;*len=r==0x9d?8:r==0xbf?4:0;return 0;}
static int apple_z2_read_report_locked(struct apple_z2*z,u8 r,u16 n,const char*t)
{(void)t;assert(r==0xbf&&n==4);bf_reads++;memcpy(z->rx_buf+3,"\x9d\x31\x09\0",4);return fail_read;}
static int apple_z2_write_report_locked(struct apple_z2*z,u8 r,const u8*d,u8 n,const char*t)
{(void)z;(void)t;if(r==0x9d){assert(n==8&&d[0]==1);n9d++;}else{assert(r==0xbf&&n==4);bf_writes++;memcpy(saved_bf,d,4);}return 0;}
static int apple_z2_advance_protocol(struct apple_z2*z,int a,int b)
{(void)z;assert(a==8&&b==9);return 0;}
'''
m=r'''
int main(void){struct apple_z2 z={.j171=true};
 assert(!apple_z2_j172_init_writes_locked(&z));
 assert(bf_reads==1&&bf_writes==1&&n9d==2&&!memcmp(saved_bf,"\x9d\x31\x09\0",4));
 z.j171=false;assert(!apple_z2_j172_init_writes_locked(&z));
 assert(bf_reads==1&&bf_writes==2&&!memcmp(saved_bf,"\x9d\x81\x09\0",4));
 z.j171=true;fail_read=-EIO;
 assert(apple_z2_j172_init_writes_locked(&z)==-EIO&&bf_writes==2);
 puts("PASS: J171 BF read/preserve, legacy constant unchanged, read failure prevents write");}
'''
with tempfile.TemporaryDirectory(prefix='j171-init-') as d:
    p=Path(d)/'test.c';p.write_text(h+s[a:b]+m)
    subprocess.run(['cc','-Wall','-Wextra','-Werror','-fsanitize=undefined,bounds',str(p),'-o',str(p.with_suffix(''))],check=True)
    subprocess.run([str(p.with_suffix(''))],check=True)
