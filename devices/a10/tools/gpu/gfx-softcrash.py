#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""G3 helper (devices/a10/docs/gpu.md): boot the GFX firmware, check it answers
a ping, then ask it for a crash dump (crash endpoint 1, message 0x40) and let
m1n1's crash-log parser print its tasks; the dump goes to build/gpu/crash.bin.
Shows where the firmware's tasks wait (e.g. why the AGX main task has not
started). Same proxy session reuse as gfx-init.py.

    M1N1DEVICE=/dev/cu.usbmodemXXXX1 build/proxyvenv/bin/python tools/gpu/gfx-softcrash.py
"""
import importlib.util
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location("gfxboot", os.path.join(HERE, "gfx-boot.py"))
gb = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gb)

gb.gfx_power_on()
kf, buf = gb.boot_firmware(5.0)
gb.out("asking for a crash dump (ep 1, 0x40)")
kf.send((1 << 56) | 0x40)   # m1n1's crash_soft() passes a bare int to send()
try:
    kf.work_for(5.0)
    gb.out("no crash report within 5 s")
except Exception as e:
    gb.out(f"stopped on {e!r}")
gb.regs("end")
