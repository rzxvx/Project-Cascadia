#!/usr/bin/env python3
"""Real-crypto offline tests; synthetic keys, no accessory/device trust."""
from pathlib import Path
import subprocess,sys
R=Path(__file__).resolve().parents[1]
subprocess.run([sys.executable,str(R/'experiments/p105-authcp-verify/test_authcp_verify.py')],check=True)
