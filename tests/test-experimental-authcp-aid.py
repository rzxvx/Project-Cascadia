"""Actual unregistered AuthCPAID C parser, original synthetic oracle; no I/O."""
from pathlib import Path
import runpy
runpy.run_path(str(Path(__file__).resolve().parents[1]/"experiments/p105-authcp-aid/test-aid.py"),run_name="__main__")
