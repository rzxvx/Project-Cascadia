#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Actual kernel and PIO page readers against local register/API fixtures.

The four-cycle expectation comes from the original PPN >=1.5 firmware
selection and read-command callsites, not from the implementation under test.
No hardware, firmware execution, ioctl or device path is used by this test.
"""
import os
from pathlib import Path
import re
import resource
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(os.environ.get('CASCADIA_ROW_SOURCE_ROOT', Path(__file__).resolve().parents[1]))


def function(text, name):
    match = re.search(r'(?m)^(?:static )?int ' + name + r'\(', text)
    if not match:
        raise ValueError('missing function: ' + name)
    brace = text.index('{', match.start())
    end, depth = brace + 1, 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[match.start():end]


PRELUDE = r'''
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
typedef uint8_t u8;
typedef uint32_t u32;
#define FMC_ON 0
#define FMC_IF_CTRL 8
#define FMC_CE_CTRL 12
#define FMC_RW_CTRL 16
#define FMC_ADDR0 24
#define FMC_ADDRNUM 32
#define FMC_STATUS 68
#define H2FMI_CE 0
#define H2FMI_PAGE_RAW 16448
static unsigned char registers[2][256];
static void *fmc[2] = { registers[0], registers[1] };
static int fault, ends, restored, data_calls, cleared;
static u32 readl(void *p) {
    u32 value;
    memcpy(&value, p, sizeof(value));
    return value;
}
static void writel(u32 value, void *p) { memcpy(p, &value, sizeof(value)); }
static u32 read_reg(void *p, unsigned off) { return readl((char *)p + off); }
static void write_reg(void *p, unsigned off, u32 v) { writel(v, (char *)p + off); }
#define R read_reg
#define W write_reg
struct h2fmi { int unused; };
struct h2fmi_bus { void *fmc; u8 *page; };
static int h2fmi_cmd_write(struct h2fmi *f, struct h2fmi_bus *b, u8 op, u8 op2) {
    (void)f; (void)b; assert(op == 0x0a && op2 == 0x37); return 0;
}
static int h2fmi_wait(struct h2fmi *f, void *base, u32 off, u32 mask, u32 want) {
    (void)f; assert(base && off == FMC_STATUS && mask == 0xb && want == 0xb);
    return fault == 1 ? -ETIMEDOUT : 0;
}
static void h2fmi_clear_irqs(struct h2fmi_bus *b) { (void)b; cleared++; }
static int h2fmi_status(struct h2fmi *f, struct h2fmi_bus *b, u8 *st) {
    (void)f; (void)b; *st = 0x40; return fault == 2 ? -EIO : 0;
}
static int h2fmi_data_out(struct h2fmi *f, struct h2fmi_bus *b, u8 *data, u32 len) {
    (void)f; (void)b; assert(len == H2FMI_PAGE_RAW); memset(data, 0xa6, len);
    data_calls++; return 0;
}
static int h2fmi_cmd1(struct h2fmi *f, struct h2fmi_bus *b, u8 op) {
    (void)f; (void)b; assert(op == 0x77); ends++; return 0;
}
static void h2fmi_device_reset(struct h2fmi_bus *b, u32 on, u32 control) {
    (void)b; assert(on == 5 && control == 0x1234); restored++;
}
static void nand_cmd2(int bus, uint8_t op, uint8_t op2) {
    assert(bus < 2 && op == 0x0a && op2 == 0x37);
}
static int wait_done(void *base, u32 off, u32 mask, u32 want) {
    return h2fmi_wait(NULL, base, off, mask, want);
}
static void clear_irqs(int bus) { assert(bus < 2); cleared++; }
static int ppn_status(int bus, uint8_t *st) {
    assert(bus < 2); *st = 0x40; return fault == 2 ? -EIO : 0;
}
static int ppn_data_out(int bus, uint8_t *data, uint32_t len) {
    assert(bus < 2 && len == H2FMI_PAGE_RAW); memset(data, 0xa6, len);
    data_calls++; return 0;
}
static int ppn_cmd1(int bus, uint8_t op) { assert(bus < 2 && op == 0x77); ends++; return 0; }
static void device_reset(int bus, uint32_t on, uint32_t control) {
    assert(bus < 2 && on == 5 && control == 0x1234); restored++;
}
'''

MAIN = r'''
int main(int argc, char **argv) {
    static const u32 rows[] = { 0, 0x100, 0x7ffff, 0xc27ff };
    u8 page[H2FMI_PAGE_RAW], st;
    struct h2fmi f;
    struct h2fmi_bus b = { .fmc = registers[0], .page = page };
    assert(argc == 2);
    for (unsigned row = 0; row < sizeof(rows)/sizeof(rows[0]); ++row)
        for (int injected = 0; injected < 3; ++injected) {
            int ret;
            memset(registers, 0, sizeof(registers)); memset(page, 0, sizeof(page));
            fault = injected; ends = restored = data_calls = cleared = 0;
            W(registers[0], FMC_ON, 5); W(registers[0], FMC_IF_CTRL, 0x1234);
            if (atoi(argv[1]) == 0)
                ret = h2fmi_read_raw_page(&f, &b, rows[row], &st);
            else
                ret = ppn_read_page(0, 0, rows[row], page, sizeof(page), &st);
            /* FMC encodes N cycles as N-1: four means 3, not 2. */
            assert(R(registers[0], FMC_ADDRNUM) == 3);
            assert(R(registers[0], FMC_ADDR0) == rows[row]);
            assert(R(registers[0], FMC_CE_CTRL) == 0);
            assert(R(registers[0], FMC_RW_CTRL) == 0xb);
            assert(ends == 1 && restored == 1 && cleared == 1);
            assert(data_calls == (fault == 0));
            assert((ret == 0) == (fault == 0));
            if (!fault) assert(st == 0x40 && page[0] == 0xa6);
        }
    return 0;
}
'''


class RowCycleTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which('cc')
        if not compiler:
            raise unittest.SkipTest('C compiler unavailable')
        kernel = (ROOT / 'patches/files/drivers/mtd/devices/mtd-apple-h2fmi.c').read_text()
        pio = (ROOT / 'tools/nand/ppn.c').read_text()
        cls.temp = tempfile.TemporaryDirectory(prefix='ppn-row-cycles-')
        cls.addClassCleanup(cls.temp.cleanup)
        src = Path(cls.temp.name) / 'test.c'
        cls.exe = Path(cls.temp.name) / 'test'
        src.write_text(PRELUDE + '\n' + function(kernel, 'h2fmi_cmd_addr_cmd') + '\n' +
                       function(kernel, 'h2fmi_read_raw_page') + '\n' +
                       function(pio, 'ppn_cmd_addr_cmd') + '\n' + function(pio, 'ppn_read_page') + MAIN)
        subprocess.run([compiler, '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=undefined', '-fno-sanitize-recover=all',
                        str(src), '-o', str(cls.exe)], check=True)

    def run_case(self, case):
        def no_core():
            resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
        r = subprocess.run([str(self.exe), str(case)], capture_output=True,
                           text=True, preexec_fn=no_core, timeout=5)
        self.assertEqual(r.returncode, 0, r.stderr)

    def test_kernel_row_width_and_failure_cleanup(self): self.run_case(0)
    def test_userspace_row_width_and_failure_cleanup(self): self.run_case(1)


if __name__ == '__main__':
    unittest.main()
