#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""
syscfg.py - read the iPhone's syscfg (NVMe namespace 3, magic "gfCS") and
print what the device tree needs from it, for boot/package.sh.

  syscfg.py SYSCFG.bin keys            all keys and their sizes
  syscfg.py SYSCFG.bin get KEY OUT     write one key's data to OUT

Format (version 2): header "SCfg" (LE "gfCS"), size, max size, version,
big size, key count; then entries: 4-byte key (reversed) + 16 bytes of data,
or "CNTB" + key + length + offset for longer data.

The file holds the phone's serial numbers: it stays in boot/inputs/
(ignored by git) and is never published.
"""
import struct
import sys


def parse(data):
    magic, size, maxsize, ver, bigsz, nkeys = struct.unpack_from("<4sIIIII", data, 0)
    if magic != b"gfCS":
        raise SystemExit("not a syscfg (magic %r)" % magic)
    keys, off = {}, 0x18
    for _ in range(nkeys):
        k = data[off:off + 4][::-1].decode("latin1")
        if k == "CNTB":
            real = data[off + 4:off + 8][::-1].decode("latin1")
            ln, o = struct.unpack_from("<II", data, off + 8)
            keys[real] = data[o:o + ln]
            off += 20
        else:
            keys[k] = data[off + 4:off + 20]
            off += 20
    return keys


def main():
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    keys = parse(open(sys.argv[1], "rb").read())
    if sys.argv[2] == "keys":
        for k in sorted(keys):
            print(k, len(keys[k]))
    elif sys.argv[2] == "get" and len(sys.argv) == 5:
        k = sys.argv[3]
        if k not in keys:
            raise SystemExit("no %s in syscfg" % k)
        open(sys.argv[4], "wb").write(keys[k])
    else:
        raise SystemExit(__doc__)


if __name__ == "__main__":
    main()
