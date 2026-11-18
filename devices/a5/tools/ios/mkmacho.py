#!/usr/bin/env python3
"""mkmacho.py ELF OUT -- a freestanding ARM ELF (linked at 0x2000, code and
read-only data only) as a static armv7 Mach-O executable for the iPad's iOS.

Layout: __PAGEZERO (0-0x1000), __TEXT from file offset 0 at 0x1000 (the
header and load commands, then the code at 0x2000 = file offset 0x1000), an
empty __LINKEDIT for the signature that `ldid -S` adds on the iPad, and
LC_UNIXTHREAD with pc = the ELF's entry: no dyld, XNU starts it directly with
sp -> argc.  Python's standard library only.
"""
import struct
import sys

LOAD = 0x2000           # where the ELF is linked
PAGE = 0x1000


def elf_image(data):
    if data[:4] != b"\x7fELF" or data[4] != 1 or data[5] != 1:
        sys.exit("mkmacho: not a 32-bit little-endian ELF")
    (e_type, e_machine, _, e_entry, _, e_shoff, _, _, _, _, e_shentsize,
     e_shnum, e_shstrndx) = struct.unpack_from("<HHIIIIIHHHHHH", data, 16)
    if e_machine != 40:
        sys.exit("mkmacho: not ARM")
    secs = [struct.unpack_from("<IIIIIIIIII", data, e_shoff + i * e_shentsize) for i in range(e_shnum)]
    strtab = secs[e_shstrndx]
    name = lambda off: data[strtab[4] + off:data.index(b"\0", strtab[4] + off)].decode()
    parts = []
    for s in secs:
        sh_name, sh_type, sh_flags, sh_addr, sh_offset, sh_size = s[:6]
        if not (sh_flags & 2) or sh_size == 0:          # SHF_ALLOC
            continue
        if sh_flags & 1:                                 # SHF_WRITE
            sys.exit("mkmacho: writable section %s: keep the program to code, read-only data and the stack" % name(sh_name))
        if sh_type == 8:                                 # SHT_NOBITS
            sys.exit("mkmacho: section %s has no bits" % name(sh_name))
        parts.append((sh_addr, data[sh_offset:sh_offset + sh_size], name(sh_name)))
    lo = min(a for a, _, _ in parts)
    hi = max(a + len(b) for a, b, _ in parts)
    if lo != LOAD:
        sys.exit("mkmacho: lowest section at %#x, expected %#x" % (lo, LOAD))
    img = bytearray(hi - lo)
    for a, b, _ in parts:
        img[a - lo:a - lo + len(b)] = b
    if not lo <= e_entry < hi:
        sys.exit("mkmacho: entry %#x outside the image" % e_entry)
    return bytes(img), e_entry, [n for _, _, n in parts]


def seg(name, vmaddr, vmsize, fileoff, filesize, prot, sects=b"", nsects=0):
    return struct.pack("<II16sIIIIIIII", 1, 56 + len(sects), name.encode(), vmaddr, vmsize,
                       fileoff, filesize, prot, prot, nsects, 0) + sects


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    img, entry, names = elf_image(open(sys.argv[1], "rb").read())
    text_file = (PAGE + len(img) + PAGE - 1) // PAGE * PAGE         # header page + code, rounded
    link_size = 16
    sect = struct.pack("<16s16sIIIIIIIII", b"__text", b"__TEXT", LOAD, len(img), PAGE, 2, 0, 0,
                       0x80000400, 0, 0)
    regs = [0] * 17
    regs[15] = entry                                               # pc (ARM state: bit 0 clear)
    if entry & 1:
        sys.exit("mkmacho: Thumb entry point; build _start as ARM")
    thread = struct.pack("<IIII", 5, 16 + 17 * 4, 1, 17) + struct.pack("<17I", *regs)
    cmds = (seg("__PAGEZERO", 0, PAGE, 0, 0, 0)
            + seg("__TEXT", PAGE, text_file, 0, text_file, 5, sect, 1)
            + seg("__LINKEDIT", PAGE + text_file, PAGE, text_file, link_size, 1)
            + thread)
    hdr = struct.pack("<IiiIIII", 0xfeedface, 12, 9, 2, 4, len(cmds), 1)     # ARM, v7, MH_EXECUTE, NOUNDEFS
    out = bytearray(text_file + link_size)
    out[:len(hdr) + len(cmds)] = hdr + cmds
    out[PAGE:PAGE + len(img)] = img
    open(sys.argv[2], "wb").write(out)
    print("mkmacho: %s: %d bytes of %s, entry %#x, %d bytes" % (sys.argv[2], len(img), "+".join(names), entry, len(out)))


main()
