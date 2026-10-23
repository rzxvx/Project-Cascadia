#!/usr/bin/env python3
"""Real direct certificate/nonce crypto; never yields accessory or power trust."""
from pathlib import Path
import os, subprocess, sys
root = Path(__file__).resolve().parents[1]
environment = dict(os.environ, PYTHONPATH=str(root / 'experiments/p105-authcp-verify'))
subprocess.run([sys.executable, str(root / 'experiments/p105-iap-direct-evidence/test_direct_evidence.py')],
               check=True, env=environment)
