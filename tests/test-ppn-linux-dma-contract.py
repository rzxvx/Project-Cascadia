#!/usr/bin/env python3
"""Pinned Linux provider/IRQ contract controls; no physical DMA operations."""
from pathlib import Path
import subprocess,sys
R=Path(__file__).resolve().parents[1]
command=[sys.executable,str(R/'tests/fixtures/ppn-linux-dma-contract/run.py')]
if (R/'build/linux/drivers/dma/tegra186-gpc-dma.c').exists():command+=['--linux',str(R/'build/linux')]
subprocess.run(command,check=True)
