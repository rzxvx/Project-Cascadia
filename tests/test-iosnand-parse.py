#!/usr/bin/env python3
"""Compile and run pure production LwVM parser tests without device code."""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='cascadia-iosnand-test-') as work:
    binary = Path(work) / 'test-iosnand-parse'
    subprocess.run([os.environ.get('CC', 'gcc'), '-std=gnu11', '-O1', '-g',
                    '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections',
                    '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                    str(root / 'tests/test-iosnand-parse.c'), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
