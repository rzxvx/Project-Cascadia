#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""
touchcal.py - calibrate the iPhone 11's touchscreen: map the panel's raw
positions to the screen's corners (./cascadia a13 recalibrate).

  touchcal.py PHONE OUT        PHONE: ssh target (root@172.16.42.1)

Puts the driver into raw mode (apple_hidspi_touch's `raw` parameter), asks
for a tap on each corner of the screen, and writes OUT (shell syntax) with
the DT values `./cascadia a13 pack` puts on the touchscreen node:
touchscreen-min-x/-y, touchscreen-size-x/-y, touchscreen-inverted-x/-y.
The driver turns an inverted axis around min..max.
"""
import statistics
import struct
import subprocess
import sys

EV_KEY, EV_ABS, BTN_TOUCH = 1, 3, 0x14A
ABS_MT_X, ABS_MT_Y = 0x35, 0x36
PARAM = "/sys/module/apple_hidspi_touch/parameters/raw"
CORNERS = [("TL", "top-left"), ("TR", "top-right"), ("BR", "bottom-right"), ("BL", "bottom-left")]
FIND = ('for n in /sys/class/input/event*; do '
        '[ "$(cat $n/device/name 2>/dev/null)" = "Apple iPhone touchscreen" ] '
        '&& echo /dev/input/${n##*/} && break; done')


def ssh(phone, cmd, **kw):
    return subprocess.run(["ssh", "-o", "ConnectTimeout=8", phone, cmd], **kw)


def taps(stream):
    """yield the median (x, y) of each touch, finger down to finger up"""
    xs, ys, down = [], [], False
    x = y = None
    while True:
        ev = stream.read(24)
        if len(ev) < 24:
            return
        _, _, typ, code, val = struct.unpack("<qqHHi", ev)
        if typ == EV_KEY and code == BTN_TOUCH:
            if val:
                down, xs, ys = True, [], []
            elif down:
                down = False
                if xs and ys:
                    yield statistics.median(xs), statistics.median(ys)
        elif typ == EV_ABS and down:
            if code == ABS_MT_X:
                xs.append(val)
            elif code == ABS_MT_Y:
                ys.append(val)


def main():
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    phone, out = sys.argv[1], sys.argv[2]
    dev = ssh(phone, FIND, capture_output=True, text=True).stdout.strip()
    if not dev:
        raise SystemExit(f"no touchscreen on {phone} (is the phone booted, the driver up?)")
    if ssh(phone, f"[ -w {PARAM} ] && echo 1 > {PARAM}").returncode:
        raise SystemExit("this kernel has no raw mode: ./cascadia a13 build, pack and boot first")
    print(f"touchscreen {dev} on {phone}, raw mode on")
    got = {}
    p = subprocess.Popen(["ssh", phone, f"exec cat {dev}"], stdout=subprocess.PIPE)
    try:
        it = taps(p.stdout)
        for key, name in CORNERS:
            print(f"  tap the {name} corner, as far into it as the finger goes, and lift ...",
                  end="", flush=True)
            got[key] = next(it)
            print(f" ({got[key][0]:.0f}, {got[key][1]:.0f})")
    finally:
        p.kill()
        ssh(phone, f"echo 0 > {PARAM}")
    xl = (got["TL"][0] + got["BL"][0]) / 2
    xr = (got["TR"][0] + got["BR"][0]) / 2
    yt = (got["TL"][1] + got["TR"][1]) / 2
    yb = (got["BL"][1] + got["BR"][1]) / 2
    if abs(xr - xl) < 1000 or abs(yb - yt) < 1000:
        raise SystemExit("the corners are too close together -- tap the four corners again")
    if abs(got["TL"][0] - got["TR"][0]) < abs(got["TL"][1] - got["TR"][1]):
        raise SystemExit("x and y look swapped -- not handled yet; tap the corners in the order asked")
    cal = {
        "MIN_X": round(min(xl, xr)), "SIZE_X": round(max(xl, xr)) + 1, "INV_X": int(xl > xr),
        "MIN_Y": round(min(yt, yb)), "SIZE_Y": round(max(yt, yb)) + 1, "INV_Y": int(yt > yb),
    }
    with open(out, "w") as f:
        f.write("# ./cascadia a13 recalibrate: the touchscreen's raw range (pack puts it in the DT)\n")
        for k, v in cal.items():
            f.write(f"{k}={v}\n")
    print(" ".join(f"{k}={v}" for k, v in cal.items()))
    print(f"saved {out} -- next: ./cascadia a13 pack, then boot")


if __name__ == "__main__":
    main()
