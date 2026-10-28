#!/usr/bin/env python3
"""Actual complete-C THS/parser composition; synthetic late-frame counterexample."""
from pathlib import Path
import subprocess,sys
R=Path(__file__).resolve().parents[1]
subprocess.run([sys.executable,str(R/'experiments/p105-ths-idbus/pipeline/test-pipeline.py'),'--source-root',str(R)],check=True)
