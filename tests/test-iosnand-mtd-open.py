#!/usr/bin/env python3
"""Actual MTD discovery/node binding; strict fake APIs, no real files/devices."""
from pathlib import Path
import os
import subprocess
import tempfile
root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='cascadia-mtd-open-test-') as tmp:
    work = Path(tmp)
    binary = work / 'test'
    command = [os.environ.get('CC', 'gcc'), '-std=gnu11', '-O1', '-g',
               '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections',
               '-Wall', '-Wextra', '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
               str(root / 'tests/test-iosnand-mtd-open.c'), '-o', str(binary)]
    subprocess.run(command, check=True)
    subprocess.run([str(binary)], check=True, timeout=30)
    old = work / 'old-mtd.h'
    old.write_text('#define mtd_read_req hidden_modern_read_req\n'
                   '#define mtd_read_req_ecc_stats hidden_modern_ecc_stats\n'
                   '#include <mtd/mtd-abi.h>\n'
                   '#undef mtd_read_req\n#undef mtd_read_req_ecc_stats\n#undef MEMREAD\n')
    subprocess.run(command + ['-include', str(old)], check=True)
    subprocess.run([str(binary)], check=True, timeout=30)
