#!/usr/bin/env python3
"""Exercise extracted J171 receive code with mocked SPI and guarded buffers."""
from pathlib import Path
import subprocess
import tempfile
s=(Path(__file__).resolve().parents[1] / 'extras/apple_z2_j171.c').read_text()
a=s.index('static void apple_z2_z2_checksum(u8 *cmd, unsigned int len)\n{');b=s.index('static void apple_z2_reset_protocol(',a)
functions=s[a:b]
header=r'''
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <assert.h>
typedef uint8_t u8; typedef uint16_t u16;
#define APPLE_Z2_CMD_SIZE 16
#define APPLE_Z2_RX_BUF_SIZE 4000
#define APPLE_Z2_CMD_READ_INTERRUPT_DATA 0xeb
#define APPLE_Z2_REPLY_INTERRUPT_DATA 0xe1
#define min_t(t,a,b) ((t)(a)<(t)(b)?(t)(a):(t)(b))
static u16 get_unaligned_le16(const void *p){const u8*b=p;return b[0]|b[1]<<8;}
static void put_unaligned_le16(u16 n,void*p){u8*b=p;b[0]=n;b[1]=n>>8;}
struct device {int unused;};struct spi_device{struct device dev;};
struct spi_transfer{const void*tx_buf;void*rx_buf;unsigned len;unsigned bits_per_word;};
struct apple_z2{bool runtime_frame_valid,runtime_idle_valid,j171,j172,runtime_resync_needed;u8 runtime_trace_count;u8*tx_buf,*rx_buf;struct spi_device*spidev;int index_parity;u16 max_packet_size;size_t rx_buf_size;};
#define dev_info(d,...) ((void)(d))
#define dev_warn(d,...) ((void)(d))
#define dev_warn_ratelimited(d,...) ((void)(d))
static void apple_z2_trace_xfer(struct apple_z2*z,bool d,unsigned n,int e){(void)z;(void)d;(void)n;(void)e;}
static void apple_z2_capture_failure(struct apple_z2*z,int e){(void)z;(void)e;}
static void *memchr_inv(const void*p,int c,size_t n){const u8*b=p;for(size_t i=0;i<n;i++)if(b[i]!=(u8)c)return (void*)(b+i);return NULL;}
static u8 reply[16],frame[65536];static unsigned calls,wire_len,dispatches,dispatched_len;static int fail_transfer;
static unsigned events[32],event_count,sleeps;
static void msleep(unsigned n){assert(n==1);sleeps++;}
static int spi_sync_transfer(struct spi_device*d,struct spi_transfer*x,unsigned n){
 (void)d;assert(n==1);calls++;
 if(event_count){
  assert(calls<=event_count);unsigned e=events[calls-1];
  if(e==6)return -EIO;
  if(e==4||e==5){assert(x->len==get_unaligned_le16(reply+1)+5u);wire_len=x->len;memcpy(x->rx_buf,frame,x->len);if(e==5)((u8*)x->rx_buf)[4]^=1;return 0;}
  assert(x->len==16);assert(((const u8*)x->tx_buf)[0]==0xeb);assert(((const u8*)x->tx_buf)[1]==1);
  memcpy(x->rx_buf,reply,16);
  if(e==2)((u8*)x->rx_buf)[0]=0xe3;
  if(e==3)((u8*)x->rx_buf)[14]^=1;
  return 0;
 }
 if(fail_transfer)return -EIO;
 if(calls==1){assert(x->len==16);memcpy(x->rx_buf,reply,16);}
 else{assert(calls==2);assert(x->len<=65535);wire_len=x->len;memcpy(x->rx_buf,frame,x->len);}
 return 0;
}
static int spi_read(struct spi_device*d,void*p,unsigned n){struct spi_transfer x={.rx_buf=p,.len=n};return spi_sync_transfer(d,&x,1);}
static void apple_z2_post_z2_xfer_delay(struct apple_z2*z){(void)z;}
static void apple_z2_dispatch_frame(struct apple_z2*z,const u8*p,size_t n){(void)z;(void)p;dispatches++;dispatched_len=n;}
'''
main=r'''
static u8 tx[16],rx[65536+16];static struct spi_device dev;
static struct apple_z2 z;
static void reset_case(unsigned pending){
 memset(&z,0,sizeof(z));z.j171=z.j172=true;z.runtime_trace_count=8;
 z.tx_buf=tx;z.rx_buf=rx;z.spidev=&dev;z.max_packet_size=8928;z.rx_buf_size=8928;
 memset(rx,0xcc,sizeof(rx));memset(reply,0,16);reply[0]=0xe1;put_unaligned_le16(pending,reply+1);apple_z2_z2_checksum(reply,16);
 memset(frame,0,sizeof(frame));frame[0]=0xea;frame[1]=1;put_unaligned_le16(pending,frame+2);
 frame[4]=-(frame[0]+frame[1]+frame[2]+frame[3]);
 calls=wire_len=dispatches=dispatched_len=event_count=sleeps=0;fail_transfer=0;
}
static void canary(void){for(unsigned i=z.rx_buf_size;i<sizeof(rx);i++)assert(rx[i]==0xcc);}
int main(void){
 size_t n=123;
 reset_case(4512);assert(!apple_z2_read_packet(&z));assert(calls==2 && wire_len==4517);assert(z.runtime_frame_valid && dispatches==1 && dispatched_len==4510 && z.index_parity==1);canary();
 reset_case(8923);assert(!apple_z2_read_packet(&z));assert(wire_len==8928);canary();
 reset_case(8924);assert(apple_z2_read_packet(&z)==-EMSGSIZE && calls==1 && !dispatches && !z.index_parity);canary();
 reset_case(65535);assert(apple_z2_read_packet(&z)==-EMSGSIZE && calls==1);canary();
 reset_case(4512);z.rx_buf_size=4000;assert(apple_z2_read_packet(&z)==-EMSGSIZE && calls==1);canary();
 reset_case(4512);reply[1]^=1;assert(apple_z2_read_packet(&z)==-EBADMSG && calls==1 && !dispatches && !z.index_parity);canary();
 reset_case(4512);reply[0]=0xe2;apple_z2_z2_checksum(reply,16);assert(apple_z2_read_packet(&z)==-EPROTO && calls==1);canary();
 reset_case(0);assert(!apple_z2_read_packet(&z) && calls==1 && !wire_len && !dispatches && z.runtime_idle_valid && !z.runtime_frame_valid && z.index_parity==1);canary();
 reset_case(2);assert(!apple_z2_read_packet(&z) && wire_len==7 && z.runtime_frame_valid);canary();
 reset_case(3);frame[5]=0x12;put_unaligned_le16(0x12,frame+6);assert(!apple_z2_read_packet(&z) && wire_len==8);canary();
 reset_case(10);frame[5]=1;assert(apple_z2_read_packet(&z)==-EPROTO && calls==2 && !dispatches && !z.index_parity);canary();
 reset_case(10);frame[1]=2;frame[4]=-(frame[0]+frame[1]+frame[2]+frame[3]);assert(apple_z2_read_packet(&z)==-EPROTO && !dispatches && !z.index_parity);canary();
 reset_case(10);frame[4]^=1;assert(apple_z2_read_packet(&z)==-EPROTO && !dispatches);canary();
 reset_case(10);put_unaligned_le16(100,frame+2);frame[4]=-(frame[0]+frame[1]+frame[2]+frame[3]);assert(apple_z2_read_packet(&z)==-EPROTO && !dispatches);canary();
 reset_case(10);fail_transfer=1;assert(apple_z2_read_packet(&z)==-EIO && calls==1 && !z.index_parity);canary();
 reset_case(10);assert(apple_z2_j171_result_length(reply,15,8928,8928,&n)==-EPROTO && n==0);
 assert(apple_z2_j171_result_length(reply,16,0,8928,&n)==-EPROTO);
 /* Non-J171 retains its old four-byte alignment and fixed-size gate. */
 reset_case(10);z.j171=false;reply[14]^=1;assert(!apple_z2_read_packet(&z) && calls==2 && wire_len==16);canary();
 reset_case(4512);z.j171=false;assert(apple_z2_read_packet(&z)==-EMSGSIZE && calls==1);canary();
 puts("PASS 19 extracted-C receive tests: exact/odd wire lengths, advertised/allocation bounds, zero-idle, header/payload checksums, marker/counter, inner-length bounds, transfer error, non-J171 regression and canaries");
}
'''
# Existing cases exercise one attempt; retry cases exercise the public wrapper.
main=main.replace('apple_z2_read_packet(', 'apple_z2_read_packet_once(')
retry_cases=r'''
 reset_case(10);events[0]=2;events[1]=1;events[2]=1;events[3]=4;event_count=4;
 assert(!apple_z2_read_packet(&z)&&calls==4&&sleeps==1&&dispatches==1&&z.index_parity==1&&!z.runtime_resync_needed);canary();
 reset_case(10);events[0]=1;events[1]=5;events[2]=1;events[3]=1;events[4]=4;event_count=5;
 assert(!apple_z2_read_packet(&z)&&calls==5&&sleeps==1&&dispatches==1&&z.index_parity==1);canary();
 reset_case(0);z.runtime_resync_needed=true;events[0]=events[1]=1;event_count=2;
 assert(!apple_z2_read_packet(&z)&&calls==2&&!sleeps&&!dispatches&&z.runtime_idle_valid&&z.index_parity==1&&!z.runtime_resync_needed);canary();
 reset_case(10);for(unsigned i=0;i<5;i++)events[i]=6;event_count=5;
 assert(apple_z2_read_packet(&z)==-EIO&&calls==5&&sleeps==4&&!dispatches&&!z.index_parity&&z.runtime_resync_needed);canary();
 reset_case(10);for(unsigned i=0;i<10;i++)events[i]=2;event_count=10;
 assert(apple_z2_read_packet(&z)==-EPROTO&&calls==5&&sleeps==4&&!dispatches&&!z.index_parity&&z.runtime_resync_needed);canary();
 reset_case(10);z.runtime_resync_needed=true;events[0]=1;events[1]=3;events[2]=1;events[3]=1;events[4]=4;event_count=5;
 assert(!apple_z2_read_packet(&z)&&calls==5&&sleeps==1&&dispatches==1&&z.index_parity==1);canary();
 reset_case(10);z.runtime_resync_needed=true;events[0]=1;events[1]=1;events[2]=4;event_count=3;
 assert(!apple_z2_read_packet(&z)&&calls==3&&!sleeps&&dispatches==1&&z.index_parity==1);canary();
 reset_case(10);z.j171=false;fail_transfer=1;
 assert(apple_z2_read_packet(&z)==-EIO&&calls==1&&!sleeps);canary();
 puts("PASS 8 retry/resync cases: stale reply, corrupt frame, idle, bounded errors, second-query checksum, handoff and non-J171 no retry");
'''
before,after=main.rsplit('\n}',1)
main=before+'\n'+retry_cases+'\n}'+after
with tempfile.TemporaryDirectory(prefix='j171-rx-test-') as work:
    p = Path(work) / 'rx-host-test.c'
    binary = Path(work) / 'rx-host-test'
    p.write_text(header + functions + main)
    subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                    '-Wno-sign-compare', '-fsanitize=undefined,bounds', '-g',
                    str(p), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
