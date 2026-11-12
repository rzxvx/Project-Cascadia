#!/usr/bin/env python3
"""Compatibility name for XR; new callers should use prepare-device-bootset.py."""
import os
import sys
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from profile import selected
if selected() == 'ipad8':
    sys.exit('XR preparation entry point refuses an iPad profile')
os.execv(sys.executable,[sys.executable,str(Path(__file__).with_name('prepare-device-bootset.py')),*sys.argv[1:]])
