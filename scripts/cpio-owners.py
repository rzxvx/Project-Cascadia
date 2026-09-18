#!/usr/bin/env python3
"""List the owners of a newc cpio archive that are not root.

    cpio-owners.py <archive.cpio>

Prints two lines: the non-zero uids and the non-zero gids found, each as
"uid=N:first/path/with/it", or "-" when there are none.  ./cascadia build
runs it on the initramfs the kernel just embedded, because the kernel build
records every file's real owner and says nothing about it -- the first build
on a Linux host shipped an image owned by uid 1000 throughout, and the only
symptom was dropbear refusing root's key.

Only the header is read; nothing in the archive is extracted or executed.
"""
import sys


def main() -> int:
    data = open(sys.argv[1], "rb").read()
    off, uids, gids = 0, {}, {}
    while off + 110 <= len(data) and data[off:off + 6] in (b"070701", b"070702"):
        field = [int(data[off + 6 + i * 8:off + 14 + i * 8], 16) for i in range(13)]
        uid, gid, fsize, nsize = field[2], field[3], field[6], field[11]
        name = data[off + 110:off + 110 + nsize - 1].decode("utf-8", "replace")
        off = (off + 110 + nsize + 3) & ~3
        off = (off + fsize + 3) & ~3
        if name == "TRAILER!!!":
            break
        if uid:
            uids.setdefault(uid, name)
        if gid:
            gids.setdefault(gid, name)
    print(" ".join("uid=%d:%s" % kv for kv in uids.items()) or "-")
    print(" ".join("gid=%d:%s" % kv for kv in gids.items()) or "-")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
