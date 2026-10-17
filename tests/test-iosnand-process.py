#!/usr/bin/env python3
"""Production process-control header: all relevant APIs are compile-time fakes."""
from pathlib import Path
import os
import subprocess
import tempfile
root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='cascadia-iosnand-process-test-') as work:
    binary = Path(work) / 'test'
    subprocess.run([os.environ.get('CC', 'gcc'), '-std=gnu11', '-O1', '-g',
                    '-D_FILE_OFFSET_BITS=64', '-Wall', '-Wextra',
                    '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                    str(root / 'tests/test-iosnand-process.c'), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True, timeout=30)
