#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""glref-compare.py A.raw B.raw W H [TOL]: two g9p-glref outputs (g9p and
softpipe) pixel by pixel -- the pixels differing by more than TOL (default
8) in any channel, their share, and a few of them; a side-by-side PPM
(A | B | difference x4) next to A for looking at."""
import sys

a = open(sys.argv[1], "rb").read()
b = open(sys.argv[2], "rb").read()
w, h = int(sys.argv[3]), int(sys.argv[4])
tol = int(sys.argv[5]) if len(sys.argv) > 5 else 8
bad = []
for i in range(0, 4 * w * h, 4):
    d = max(abs(a[i + k] - b[i + k]) for k in range(4))
    if d > tol:
        bad.append((i // 4 % w, i // 4 // w, tuple(a[i:i + 4]), tuple(b[i:i + 4])))
print("%s: %d of %d pixels differ by more than %d (%.2f%%)" % ("ok" if len(bad) < w * h // 200 else "FAIL", len(bad),
                                                               w * h, tol, 100.0 * len(bad) / (w * h)))
for p in bad[:5]:
    print("  (%d, %d): %s vs %s" % p)
rows = []
for y in range(h - 1, -1, -1):
    r = bytearray()
    for src in (a, b, None):
        for x in range(w):
            i = 4 * (y * w + x)
            if src is None:
                r += bytes(min(255, 4 * abs(a[i + k] - b[i + k])) for k in range(3))
            else:
                r += src[i:i + 3]
    rows.append(bytes(r))
open(sys.argv[1] + ".ppm", "wb").write(b"P6 %d %d 255\n" % (3 * w, h) + b"".join(rows))
