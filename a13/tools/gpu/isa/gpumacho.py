#!/usr/bin/env python3
"""Pull the shader code (__TEXT,__text) out of every Mach-O nested in an
Apple GPU binary archive (metal-tt output): prints name, size, hex."""
import struct, sys

def sections(buf, base):
    magic, cpu, sub, ftype, ncmds, sz, flags, _ = struct.unpack_from("<8I", buf, base)
    off = base + 32
    out = []
    for _ in range(ncmds):
        cmd, cmdsize = struct.unpack_from("<2I", buf, off)
        if cmd == 0x19:
            nsects = struct.unpack_from("<I", buf, off + 64)[0]
            for i in range(nsects):
                s = off + 72 + 80 * i
                sect = buf[s:s+16].rstrip(b"\0").decode()
                seg = buf[s+16:s+32].rstrip(b"\0").decode()
                addr, size, foff = struct.unpack_from("<QQI", buf, s + 32)
                out.append((seg, sect, addr, size, foff))
        off += cmdsize
    return out

buf = open(sys.argv[1], "rb").read()
top = sections(buf, 0)
for seg, sect, addr, size, foff in top:
    if sect != "__compute" and sect != "__vertex" and sect != "__fragment":
        continue
    pos = foff
    end = foff + size
    n = 0
    while True:
        pos = buf.find(b"\xcf\xfa\xed\xfe", pos, end)
        if pos < 0:
            break
        for s2 in sections(buf, pos):
            if s2[1] == "__text":
                code = buf[pos + s2[4]: pos + s2[4] + s2[3]]
                print(f"{sect}[{n}] @+{pos - foff:#x} text {s2[3]:#x} bytes")
                print(code.hex())
        n += 1
        pos += 4
