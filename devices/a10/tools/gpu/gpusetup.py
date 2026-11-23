# SPDX-License-Identifier: MIT
"""Backend switch for the GPU tools: m1n1's proxy (default, M1N1DEVICE=...)
or the iPad's Linux (GPU_BACKEND=linux, see linuxproxy.py). Exports p, u,
iface and GUARD like `from m1n1.setup import *`."""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.environ.get("M1N1_PROXYCLIENT", os.path.join(HERE, "../../build/m1n1/proxyclient")))
sys.path.insert(0, HERE)

if os.environ.get("GPU_BACKEND") == "linux":
    import linuxproxy
    p, u, iface = linuxproxy.setup()
    GUARD = linuxproxy.GUARD
else:
    from m1n1.setup import p, u, iface  # noqa: F401
    from m1n1.proxy import GUARD  # noqa: F401

__all__ = ["p", "u", "iface", "GUARD"]
