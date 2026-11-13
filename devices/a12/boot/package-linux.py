#!/usr/bin/env python3
"""Compatibility entry point for the selected-device packer (no legacy fallback)."""
import os
from pathlib import Path
import sys
os.execv(sys.executable,[sys.executable,str(Path(__file__).with_name('package-device-linux.py')),*sys.argv[1:]])
