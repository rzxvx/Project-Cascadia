#!/usr/bin/env python3
"""Capture Pico DCSD serial across USB disconnect/re-enumeration."""
import argparse
import os
import select
import sys
import termios
import time

p = argparse.ArgumentParser()
p.add_argument("log")
p.add_argument("--device", default="/dev/ttyACM0")
a = p.parse_args()

with open(a.log, "ab", buffering=0) as out:
    while True:
        try:
            fd = os.open(a.device, os.O_RDONLY | os.O_NOCTTY | os.O_NONBLOCK)
            attrs = termios.tcgetattr(fd)
            attrs[0] = attrs[1] = attrs[3] = 0
            attrs[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
            attrs[4] = attrs[5] = termios.B115200
            attrs[6][termios.VMIN] = 0
            attrs[6][termios.VTIME] = 0
            termios.tcsetattr(fd, termios.TCSANOW, attrs)
            out.write(f"\n[DCSD opened {a.device} {time.time():.3f}]\n".encode())
            while True:
                ready, _, _ = select.select([fd], [], [], 1)
                if ready:
                    data = os.read(fd, 65536)
                    if not data:
                        break
                    out.write(data)
                if not os.path.exists(a.device):
                    break
        except (OSError, termios.error) as exc:
            print(f"DCSD reconnect: {exc}", file=sys.stderr, flush=True)
        finally:
            if 'fd' in locals():
                os.close(fd)
                del fd
        time.sleep(0.2)
