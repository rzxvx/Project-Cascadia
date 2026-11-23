# SPDX-License-Identifier: MIT
"""The USC code of our own kernels, as Apple's offline compiler builds them
for the A10 (`metal-tt -arch applegpu_g9p`, devices/a10/docs/gpu.md "Shader
ISA"): a Mach-O archive whose __compute section holds one nested Mach-O per
kernel; each one's __TEXT,__text is the USC code. The binary stays in build/
(compiler output), never in git.

    kernels(path) -> [code bytes, ...] in __compute order
"""
import struct


def _sections(buf, base):
    ncmds = struct.unpack_from("<8I", buf, base)[4]
    off = base + 32
    out = []
    for _ in range(ncmds):
        cmd, cmdsize = struct.unpack_from("<2I", buf, off)
        if cmd == 0x19:                     # LC_SEGMENT_64
            nsects = struct.unpack_from("<I", buf, off + 64)[0]
            for i in range(nsects):
                s = off + 72 + 80 * i
                sect = buf[s:s + 16].rstrip(b"\0").decode()
                _, size, foff = struct.unpack_from("<QQI", buf, s + 32)
                out.append((sect, size, foff))
        off += cmdsize
    return out


def kernels(path, section="__compute"):
    buf = open(path, "rb").read()
    out = []
    for sect, size, foff in _sections(buf, 0):
        if sect != section:
            continue
        pos, end = foff, foff + size
        while True:
            pos = buf.find(b"\xcf\xfa\xed\xfe", pos, end)
            if pos < 0:
                break
            for s2, sz, fo in _sections(buf, pos):
                if s2 == "__text":
                    out.append(buf[pos + fo:pos + fo + sz])
            pos += 4
    return out


if __name__ == "__main__":
    import sys
    for i, code in enumerate(kernels(sys.argv[1])):
        print(i, f"{len(code):#x}", code.hex())
