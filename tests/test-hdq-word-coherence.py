#!/usr/bin/env python3
"""Compile the production HDQ word reader against scripted read-only byte I/O.

No UART, battery, PMU, kernel module, privilege or firmware input is used.
The byte script verifies exact command order and stops at injected errors.
"""
from pathlib import Path
import os
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SOURCE = Path(os.environ.get('CASCADIA_HDQ_SOURCE',
    str(ROOT / 'patches/files/drivers/power/supply/apple-hdq-battery.c')))


def production_function(source):
    start = source.index('static int hdq_read16(')
    brace = source.index('{', start)
    depth, end = 1, brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


class WordCoherence(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix='cascadia-hdq-word-')
        path = Path(cls.temp.name)
        prefix = r'''
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint8_t u8;
struct hdq_battery { int unused; };
static struct { u8 command; int result; } script[12];
static unsigned int count, cursor;
static int hdq_read8(struct hdq_battery *b, u8 command)
{
 (void)b;
 assert(cursor < count);
 assert(command == script[cursor].command);
 assert(!(command & 0x80)); /* never a gauge-write command */
 return script[cursor++].result;
}
static void triplet(unsigned int offset, u8 reg, int high_before, int low, int high_after)
{
 script[offset].command = reg+1; script[offset].result = high_before;
 script[offset+1].command = reg; script[offset+1].result = low;
 script[offset+2].command = reg+1; script[offset+2].result = high_after;
}
'''
        suffix = r'''
int main(int argc, char **argv)
{
 struct hdq_battery battery = {};
 assert(argc == 2);
 if (!strcmp(argv[1], "stable")) {
  triplet(0,8,0x0f,0xa0,0x0f);count=3;cursor=0;
  assert(hdq_read16(&battery,8)==4000 && cursor==3);
 } else if (!strcmp(argv[1], "rollover")) {
  triplet(0,8,0,0xff,1);triplet(3,8,1,2,1);count=6;cursor=0;
  assert(hdq_read16(&battery,8)==0x102 && cursor==6);
 } else if (!strcmp(argv[1], "errors")) {
  for (unsigned int fail=0;fail<3;fail++) {
   triplet(0,8,1,2,1);script[fail].result=-ETIMEDOUT;count=3;cursor=0;
   assert(hdq_read16(&battery,8)==-ETIMEDOUT && cursor==fail+1);
  }
  triplet(0,8,0,0xff,1);triplet(3,8,1,2,1);
  script[4].result=-EREMOTEIO;count=6;cursor=0;
  assert(hdq_read16(&battery,8)==-EREMOTEIO && cursor==5);
 } else if (!strcmp(argv[1], "exhaustion")) {
  for (unsigned int i=0;i<9;i+=3)triplet(i,8,0,0xff,1);
  count=9;cursor=0;
  assert(hdq_read16(&battery,8)==-EIO && cursor==9);
 } else {
  triplet(0,0x14,255,255,255);count=3;cursor=0;
  assert(hdq_read16(&battery,0x14)==65535 && cursor==3);
  /* Signed average-current0xffff is valid -1mA, not a universal error. */
  assert((int16_t)65535==-1);
  triplet(0,0x7e,255,1,255);count=3;cursor=0;
  assert(hdq_read16(&battery,0x7e)==0xff01 && cursor==3);
  cursor=0;
  assert(hdq_read16(&battery,0x7f)==-EINVAL && cursor==0);
  assert(hdq_read16(&battery,0xff)==-EINVAL && cursor==0);
 }
 puts("actual HDQ word fixture PASS");
}
'''
        (path / 'fixture.c').write_text(prefix + production_function(SOURCE.read_text()) + suffix)
        cls.binary = path / 'fixture'
        subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu11', '-Wall', '-Wextra',
            '-Werror', '-fsanitize=undefined', '-fno-sanitize-recover=all', '-g',
            str(path / 'fixture.c'), '-o', str(cls.binary)], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def fixture(self, name):
        self.assertIn('fixture PASS', subprocess.check_output([str(self.binary), name], text=True))

    def test_stable_word(self):
        self.fixture('stable')

    def test_rollover_retries_and_publishes_only_stable_word(self):
        self.fixture('rollover')

    def test_transport_errors_stop_and_preserve_errno(self):
        self.fixture('errors')

    def test_three_unstable_attempts_fail(self):
        self.fixture('exhaustion')

    def test_valid_signed_sentinel_and_read_command_boundaries(self):
        self.fixture('boundaries')


if __name__ == '__main__':
    unittest.main()
