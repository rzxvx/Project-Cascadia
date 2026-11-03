#!/usr/bin/env python3
"""Wraps one of Apple's IM4P files, byte for byte except its 4-letter type,
into an IMG4 with our IM4M (gpu.md, G4: iOS's own kernel by our boot path).
The payload and its compression record stay Apple's own (ipsw img4 create
re-encodes them; iBoot refused its LZFSE kernelcache: "Kernelcache image
not valid").

    retag-img4.py IM4P TYPE IM4M OUT       e.g. kernelcache krnl -> rkrn
"""
import sys


def der_len(n):
    if n < 0x80:
        return bytes([n])
    b = n.to_bytes((n.bit_length() + 7) // 8, "big")
    return bytes([0x80 | len(b)]) + b


def header(data):
    """(tag, header length, content length) of the DER item at 0"""
    n = data[1]
    if n & 0x80:
        k = n & 0x7f
        return data[0], 2 + k, int.from_bytes(data[2:2 + k], "big")
    return data[0], 2, n


if len(sys.argv) != 5 or len(sys.argv[2]) != 4:
    sys.exit(__doc__)
im4p = bytearray(open(sys.argv[1], "rb").read())
im4m = open(sys.argv[3], "rb").read()
tag, hl, ln = header(im4p)
if tag != 0x30 or im4p[hl:hl + 6] != b"\x16\x04IM4P" or im4p[hl + 6:hl + 8] != b"\x16\x04":
    sys.exit(f"{sys.argv[1]}: not an IM4P")
if hl + ln != len(im4p):
    sys.exit(f"{sys.argv[1]}: trailing bytes after the IM4P")
if header(im4m)[0] != 0x30 or b"IM4M" not in im4m[:12]:
    sys.exit(f"{sys.argv[3]}: not an IM4M")
old = im4p[hl + 8:hl + 12].decode()
im4p[hl + 8:hl + 12] = sys.argv[2].encode()

manifest = b"\xa0" + der_len(len(im4m)) + im4m
body = b"\x16\x04IMG4" + bytes(im4p) + manifest
open(sys.argv[4], "wb").write(b"\x30" + der_len(len(body)) + body)
print(f"{sys.argv[4]}: {old} -> {sys.argv[2]}, payload unchanged")
