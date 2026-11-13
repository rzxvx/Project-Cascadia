#!/usr/bin/env python3
"""Extract report validation/init code; mock transport, not protocol checks."""
from pathlib import Path
import subprocess
import tempfile

s=(Path(__file__).resolve().parents[1]/'extras/apple_z2_j171.c').read_text()
def extract(name):
    start=s.index('static int '+name+'(')
    p=s.index('{',start); depth=1; end=p+1
    while depth:
        depth+=(s[end]=='{')-(s[end]=='}'); end+=1
    return s[start:end]
header=r'''
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>
#include <assert.h>
typedef uint8_t u8; typedef uint16_t u16;
#define APPLE_Z2_CMD_REPORT_INFO 0xe3
#define APPLE_Z2_CMD_CTRL_READ_SHORT 0xe6
#define APPLE_Z2_CMD_CTRL_READ_LONG 0xe7
#define APPLE_Z2_CMD_SIZE 16
#define APPLE_Z2_CMD_LAST 0xe1
#define APPLE_Z2_CMD_CTRL_WRITE_SHORT 0xe4
#define APPLE_Z2_REPORT_SURFACE 0xd9
#define APPLE_Z2_SURFACE_DESCRIPTOR_SIZE 16
struct apple_z2 {u8 rx_buf[16]; void *spidev; bool j171;};
#define dev_dbg(...) ((void)0)
#define dev_info(...) ((void)0)
static u16 get_unaligned_le16(const u8 *b){return b[0]|b[1]<<8;}
static bool apple_z2_z2_checksum_valid(const u8*b,unsigned len){unsigned sum=0;for(unsigned i=0;i<len-2;i++)sum+=b[i];return (u16)sum==get_unaligned_le16(b+len-2);}
static void checksum(u8*b,unsigned len){unsigned sum=0;for(unsigned i=0;i<len-2;i++)sum+=b[i];b[len-2]=sum;b[len-1]=sum>>8;}
static unsigned infos,reads,read73;static u16 advertised=16;static int injected;
static int apple_z2_z2_cmd_locked(struct apple_z2*z,u8 cmd,u8 report,const char*tag){
 (void)tag;assert(cmd==0xe3);infos++;
 u16 len=report==0xd9?16:report==0xdb?125:advertised;
 memset(z->rx_buf,0,16);z->rx_buf[0]=cmd;z->rx_buf[1]=report;
 z->rx_buf[3]=len;z->rx_buf[4]=len>>8;checksum(z->rx_buf,16);return 0;
}
static int apple_z2_read_report_locked(struct apple_z2*z,u8 report,u16 len,const char*tag){
 (void)z;(void)tag;reads++;
 if(report==0x73){read73++;assert(len==advertised);}
 return injected;
}
static int apple_z2_store_surface_descriptor(struct apple_z2*z,u16 len){(void)z;assert(len==16);return 0;}
'''
main=r'''
int main(void){
 struct apple_z2 z={.j171=true};
 assert(!apple_z2_j172_init_reads_locked(&z));assert(reads==7&&infos==6&&read73==3);
 advertised=8; assert(!apple_z2_j172_init_reads_locked(&z));
 advertised=512;assert(!apple_z2_j172_init_reads_locked(&z));
 advertised=513;assert(apple_z2_j172_init_reads_locked(&z)==-EPROTO);
 advertised=0;assert(apple_z2_j172_init_reads_locked(&z)==-EPROTO);
 z.j171=false;advertised=16;assert(apple_z2_j172_init_reads_locked(&z)==-EPROTO);
 advertised=8;assert(!apple_z2_j172_init_reads_locked(&z));
 z.j171=true;injected=-EIO;assert(apple_z2_j172_init_reads_locked(&z)==-EIO);
 u8 b[517]={0};unsigned lengths[]={1,8,11,12,16,125,512};
 for(unsigned i=0;i<sizeof(lengths)/sizeof(lengths[0]);i++){
  unsigned n=lengths[i],size=n<=11?16:n+5;
  memset(b,0,sizeof(b));b[0]=n<=11?0xe6:0xe7;b[1]=0x73;checksum(b,size);
  assert(!apple_z2_j171_validate_report(b,size,0x73,n));
  assert(apple_z2_j171_validate_report(b,size-1,0x73,n)==-EMSGSIZE);
  b[0]^=1;assert(apple_z2_j171_validate_report(b,size,0x73,n)==-EPROTO);b[0]^=1;
  b[1]^=1;assert(apple_z2_j171_validate_report(b,size,0x73,n)==-EPROTO);b[1]^=1;
  b[2]=1;assert(apple_z2_j171_validate_report(b,size,0x73,n)==-EPROTO);b[2]=0;
  b[size-1]^=1;assert(apple_z2_j171_validate_report(b,size,0x73,n)==-EBADMSG);
 }
 assert(apple_z2_j171_validate_report(b,16,0x73,0)==-EMSGSIZE);
 assert(apple_z2_j171_validate_report(b,518,0x73,513)==-EMSGSIZE);
 memset(b,0,16);b[0]=0xe1;b[6]=0xe4;b[7]=0xbf;checksum(b,16);
 assert(!apple_z2_j171_validate_write(b,16,0xbf));
 assert(apple_z2_j171_validate_write(b,15,0xbf)==-EPROTO);
 b[0]=0xe3;checksum(b,16);assert(apple_z2_j171_validate_write(b,16,0xbf)==-EPROTO);
 b[0]=0xe1;b[8]=2;checksum(b,16);assert(apple_z2_j171_validate_write(b,16,0xbf)==-EIO);
 b[8]=0;b[7]=0x9d;checksum(b,16);assert(apple_z2_j171_validate_write(b,16,0xbf)==-EPROTO);
 b[7]=0xbf;b[6]=0xe5;checksum(b,16);assert(apple_z2_j171_validate_write(b,16,0xbf)==-EPROTO);
 b[6]=0xe4;checksum(b,16);b[15]^=1;assert(apple_z2_j171_validate_write(b,16,0xbf)==-EBADMSG);
 puts("PASS: advertised report73 lengths, legacy restriction, failures, short/long headers, bounds and checksums");
}
'''
with tempfile.TemporaryDirectory(prefix='j171-reports-') as d:
    p=Path(d)/'test.c'
    p.write_text(header+extract('apple_z2_j171_validate_report')+
                 extract('apple_z2_j171_validate_write')+
                 extract('apple_z2_read_report_info_locked')+
                 extract('apple_z2_j172_init_reads_locked')+main)
    subprocess.run(['cc','-Wall','-Wextra','-Werror','-fsanitize=undefined,bounds',str(p),'-o',str(p.with_suffix(''))],check=True)
    subprocess.run([str(p.with_suffix(''))],check=True)
