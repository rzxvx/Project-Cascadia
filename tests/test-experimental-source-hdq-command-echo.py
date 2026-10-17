#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Actual HDQ read8 callback against explicit UART command/response fixtures.

CASCADIA_HDQ_SOURCE selects the driver. If it uses explicit-break polling, load
its licensed hash-pinned Linux fixture; CASCADIA_HDQ_IOPOLL_FIXTURE names that
fixture directory explicitly when SOURCE is a standalone source copy.
"""
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
    ROOT / 'experiments/p105-source/apple-hdq-battery.c')))


def function(text, name):
    start = text.rfind('\nstatic ', 0, text.index(name + '(')) + 1
    brace = text.index('{', text.index(name + '('))
    end, depth = brace + 1, 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]


def polling_macros(source):
    """Use the actual pinned Linux macros only when selected code needs them.

    A combined source tree supplies PR15's licensed fixture. An arbitrary source
    copy can pass CASCADIA_HDQ_IOPOLL_FIXTURE as its fixture directory explicitly.
    Standalone historical code has no dependency on that separate PR.
    """
    if 'readl_poll_timeout(' not in function(source, 'hdq_read8'):
        return ''
    fixture_override = os.environ.get('CASCADIA_HDQ_IOPOLL_FIXTURE')
    if fixture_override:
        fixture = Path(fixture_override)
    elif SOURCE.resolve() == (ROOT / 'experiments/p105-source/apple-hdq-battery.c').resolve():
        fixture = ROOT / 'tests/fixtures/hdq-explicit-break'
    else:
        # Canonical driver path: <tree>/patches/files/drivers/power/supply/...
        relative = Path('patches/files/drivers/power/supply/apple-hdq-battery.c')
        selected = SOURCE.resolve()
        if tuple(selected.parts[-len(relative.parts):]) != relative.parts:
            raise ValueError('Set CASCADIA_HDQ_IOPOLL_FIXTURE for a source copy')
        fixture = selected.parents[5] / 'tests/fixtures/hdq-explicit-break'
    header = fixture / 'iopoll-macros.h'
    provenance = json.loads((fixture / 'source-provenance.json').read_text())
    expected = '8bc67d85a5d771eeb15354e9894e076662565894c8b670e9dcba67128bb74087'
    if (provenance['linux_revision'] != 'adc218676eef25575469234709c2d87185ca223a'
            or provenance['fixture_sha256'] != expected
            or hashlib.sha256(header.read_bytes()).hexdigest() != expected):
        raise AssertionError('Pinned Linux v6.12 polling macro fixture mismatch')
    clocks = r'''
/* Explicit synthetic microsecond clock for Linux iopoll dependencies. */
typedef uint64_t u64;
typedef int64_t ktime_t;
static inline ktime_t ktime_get(void) { return now_us; }
static inline ktime_t ktime_add_us(ktime_t t, uint64_t d) { return t + d; }
static inline int ktime_compare(ktime_t a, ktime_t b) { return (a > b) - (a < b); }
#define might_sleep_if(v) ((void)(v))
#define cpu_relax() ((void)0)
'''
    return clocks + header.read_text() + '\n'


PRELUDE = r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
typedef uint8_t u8;
typedef uint32_t u32;
#define BIT(n) (1u << (n))
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define HZ 100
#define msecs_to_jiffies(m) (((m) + 9) / 10)
#define time_before(a,b) ((long)((b) - (a)) > 0)
static unsigned long jiffies;
static uint64_t now_us;
struct hdq_battery { void *base; };
static unsigned char uart[128], input[40], tx[8];
static unsigned int ucon, tx_count, rx_index, rx_length;
static bool response_ready;
static unsigned int readl(void *address) {
    unsigned int off = (unsigned char *)address - uart;
    /* Explicit synthetic self-clear isolates echo handling from the break. */
    if (off == 4) return ucon & ~BIT(4);
    if (off == 0x10) return response_ready && rx_index < rx_length;
    if (off == 0x24) { assert(rx_index < rx_length); return input[rx_index++]; }
    abort();
}
static void writel(unsigned int value, void *address) {
    unsigned int off = (unsigned char *)address - uart;
    if (off == 4) { ucon = value; return; }
    assert(off == 0x20 && tx_count < 8);
    assert(value == 0xfe || value == 0xe0);
    tx[tx_count++] = value;
}
static void udelay(unsigned int n) { now_us += n; }
static void usleep_range(unsigned int a, unsigned int b) {
    assert(a <= b); now_us += b;
    if (a == 4500 && b == 5000) response_ready = true;
}
'''

MAIN = r'''
static void setup(unsigned int command, unsigned int value, unsigned int length) {
    assert(command < 128 && value < 256 && length <= 40);
    for (unsigned int i = 0; i < 8; ++i) {
        input[i] = command & (1u << i) ? 0xfe : 0xe0;
        input[i + 8] = value & (1u << i) ? 0xfe : 0xe0;
    }
    tx_count = rx_index = 0; rx_length = length;
    ucon = 0x405; response_ready = false; jiffies = 0; now_us = 0;
}
static int read_byte(unsigned int command) {
    struct hdq_battery b = { .base = uart };
    int ret = hdq_read8(&b, command);
    assert(tx_count == 8 && rx_index == rx_length);
    for (unsigned int i = 0; i < 8; ++i)
        assert(tx[i] == (command & (1u << i) ? 0xfe : 0xe0));
    return ret;
}
int main(int argc, char **argv) {
    unsigned int command, value, i;
    assert(argc == 2);
    switch (atoi(argv[1])) {
    case 0:
        for (command = 0; command < 128; ++command)
            for (value = 0; value < 256; ++value) {
                setup(command, value, 16);
                assert(read_byte(command) == (int)value);
            }
        break;
    case 1:
        for (i = 0; i < 8; ++i)
            for (value = 0; value < 256; ++value) {
                setup(8, 0xa6, 16);
                if (value == input[i]) continue;
                input[i] = value;
                assert(read_byte(8) == -EIO);
            }
        break;
    case 2:
        for (i = 0; i < 16; ++i) {
            setup(8, 0xa6, i);
            assert(read_byte(8) == -EIO);
        }
        break;
    case 3:
        setup(8, 0xa6, 16); input[3] = 0xe0;
        assert(read_byte(8) == -EIO);
        setup(8, 0xa6, 16);
        assert(read_byte(8) == 0xa6);
        break;
    case 4:
        for (command = 128; command < 256; ++command) {
            struct hdq_battery b = { .base = uart };
            setup(8, 0xa6, 16);
            assert(hdq_read8(&b, command) == -EINVAL);
            assert(tx_count == 0 && rx_index == 0 && ucon == 0x405);
        }
        break;
    default: abort();
    }
    return 0;
}
'''


class CommandEchoTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which('cc')
        if not compiler:
            raise unittest.SkipTest('C compiler unavailable')
        text = SOURCE.read_text()
        definitions = text[text.index('#define ULCON'):text.index('struct hdq_battery')]
        cls.temp = tempfile.TemporaryDirectory(prefix='hdq-command-echo-')
        cls.addClassCleanup(cls.temp.cleanup)
        src = Path(cls.temp.name) / 'test.c'
        cls.exe = Path(cls.temp.name) / 'test'
        src.write_text(PRELUDE + '\n' + definitions + '\n' + polling_macros(text) +
                       function(text, 'hdq_drain') + '\n' +
                       function(text, 'hdq_read8') + '\n' + MAIN)
        subprocess.run([compiler, '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                        '-Wno-sign-compare', '-fsanitize=undefined',
                        '-fno-sanitize-recover=all', str(src), '-o', str(cls.exe)],
                       check=True)

    def run_case(self, case):
        def no_core():
            resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
        result = subprocess.run([str(self.exe), str(case)], capture_output=True,
                                text=True, preexec_fn=no_core)
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_all_read_commands_and_all_data_bytes(self): self.run_case(0)
    def test_every_wrong_echo_byte_at_every_position(self): self.run_case(1)
    def test_incomplete_transactions_still_return_eio(self): self.run_case(2)
    def test_non_read_commands_are_rejected_before_uart_write(self):
        self.run_case(4)

    def test_next_transaction_after_rejected_echo_can_succeed(self): self.run_case(3)


if __name__ == '__main__':
    unittest.main()
