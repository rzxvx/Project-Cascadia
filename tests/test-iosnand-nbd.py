#!/usr/bin/env python3
"""Actual read-only NBD request handler, using only local socketpairs."""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='cascadia-iosnand-nbd-test-') as work:
    binary = Path(work) / 'test-iosnand-nbd'
    subprocess.run([os.environ.get('CC', 'gcc'), '-std=gnu11', '-O1', '-g',
                    '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections',
                    '-Wl,--wrap=malloc', '-fsanitize=address,undefined',
                    '-fno-omit-frame-pointer', '-pthread',
                    str(root / 'tests/test-iosnand-nbd.c'), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True, timeout=30)
