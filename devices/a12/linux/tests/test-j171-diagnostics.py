#!/usr/bin/env python3
"""Test actual first-failure ring storage/dump guards without SPI hardware."""
from pathlib import Path
import subprocess,tempfile
s=(Path(__file__).resolve().parents[1]/'extras/apple_z2_j171.c').read_text()
a=s.index('static void apple_z2_trace_xfer(');b=s.index('static void apple_z2_dispatch_frame(',a)
functions=s[a:b]
header=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
typedef uint8_t u8;typedef uint32_t u32;typedef uint64_t u64;
#define APPLE_Z2_TRACE_DEPTH 8
#define READ_ONCE(x) (x)
#define min_t(t,a,b) ((t)(a)<(t)(b)?(t)(a):(t)(b))
struct apple_z2_trace{u64 us;u32 len;int error;u8 tag,data;u8 tx[16],rx[32];};
struct gpio_desc{int raw;};struct spi_device{struct gpio_desc cs;};
struct apple_z2{bool j171,booted,failure_captured,clk_enabled;unsigned trace_next,recovery_count,protocol_state;
 int runtime_irq_count,runtime_pending,index_parity;struct gpio_desc*interrupt_gpio,*reset_gpio;
 struct spi_device*spidev;u8 *tx_buf,*rx_buf;struct apple_z2_trace trace[8];};
static unsigned lines,firsts;static u64 clock_us;
static u64 ktime_get_boottime(void){return ++clock_us;}
#define ktime_to_us(x) (x)
static int atomic_read(int*p){return *p;}
static int gpiod_get_raw_value_cansleep(struct gpio_desc*g){return g->raw;}
static struct gpio_desc*spi_get_csgpiod(struct spi_device*s,int i){assert(i==0);return &s->cs;}
static void log_line(const char*fmt,...){lines++;if(strstr(fmt,"FIRST FAILURE"))firsts++;}
#define dev_err(d,fmt,...) log_line(fmt, ##__VA_ARGS__)
'''
main=r'''
int main(void){
 struct apple_z2 z={0};u8 tx[16],rx[64];struct spi_device spi={{1}};struct gpio_desc atn={1},reset={1};
 z.j171=z.booted=true;z.tx_buf=tx;z.rx_buf=rx;z.spidev=&spi;z.interrupt_gpio=&atn;z.reset_gpio=&reset;
 memset(tx,0xeb,16);memset(rx,0x76,64);
 apple_z2_trace_xfer(&z,false,16,0);assert(z.trace_next==1&&z.trace[0].rx[15]==0x76&&z.trace[0].rx[16]==0);
 apple_z2_trace_xfer(&z,true,64,0);assert(z.trace[1].rx[31]==0x76&&z.trace[1].tx[15]==0xa5);
 for(unsigned i=0;i<10;i++){rx[0]=i;apple_z2_trace_xfer(&z,false,16,-EPROTO);}
 assert(z.trace_next==12&&z.trace[3].rx[0]==9&&z.trace[4].rx[0]==2); /* oldest retained */
 apple_z2_capture_failure(&z,-EPROTO);assert(firsts==1&&lines==9&&z.failure_captured);
 apple_z2_capture_failure(&z,-EIO);assert(firsts==1&&lines==9); /* one-shot before recovery */
 z.failure_captured=false;z.trace_next=0;z.interrupt_gpio=NULL;
 apple_z2_capture_failure(&z,-EIO);assert(firsts==2&&lines==10); /* optional ATN safe */
 z.booted=false;apple_z2_trace_xfer(&z,false,16,0);assert(!z.trace_next);
 z.failure_captured=false;apple_z2_capture_failure(&z,-EIO);assert(firsts==2);
 z.booted=true;z.j171=false;apple_z2_trace_xfer(&z,false,16,0);apple_z2_capture_failure(&z,-EIO);assert(!z.trace_next&&firsts==2);
 puts("PASS: actual history ring bounds/wrap, TX/RX snapshots, zero padding, one-shot capture, boot/legacy guards");
}
'''
with tempfile.TemporaryDirectory(prefix='j171-diag-') as d:
 p=Path(d)/'test.c';p.write_text(header+functions+main)
 subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror','-fsanitize=undefined,bounds',str(p),'-o',str(p.with_suffix(''))],check=True)
 subprocess.run([str(p.with_suffix(''))],check=True)
