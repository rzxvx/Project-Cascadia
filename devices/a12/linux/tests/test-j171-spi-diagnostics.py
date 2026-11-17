#!/usr/bin/env python3
"""Exercise the actual before-cleanup SPI snapshot block, including aliased TX/RX."""
from pathlib import Path
import subprocess,tempfile
s=(Path(__file__).resolve().parents[1]/'extras/spi-apple-sio.c').read_text()
a=s.index('\n\tif (of_device_is_compatible(device->dev.of_node, "apple,j171a-z2-touchscreen") &&\n\t    t->len >= 16')
b=s.index('\n\tapple_spi_t8010_finish_xfer(',a)
body=s[a:b]
assert 'APPLE_SPI_RXDATA' not in body, 'Snapshot must not consume RX FIFO data'
header=r'''
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>
typedef uint8_t u8;typedef uint32_t u32;typedef uint64_t u64;
#define APPLE_SPI_CTRL 0
#define APPLE_SPI_CFG 1
#define APPLE_SPI_PIN 2
#define APPLE_SPI_STATUS 3
#define APPLE_SPI_FIFOSTAT 4
#define APPLE_SPI_TXCNT 5
#define APPLE_SPI_RXCNT 6
#define APPLE_SPI_CLKDIV 7
#define APPLE_SPI_WORD_DELAY 8
struct apple_spi{unsigned j171_zero_snapshots;bool j171_zero_latched;u32 j171_last_good[9];u64 j171_last_good_us;u32 j171_last_good_len;void*clk;};
struct spi_device{struct{bool of_node;}dev;};
struct spi_transfer{unsigned len;void*tx_buf,*rx_buf;};
static unsigned logs,reg_reads;static u64 clock_us;
static u64 ktime_get_boottime(void){return ++clock_us;}
#define ktime_to_us(x) (x)
static bool of_device_is_compatible(bool n,const char*s){(void)s;return n;}
static void*memchr_inv(const void*p,int c,size_t n){const u8*b=p;for(size_t i=0;i<n;i++)if(b[i]!=(u8)c)return (void*)(b+i);return NULL;}
static u32 reg_read(struct apple_spi*s,unsigned r){(void)s;reg_reads++;return 0x100+r;}
static unsigned long clk_get_rate(void*p){(void)p;return 200000000;}
static void log_line(const char*fmt,...){(void)fmt;logs++;}
#define dev_err(d,fmt,...) log_line(fmt, ##__VA_ARGS__)
static void snapshot(struct apple_spi*spi,struct spi_device*device,struct spi_transfer*t,u8 tx_first){unsigned tx_done=t->len,rx_done=t->len;
'''
main=r'''
}
int main(void){struct apple_spi s={0};struct spi_device d={{true}};u8 tx[64],rx[64];struct spi_transfer t={16,tx,rx};
 memset(rx,0xe1,sizeof(rx));snapshot(&s,&d,&t,0xeb);assert(reg_reads==9&&s.j171_last_good_len==16&&!logs);
 memset(rx,0,sizeof(rx));snapshot(&s,&d,&t,0xeb);assert(logs==2&&s.j171_zero_snapshots==1&&s.j171_zero_latched);
 snapshot(&s,&d,&t,0xeb);assert(logs==2&&s.j171_zero_snapshots==1);
 t.len=64;t.tx_buf=rx;rx[0]=0xea;snapshot(&s,&d,&t,0xa5); /* saved pre-transfer opcode, not overwritten TX[0] */
 assert(s.j171_last_good_len==64&&!s.j171_zero_latched&&s.j171_last_good[8]==0x108);
 t.len=16;t.tx_buf=tx;memset(rx,0,sizeof(rx));snapshot(&s,&d,&t,0xeb);assert(logs==4&&s.j171_zero_snapshots==2);
 rx[0]=0xe1;snapshot(&s,&d,&t,0xeb);memset(rx,0,sizeof(rx));snapshot(&s,&d,&t,0xeb);assert(logs==4);
 d.dev.of_node=false;unsigned before=reg_reads;snapshot(&s,&d,&t,0xeb);assert(reg_reads==before&&logs==4);
 puts("PASS: pre-cleanup snapshot, consecutive-zero latch, bounded episodes, aliased data opcode, non-J171 guard, non-destructive registers");
}
'''
with tempfile.TemporaryDirectory(prefix='j171-spi-diag-') as d:
 p=Path(d)/'test.c';p.write_text(header+body+main)
 subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror','-fsanitize=undefined,bounds',str(p),'-o',str(p.with_suffix(''))],check=True)
 subprocess.run([str(p.with_suffix(''))],check=True)
