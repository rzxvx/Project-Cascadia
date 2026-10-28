#!/usr/bin/env python3
"""Offline tests of production exact sequence mapping and v2 cache parsing."""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='cascadia-iosnand-cache-test-') as work:
    binary = Path(work) / 'test-iosnand-cache'
    subprocess.run([os.environ.get('CC', 'gcc'), '-std=gnu11', '-O1', '-g',
                    '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections',
                    '-Wl,--wrap=fsync', '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                    str(root / 'tests/test-iosnand-cache.c'), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
