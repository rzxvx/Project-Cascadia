#!/usr/bin/env python3
"""Production NBD lifecycle, with every process/device API intercepted."""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='cascadia-iosnand-lifecycle-test-') as work:
    binary = Path(work) / 'test-iosnand-lifecycle'
    wrappers = ['open', 'socketpair', 'close', 'ioctl', 'fork', '_exit', 'waitpid',
                'signal', 'poll', '__poll_chk', 'opendir', 'dirfd', 'readdir', 'closedir',
                'read', 'write', 'kill']
    subprocess.run([os.environ.get('CC', 'gcc'), '-std=gnu11', '-O1', '-g',
                    '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections',
                    *[f'-Wl,--wrap={name}' for name in wrappers],
                    '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                    str(root / 'tests/test-iosnand-lifecycle.c'), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True, timeout=30)
