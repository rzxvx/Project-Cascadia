#!/usr/bin/env python3
"""Offline actual raw-owner guard plus preserved historical caller controls."""
from pathlib import Path
import subprocess,sys
R=Path(__file__).resolve().parents[1]
subprocess.run([sys.executable,str(R/'tests/fixtures/ppn-raw-kernel-owner/run.py'),'--source',str(R/'tools/nand/ppn.c')],check=True)
