#!/usr/bin/env python3
"""Test the actual SPI setup selector and word-delay helper, not hardware."""
from pathlib import Path
import subprocess
import tempfile

source = (Path(__file__).resolve().parents[1] / 'extras/spi-apple-sio.c').read_text()

def function(name):
    start = source.index('static ', source.index(name) - 20)
    opening = source.index('{', start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]

header = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
typedef uint32_t u32;
#define APPLE_SPI_FIFO_DEPTH 16
#define SPI_CPOL 4
struct spi_device { struct {const char *of_node;} dev; unsigned mode; };
struct spi_transfer {const void *tx_buf; void *rx_buf;};
struct apple_spi {u32 word_delay;};
static bool of_device_is_compatible(const char *node, const char *name)
{return node && !strcmp(node, name);}
'''
main = r'''
int main(void) {
 struct spi_device j={.dev={"apple,j171a-z2-touchscreen"},.mode=SPI_CPOL};
 struct spi_device xr={.dev={"apple,dfrmtfw"},.mode=SPI_CPOL};
 struct spi_device other={0};
 struct apple_spi s={0}; char b;
 struct spi_transfer t={&b,&b};
 unsigned lengths[]={1,2,8,16,17,23,4000,4517,4520,8928,65535};
 for(unsigned i=0;i<sizeof(lengths)/sizeof(lengths[0]);i++) {
  unsigned n=lengths[i];
  assert(apple_spi_t8010_uses_descriptor(&j,&t,n));
  assert(apple_spi_t8010_word_delay(&s,&j,true)==2);
  assert(apple_spi_t8010_uses_descriptor(&xr,&t,n)==(n<=16));
  assert(apple_spi_t8010_uses_descriptor(&other,&t,n)==(n<=16));
 }
 t.rx_buf=0;
 for(unsigned n=1;n<33;n++) {
  bool expected=n==2||n==8||n==16;
  assert(apple_spi_t8010_uses_descriptor(&j,&t,n)==expected);
  assert(apple_spi_t8010_uses_descriptor(&xr,&t,n)==expected);
 }
 t.tx_buf=0; t.rx_buf=&b;
 assert(!apple_spi_t8010_uses_descriptor(&j,&t,23));
 s.word_delay=9; assert(apple_spi_t8010_word_delay(&s,&j,true)==9);
 s.word_delay=0; j.mode=0;
 assert(apple_spi_t8010_word_delay(&s,&j,true)==0);
 puts("PASS: J171 length-independent setup; other-board and TX-only selection preserved");
}
'''
with tempfile.TemporaryDirectory(prefix='j171-spi-setup-') as d:
    path = Path(d)
    (path/'test.c').write_text(header + function('apple_spi_t8010_uses_descriptor')
                              + function('apple_spi_t8010_word_delay') + main)
    subprocess.run(['cc', '-Wall', '-Wextra', '-Werror', '-fsanitize=undefined,bounds',
                    str(path/'test.c'), '-o', str(path/'test')], check=True)
    subprocess.run([str(path/'test')], check=True)
