#!/usr/bin/env python3
"""Compile and exercise the actual dispatcher with bounded TLV buffers."""
from pathlib import Path
import subprocess
import tempfile
s=(Path(__file__).resolve().parents[1]/'extras/apple_z2_j171.c').read_text()
a=s.index('static void apple_z2_dispatch_frame(')
b=s.index('static void apple_z2_z2_checksum(',a)
header=r'''
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <assert.h>
#include <stdio.h>
typedef uint8_t u8;typedef uint16_t u16;
#define APPLE_Z2_FRAME_INPUT_REPORT 0x90
#define APPLE_Z2_INTERFACE_GRAPE 2
#define APPLE_Z2_HID_REPORT_ID 0x76
struct apple_z2{bool j171;u8 frame_trace_count,grape_trace_count;void*spidev;};
#define dev_warn_ratelimited(...) ((void)0)
#define dev_info(...) ((void)0)
static u16 get_unaligned_le16(const u8*p){return p[0]|p[1]<<8;}
static unsigned dispatched;
static void apple_z2_parse_touches(struct apple_z2*z,const u8*p,size_t n)
{(void)z;assert(p[0]==0x76&&n==1);dispatched++;}
'''
main=r'''
int main(void){struct apple_z2 z={.j171=true};
 const u8 one[]={0x90,2,1,0,0x76};
 const u8 batch[]={0x90,8,1,0,0x5a,0x90,3,1,0,0x76,0x90,2,1,0,0x76};
 const u8 empty[]={0x90,2,0,0,0x90,2,1,0,0x76};
 const u8 bad[]={0x90,2,0xff,0xff,0x76};
 const u8 tail[]={0x90,2,1,0,0x76,0x90};
 apple_z2_dispatch_frame(&z,one,sizeof(one));assert(dispatched==1);
 apple_z2_dispatch_frame(&z,batch,sizeof(batch));assert(dispatched==3);
 apple_z2_dispatch_frame(&z,empty,sizeof(empty));assert(dispatched==4);
 apple_z2_dispatch_frame(&z,bad,sizeof(bad));assert(dispatched==4);
 apple_z2_dispatch_frame(&z,tail,sizeof(tail));assert(dispatched==5);
 for(unsigned n=0;n<4;n++)apple_z2_dispatch_frame(&z,one,n);
 assert(dispatched==5);
 z.j171=false;
 apple_z2_dispatch_frame(&z,batch,sizeof(batch));assert(dispatched==5);
 apple_z2_dispatch_frame(&z,one,sizeof(one));assert(dispatched==6);
 puts("PASS: single/batched/empty TLVs, mixed masks, malformed tails, bounds, legacy behavior");
}
'''
with tempfile.TemporaryDirectory(prefix='j171-dispatch-') as d:
    p=Path(d)/'test.c';p.write_text(header+s[a:b]+main)
    subprocess.run(['cc','-Wall','-Wextra','-Werror','-fsanitize=undefined,bounds',str(p),'-o',str(p.with_suffix(''))],check=True)
    subprocess.run([str(p.with_suffix(''))],check=True)
