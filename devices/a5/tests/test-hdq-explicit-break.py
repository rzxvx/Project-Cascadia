#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Actual HDQ read8 and Linux polling macros against explicit time/MMIO fakes."""
import hashlib
import json
import os
from pathlib import Path
import resource
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SOURCE = Path(os.environ.get('CASCADIA_HDQ_SOURCE', str(
    ROOT / 'patches/files/drivers/power/supply/apple-hdq-battery.c')))
FIXTURE = ROOT / 'tests/fixtures/hdq-explicit-break'


def function(text, name):
    start = text.rfind('\nstatic ', 0, text.index(name + '(')) + 1
    brace = text.index('{', text.index(name + '('))
    end, depth = brace + 1, 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]


PRELUDE = r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
typedef uint8_t u8;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int64_t ktime_t;
#define BIT(n) (1u << (n))
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define HZ 100
static uint64_t now_us, phase_us, tx_first_us;
static unsigned int ucon, tx_count, rx_count, rx_limit, event_count, events[32];
static bool stuck, response_ready, tx_during_break;
static unsigned char uart[128], tx[8];
struct hdq_battery { void *base; };
#define jiffies ((unsigned long)((phase_us + now_us) / 10000))
/* Exact HZ<=1000/divisible branch from Linux v6.12 jiffies.h. */
#define msecs_to_jiffies(m) (((m) + (1000 / HZ) - 1) / (1000 / HZ))
#define time_before(a,b) ((long)((b) - (a)) > 0)
static ktime_t ktime_get(void) { return now_us; }
static ktime_t ktime_add_us(ktime_t v, uint64_t d) { return v + d; }
static int ktime_compare(ktime_t a, ktime_t b) { return (a > b) - (a < b); }
#define might_sleep_if(v) ((void)(v))
#define cpu_relax() ((void)0)
static void udelay(unsigned int d) {
    now_us += d;
    if (d == 200) events[event_count++] = 2;
    if (d == 40) events[event_count++] = 4;
}
static void usleep_range(unsigned int a, unsigned int b) {
    assert(a <= b);
    now_us += b;
    if (a == 4500 && b == 5000) response_ready = true;
}
static unsigned int readl(void *addr) {
    unsigned int off = (unsigned char *)addr - uart;
    if (off == 4) {
        /* Explicit fake register storage: no assumed hardware self-clear. */
        return ucon;
    }
    if (off == 0x10) return response_ready && rx_count < rx_limit;
    if (off == 0x24) {
        unsigned int i = rx_count++;
        /* Echo the actual transmitted command before the gauge data. */
        if (i < 8) return tx[i];
        return ((0xa6 >> (i - 8)) & 1) ? 0xfe : 0xe0;
    }
    abort();
}
static void writel(unsigned int v, void *addr) {
    unsigned int off = (unsigned char *)addr - uart;
    if (off == 4) {
        if (v & BIT(4)) { events[event_count++] = 1; }
        else events[event_count++] = 3;
        if (!stuck || (v & BIT(4))) ucon = v;
        return;
    }
    assert(off == 0x20 && (v == 0xfe || v == 0xe0));
    if (!tx_count) { tx_first_us = now_us; events[event_count++] = 5; }
    if (ucon & BIT(4)) tx_during_break = true;
    assert(tx_count < ARRAY_SIZE(tx));
    tx[tx_count++] = v;
}
'''

MAIN = r'''
static void reset(unsigned int phase, unsigned int initial, bool hold) {
    now_us = 0; phase_us = phase; stuck = hold;
    ucon = initial; tx_count = rx_count = event_count = 0;
    rx_limit = 16;
    response_ready = false; tx_during_break = false; tx_first_us = 0;
}
static void sequence(void) {
    static const unsigned int expected[] = {1,2,3,4,5};
    assert(event_count == 5);
    for (unsigned int i=0;i<5;++i) assert(events[i] == expected[i]);
    assert(tx_first_us == 240 && tx_count == 8 && !tx_during_break);
}
int main(int argc, char **argv) {
    struct hdq_battery b = { .base = uart };
    unsigned int phase, i;
    static const unsigned int initial[] = {0x405,0x415,0x8405,0};
    assert(argc == 2);
    switch (atoi(argv[1])) {
    case 0:
        for (phase=0;phase<10000;phase+=10) {
            reset(phase,0x405,false);
            assert(hdq_read8(&b,8) == 0xa6);
            sequence();
        }
        break;
    case 1:
        for (phase=0;phase<10000;phase+=10) {
            reset(phase,0x405,true);
            assert(hdq_read8(&b,8) == -ETIMEDOUT);
            assert(tx_count == 0 && event_count == 3);
            assert(events[0]==1 && events[1]==2 && events[2]==3);
            assert(now_us>=5200 && now_us<=5210);
        }
        break;
    case 2:
        for (i=0;i<ARRAY_SIZE(initial);++i) {
            reset(9990,initial[i],false);
            assert(hdq_read8(&b,8) == 0xa6);
            sequence();
            assert(ucon == (initial[i] & ~BIT(4)));
        }
        break;
    case 3:
        reset(9990,0x405,false);rx_limit=15;
        assert(hdq_read8(&b,8)==-EIO);sequence();
        reset(0,0x415,false);rx_limit=0;
        assert(hdq_read8(&b,8)==-EIO);sequence();
        break;
    default: abort();
    }
    return 0;
}
'''


class BreakPulseTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which('cc')
        if not compiler:
            raise unittest.SkipTest('C compiler unavailable')
        text = SOURCE.read_text()
        header = FIXTURE / 'iopoll-macros.h'
        provenance = json.loads((FIXTURE / 'source-provenance.json').read_text())
        if hashlib.sha256(header.read_bytes()).hexdigest() != provenance['fixture_sha256']:
            raise AssertionError('Linux polling macro fixture hash mismatch')
        definitions = text[text.index('#define ULCON'):text.index('struct hdq_battery')]
        cls.temp = tempfile.TemporaryDirectory(prefix='hdq-break-pulse-')
        cls.addClassCleanup(cls.temp.cleanup)
        src = Path(cls.temp.name) / 'test.c'
        cls.exe = Path(cls.temp.name) / 'test'
        src.write_text(PRELUDE + '\n' + definitions + '\n' + header.read_text() +
                       '\n' + function(text, 'hdq_drain') + '\n' +
                       function(text, 'hdq_read8') + '\n' + MAIN)
        subprocess.run([compiler, '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                        '-Wno-sign-compare', '-Wno-unused-function',
                        '-fsanitize=undefined', '-fno-sanitize-recover=all',
                        str(src), '-o', str(cls.exe)], check=True)

    def run_case(self, case):
        def no_core():
            resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
        r = subprocess.run([str(self.exe), str(case)], capture_output=True,
                           text=True, preexec_fn=no_core, timeout=5)
        self.assertEqual(r.returncode, 0, r.stderr)

    def test_explicit_sequence_for_every_tick_phase(self): self.run_case(0)
    def test_rejected_clear_aborts_before_tx_for_every_phase(self): self.run_case(1)
    def test_other_ucon_bits_preserved(self): self.run_case(2)
    def test_short_rx_preserves_eio_after_break_sequence(self): self.run_case(3)


if __name__ == '__main__':
    unittest.main()
