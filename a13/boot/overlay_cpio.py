#!/usr/bin/env python3
"""Write a small newc cpio archive, every entry owned by root.

Usage: overlay_cpio.py OUT DEST=SRC[:MODE] ...   (MODE octal, default 0644)
Parent directories of each DEST are added (0700 for root and .ssh, 0755
otherwise). The kernel unpacks
it on top of the main initramfs when the two archives are concatenated.
"""
import os
import sys


def entry(name, mode, data=b""):
    hdr = "070701" + "".join("%08X" % v for v in (
        0, mode, 0, 0, 1, 0, len(data), 0, 0, 0, 0, len(name) + 1, 0))
    out = hdr.encode() + name.encode() + b"\0"
    out += b"\0" * (-len(out) % 4)
    out += data + b"\0" * (-len(data) % 4)
    return out


def main():
    out, specs = sys.argv[1], sys.argv[2:]
    blob, dirs = b"", set()
    for spec in specs:
        dest, src = spec.split("=", 1)
        mode = 0o644
        if ":" in src:
            src, m = src.rsplit(":", 1)
            mode = int(m, 8)
        dest = dest.strip("/")
        parts = dest.split("/")[:-1]
        for i in range(1, len(parts) + 1):
            d = "/".join(parts[:i])
            if d not in dirs:
                dirs.add(d)
                private = d.split("/")[-1] in ("root", ".ssh")
                blob += entry(d, 0o040000 | (0o700 if private else 0o755))
        with open(src, "rb") as f:
            blob += entry(dest, 0o100000 | mode, f.read())
    blob += entry("TRAILER!!!", 0)
    with open(out, "wb") as f:
        f.write(blob)


if __name__ == "__main__":
    main()
