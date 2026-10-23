#!/usr/bin/env python3
"""Exercise the isolated lifetime proposal's actual read wrapper offline."""
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
GROUP = ROOT / 'experiments/p105-h2fmi-lifetime'
environment = dict(os.environ, REPO_ROOT=str(ROOT), DRIVER_FILE=str(GROUP / 'mtd-apple-h2fmi.c'))
raise SystemExit(subprocess.run([sys.executable, str(GROUP / 'test-read.py')],
                               env=environment, check=False).returncode)
