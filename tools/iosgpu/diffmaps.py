#!/usr/bin/env python3
"""diffmaps.py -- byte-diff the GPU buffer snapshots gltrace writes.

gltrace dumps each IOConnectMapMemory buffer after frames a/b/c as
gt_<a|b|c>_map<type>.bin.  This shows where two frames' command buffers differ,
which is how the SGX543 command/PDS/USSE layout gets pulled apart (see
docs/research/p105-gpu.md).

    diffmaps.py A B [maptype]     e.g. diffmaps.py b c 0   (colour-only change)
                                       diffmaps.py a b 0   (clear vs draw)

Reads gt_<A>_map<type>.bin and gt_<B>_map<type>.bin in the current directory.
"""
import sys


def main():
    A, B = sys.argv[1], sys.argv[2]
    t = sys.argv[3] if len(sys.argv) > 3 else "0"
    x = open(f"gt_{A}_map{t}.bin", "rb").read()
    y = open(f"gt_{B}_map{t}.bin", "rb").read()
    n = min(len(x), len(y))
    runs = []
    i = 0
    while i < n:
        if x[i] != y[i]:
            j = i
            gap = 0
            while j < n and gap < 8:
                gap = gap + 1 if x[j] == y[j] else 0
                j += 1
            runs.append((i, j - gap))
            i = j
        else:
            i += 1
    total = sum(b - a for a, b in runs)
    print(f"map{t}: {A} vs {B} -- {len(runs)} region(s), {total} of {n} bytes differ")
    for a, b in runs:
        print(f"\n  0x{a:04x}..0x{b:04x} ({b - a})")
        print(f"    {A}: {x[a:b].hex(' ')}")
        print(f"    {B}: {y[a:b].hex(' ')}")


if __name__ == "__main__":
    main()
