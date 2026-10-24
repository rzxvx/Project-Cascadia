#!/usr/bin/env python3
"""ASan/UBSan tests of production MTD backend using only a fake ioctl."""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='cascadia-iosnand-mtd-test-') as work:
    binary = Path(work) / 'test-iosnand-mtd'
    command = [os.environ.get('CC', 'gcc'), '-std=gnu11', '-O1', '-g',
                    '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections',
                    '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                    str(root / 'tests/test-iosnand-mtd.c'), '-o', str(binary)]
    subprocess.run(command, check=True)
    subprocess.run([str(binary)], check=True)
    # Hide modern MEMREAD definitions to emulate a 5.15 build sysroot.  The
    # production backend must compile and behave identically without them.
    old = Path(work) / 'old-mtd-headers.h'
    old.write_text('#define mtd_read_req hidden_modern_read_req\n'
                   '#define mtd_read_req_ecc_stats hidden_modern_ecc_stats\n'
                   '#include <mtd/mtd-abi.h>\n'
                   '#undef mtd_read_req\n#undef mtd_read_req_ecc_stats\n#undef MEMREAD\n')
    subprocess.run(command + ['-include', str(old)], check=True)
    subprocess.run([str(binary)], check=True)
