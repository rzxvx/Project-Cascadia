#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Compile the actual block-isbad callback with an explicit fake page reader.

No MMIO, device nodes, NAND commands or kernel modules are used. Override
CASCADIA_H2FMI_SOURCE to run the same checks against an older driver source.
"""

import os
from pathlib import Path
import re
import resource
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
SOURCE = Path(os.environ.get(
    "CASCADIA_H2FMI_SOURCE",
    str(ROOT / "patches/files/drivers/mtd/devices/mtd-apple-h2fmi.c"),
))


def callback(text):
    start = text.index("static int h2fmi_mtd_block_isbad(")
    brace = text.index("{", start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


PRELUDE = r"""
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
typedef uint8_t u8;
typedef int64_t loff_t;
struct mtd_info { uint32_t erasesize; };
static int reader_error, reader_calls;
static u8 reader_status;
static loff_t reader_offset;
static int h2fmi_read_page_at(struct mtd_info *mtd, loff_t ofs,
                            u8 *data, u8 *oob, u8 *st)
{
    assert(mtd->erasesize == (1u << 22));
    assert(data == NULL && oob == NULL);
    assert(*st == 0);
    ++reader_calls;
    reader_offset = ofs;
    *st = reader_status;
    return reader_error;
}
"""

MAIN = r"""
static int check(u8 status, int error, loff_t offset)
{
    struct mtd_info mtd = { .erasesize = 1u << 22 };
    int ret;
    reader_status = status;
    reader_error = error;
    reader_calls = 0;
    ret = h2fmi_mtd_block_isbad(&mtd, offset);
    assert(reader_calls == 1);
    assert(reader_offset == (offset & ~((loff_t)mtd.erasesize - 1)));
    return ret;
}
static int expected(unsigned int status)
{
    /* Independent expectations, not the production switch/constants. */
    if (status == 0x40 || status == 0x42 || status == 0x49)
        return 0;
    if (status == 0x45)
        return 1;
    return -EIO;
}
int main(int argc, char **argv)
{
    unsigned int status, i;
    static const int errors[] = {
        -EIO, -ETIMEDOUT, -EREMOTEIO, -EINVAL, -ENOMEM, -EBADMSG
    };
    static const loff_t offsets[] = {
        0, 1, (1LL << 22) - 1, 1LL << 22, (1LL << 22) + 16383,
        (1LL << 32) + 12345
    };
    assert(argc == 2);
    switch (atoi(argv[1])) {
    case 0:
        for (status = 0; status < 256; ++status)
            assert(check(status, 0, 1LL << 22) == expected(status));
        break;
    case 1:
        for (i = 0; i < sizeof(errors) / sizeof(errors[0]); ++i)
            for (status = 0; status < 256; ++status)
                assert(check(status, errors[i], 1LL << 22) == errors[i]);
        break;
    case 2:
        for (i = 0; i < sizeof(offsets) / sizeof(offsets[0]); ++i)
            assert(check(0x40, 0, offsets[i]) == 0);
        break;
    case 3:
        assert(check(0x50, 0, 1LL << 22) == -EIO);
        break;
    default:
        abort();
    }
    return 0;
}
"""


class BlockStatusTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which("cc")
        if not compiler:
            raise unittest.SkipTest("C compiler unavailable")
        text = SOURCE.read_text()
        definitions = []
        for name, value in (("GOOD", 0x40), ("REFRESH", 0x42),
                            ("RETIRED", 0x45), ("ERASED", 0x49)):
            match = re.search(r"^#define PPN_ST_" + name +
                              r"\s+(0x[0-9a-fA-F]+)\s*$", text, re.M)
            if not match or int(match[1], 16) != value:
                raise AssertionError("Unexpected source status constant " + name)
            definitions.append(match[0])
        cls.tmp = tempfile.TemporaryDirectory(prefix="h2fmi-block-status-")
        cls.addClassCleanup(cls.tmp.cleanup)
        src = Path(cls.tmp.name) / "test.c"
        cls.exe = Path(cls.tmp.name) / "test"
        src.write_text(PRELUDE + "\n" + "\n".join(definitions) + "\n" +
                       callback(text) + "\n" + MAIN)
        subprocess.run([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror",
                        "-fsanitize=undefined", "-fno-sanitize-recover=all",
                        str(src), "-o", str(cls.exe)], check=True)

    def run_case(self, case):
        def no_core():
            resource.setrlimit(resource.RLIMIT_CORE, (0, 0))

        result = subprocess.run([str(self.exe), str(case)], capture_output=True,
                                text=True, preexec_fn=no_core)
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_all_256_status_bytes(self):
        self.run_case(0)

    def test_transport_error_precedes_every_status(self):
        self.run_case(1)

    def test_one_read_at_aligned_64_bit_block_offset(self):
        self.run_case(2)

    def test_observed_0x50_is_unknown_not_good(self):
        self.run_case(3)


if __name__ == "__main__":
    unittest.main()
