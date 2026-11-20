#!/usr/bin/env python3
"""extract-sgx-firmware.py -- the GPU microkernel, out of the user's own IPSW.

    python3 scripts/extract-sgx-firmware.py [--board p105|k93|j1] <ipsw|kernelcache.macho> <out.fw> [<kernelcache.out>]

The SGX543 runs a microkernel that iOS ships inside its GPU kext,
com.apple.driver.IMGSGX543, in the kernelcache.  It is Apple's and
Imagination's firmware, so this repository does not carry it; like the touch
and Wi-Fi firmware, it comes out of the IPSW the user already has.

The kernelcaches it knows:

    p105  iPad2,5  8.4.1  12H321  kernelcache.release.p105  (the mini, SGX543MP2)
    k93   iPad2,1  8.4.1  12H321  kernelcache.release.k93   (the iPad 2, SGX543MP2)
    j1    iPad3,1  9.3.5  13G36   kernelcache.release.j1    (the iPad 3, SGX543MP4)

Without --board, the board is the one whose kernelcache the IPSW has.  The
input can also be a kernelcache already decrypted to a Mach-O; its build is
then told by its __data's size.  What is taken, from the kext:

    __DATA,__data   the microkernel and its boot program, tables and more
                    USSE code (0x16094 bytes in 12H321, 0x1608c in 13G36)
    __TEXT,__const  0x730 bytes: the templates of the small PDS programs the
                    host fills in

docs/research/p105-gpu.md ("The microkernel, and how iOS boots it") has the
layout of both.  The driver (drivers/gpu/drm/apple-sgx) uses offsets into
these exact bytes, so both are checked against the hashes of the 12H321
build and anything else is refused rather than half-loaded.

The iPad 2's (K93) kext is the same: its __const is byte for byte the
mini's, and of __data's 22565 words three differ, at 0x1607c, 0x16088 and
0x1608c -- kernel pointers, off by exactly the kext's own displacement in
that kernelcache.  The driver reads __data no further than 0x15fe8 (the
microkernel, its boot program and tables), so __data is checked over its
first USED_SIZE bytes, which are the same on both.

The iPad 3's (J1) 13G36 kext has the same microkernel, byte for byte; only
the three tables after the boot program are stored in the opposite order,
and 8 bytes earlier.  For 13G36 the file has them back in 12H321's order, so
the driver reads both builds at the same offsets, and holds __data only up
to the tables' end (DATA_USED): what follows is the kext's kmod_info, whose
pointers change with each kernelcache's load address.  That much is checked
by hash, the same bytes as 12H321's.

With a third argument the whole decrypted kernelcache is written there too:
tools/sgx/mkpack.py runs the GPU kext's render-target code from it in an
emulator (docs/GPU.md).

Output: a 64-byte header, then the two sections.

    0x00  char[8]  "SGX543FW"
    0x08  u32      format version (1)
    0x0c  u32      __data offset in this file
    0x10  u32      __data size (0x16094 from 12H321, 0x15fe8 from 13G36)
    0x14  u32      __const offset in this file
    0x18  u32      __const size
    0x1c  u32      __data address in the kernelcache
    0x20  u32      __const address in the kernelcache
    0x24  char[16] build the kext came from ("12H321" or "13G36")
    0x34  pad to 0x40
"""

import hashlib
import importlib.util
import os
import struct
import sys
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

# docs/kernelcache-keys.txt: the public keys for 12H321 (iPad2,5 and iPad2,1);
# ipsw.me: 13G36's for iPad3,1.
BOARDS = {
    "p105": ("kernelcache.release.p105", "ada137bf6aa705925d8ac5ada6025c11",
             "0155c713f32ee5fb9f18187e0b87d19ef38b9e56af121821264163627f894b05",
             "12H321"),
    "k93": ("kernelcache.release.k93", "38d12734c845d1dcdb84846caf619da7",
            "6dea51edc4c6f6c205e25ca1d9af5b9ee7167ed5685a937ad892e7fcbfb3e615",
            "12H321"),
    "j1": ("kernelcache.release.j1", "df201dea169ab952df43e31b1ae3ad03",
           "c1031e07e9603a5132f4366855eb7dfdb802bcbb6a8a2918ee612aaf806d8753",
           "13G36"),
}

BUILD = "12H321"
DATA_SIZE = 0x16094
DATA_SHA256 = "ee453025b21ac4633aebba7745532da96cb4909482f3d5fc2526423fd58561ba"
USED_SIZE = 0x16000             # what of __data the driver reads, and more
DATA_USED_SHA256 = "e1c5b52c612e3cd93d9f9ef252136f16c336bfaae670f6e6c8e3484ed62baa15"     # the same on the P105 and the K93
# 13G36: __data's size, and where its three tables are, in the order the
# file has them (the driver's tab 774, 784, 78c)
J1_BUILD = "13G36"
J1_DATA_SIZE = 0x1608C
J1_TABLES = (0x15FB8, 0x15EC8, 0x15D88)
TABLE_SIZES = (0x28, 0xF0, 0x140)
BOOT_END = 0x15D88              # the boot program ends here; the tables follow
TABLES_AT = 0x15D90             # where 12H321 has them
DATA_USED = 0x15FE8             # the tables' end: what the driver reads
DATA_USED_13G36_SHA256 = "d9936ab65d16c6791d5e0b62ff3b944765fa819758410d435b9d75b26aab8e31"
CONST_SIZE = 0x730
CONST_SHA256 = "b59b6d44138d90c08605d0cd01643986eca1dd35e6071694b01fc2d21166d751"

MH_MAGIC = 0xFEEDFACE
ZIP_MAGIC = b"PK\x03\x04"
LC_SEGMENT = 1


def fail(msg):
    print(f"error: {msg}", file=sys.stderr)
    raise SystemExit(1)


def img3decrypt():
    spec = importlib.util.spec_from_file_location(
        "img3decrypt", os.path.join(HERE, "img3decrypt.py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def lzss(comp, outlen):
    """Apple's complzss: 4 KiB window, 18-byte matches, flag bit 1 = literal."""
    N, F, THR = 4096, 18, 2
    text = bytearray(N)
    r = N - F
    out = bytearray()
    si, n = 0, len(comp)
    flags = 0
    while len(out) < outlen and si < n:
        flags >>= 1
        if not flags & 0x100:
            flags = comp[si] | 0xFF00
            si += 1
        if flags & 1:
            c = comp[si]
            si += 1
            out.append(c)
            text[r] = c
            r = (r + 1) & (N - 1)
        else:
            if si + 1 >= n:
                break
            i = comp[si] | ((comp[si + 1] & 0xF0) << 4)
            j = (comp[si + 1] & 0x0F) + THR
            si += 2
            for k in range(j + 1):
                c = text[(i + k) & (N - 1)]
                out.append(c)
                text[r] = c
                r = (r + 1) & (N - 1)
    return bytes(out[:outlen])


def kernelcache(path, board):
    """(board, the decrypted kernelcache): from an IPSW, or as it is (board
    as given, or None)"""
    with open(path, "rb") as fh:
        if fh.read(4) != ZIP_MAGIC:
            fh.seek(0)
            return board, fh.read()
    with zipfile.ZipFile(path) as z:
        names = set(z.namelist())
        if board is None:
            board = next((b for b, v in BOARDS.items() if v[0] in names), None)
            if board is None:
                fail(f"{path} has none of " + ", ".join(v[0] for v in BOARDS.values()))
        name, iv, key, build = BOARDS[board]
        if name not in names:
            fail(f"{name} not in {path} -- is this the {board}'s IPSW?")
        img3 = z.read(name)
    from Crypto.Cipher import AES
    ciphertext, plain_len = img3decrypt().parse_img3(img3)
    plain = AES.new(bytes.fromhex(key), AES.MODE_CBC,
                    bytes.fromhex(iv)).decrypt(ciphertext)[:plain_len]
    if plain[:8] != b"complzss":
        fail(f"kernelcache did not decrypt to complzss -- not the {build} build?")
    # The uncompressed size is at +0x0c; +0x10 is the compressed size, and
    # taking that one instead truncates the kernel.
    outlen = struct.unpack_from(">I", plain, 0x0C)[0]
    return board, lzss(plain[0x180:], outlen)


def microkernel(data, board):
    """(build, __data as the file has it), checked; board None: a Mach-O"""
    build = BOARDS[board][3] if board else (
        J1_BUILD if len(data) == J1_DATA_SIZE else BUILD)
    if build == J1_BUILD:
        if len(data) != J1_DATA_SIZE:
            fail(f"__data is not the {J1_BUILD} one (size 0x{len(data):x}) -- "
                 "the driver's offsets are for that build only")
        out = bytearray(data[:BOOT_END])
        out += bytes(TABLES_AT - BOOT_END)
        for off, size in zip(J1_TABLES, TABLE_SIZES):
            out += data[off:off + size]
        if len(out) != DATA_USED or hashlib.sha256(out).hexdigest() != DATA_USED_13G36_SHA256:
            fail(f"__data is not the {J1_BUILD} microkernel")
        return build, bytes(out)
    sha = DATA_SHA256 if board == "p105" else None
    if len(data) != DATA_SIZE or (sha and hashlib.sha256(data).hexdigest() != sha):
        fail(f"__data is not the {BUILD} one (size 0x{len(data):x}) -- "
             "the driver's offsets are for that build only")
    if hashlib.sha256(data[:USED_SIZE]).hexdigest() != DATA_USED_SHA256:
        fail(f"__data's first 0x{USED_SIZE:x} bytes are not the {BUILD} microkernel")
    return build, data


def segments(buf, base):
    """{segname: (vmaddr, fileoff, {sectname: (addr, size, offset)})}"""
    magic, _, _, _, ncmds = struct.unpack_from("<5I", buf, base)
    if magic != MH_MAGIC:
        return None
    segs, off = {}, base + 28
    for _ in range(ncmds):
        cmd, size = struct.unpack_from("<II", buf, off)
        if cmd == LC_SEGMENT:
            name = buf[off + 8:off + 24].split(b"\0")[0].decode()
            vmaddr, _, fileoff, _ = struct.unpack_from("<4I", buf, off + 24)
            nsect = struct.unpack_from("<I", buf, off + 48)[0]
            sects = {}
            for k in range(nsect):
                s = off + 56 + k * 68
                sname = buf[s:s + 16].split(b"\0")[0].decode()
                addr, ssize, soff = struct.unpack_from("<3I", buf, s + 32)
                sects[sname] = (addr, ssize, soff)
            segs[name] = (vmaddr, fileoff, sects)
        off += size
    return segs


def main():
    args = sys.argv[1:]
    board = None
    if args[:1] == ["--board"] and len(args) > 1:
        board = args[1]
        args = args[2:]
    if (board is not None and board not in BOARDS) or len(args) not in (2, 3):
        print(__doc__.split("\n\n")[1], file=sys.stderr)
        return 2
    src, out = args[:2]
    board, kc = kernelcache(src, board)
    top = segments(kc, 0) or fail("kernelcache is not a 32-bit Mach-O")
    pt = top.get("__PRELINK_TEXT") or fail("no __PRELINK_TEXT")
    pt_va, pt_off = pt[0], pt[1]

    def va2off(va):
        return va - pt_va + pt_off

    # Every prelinked kext is a Mach-O on a page boundary; the GPU kext is the
    # one whose strings name its driver class.
    found = None
    for base in range(pt_off, len(kc) - 28, 0x1000):
        if struct.unpack_from("<I", kc, base)[0] != MH_MAGIC:
            continue
        segs = segments(kc, base)
        try:
            caddr, csize, _ = segs["__TEXT"][2]["__cstring"]
        except (KeyError, TypeError):
            continue
        if b"SGXDriver543\0" in kc[va2off(caddr):va2off(caddr) + csize]:
            found = segs
            break
    if not found:
        fail("no IMGSGX543 kext in this kernelcache")

    daddr, dsize, _ = found["__DATA"][2]["__data"]
    caddr, csize, _ = found["__TEXT"][2]["__const"]
    build, data = microkernel(kc[va2off(daddr):va2off(daddr) + dsize], board)
    const = kc[va2off(caddr):va2off(caddr) + csize]
    if len(const) != CONST_SIZE or hashlib.sha256(const).hexdigest() != CONST_SHA256:
        fail(f"__const is not the known one (size 0x{len(const):x}) -- "
             "the driver's offsets are for that only")

    hdr = struct.pack("<8s7I16s", b"SGX543FW", 1, 0x40, len(data),
                      0x40 + len(data), len(const), daddr, caddr,
                      build.encode())
    hdr += b"\0" * (0x40 - len(hdr))
    with open(out, "wb") as fh:
        fh.write(hdr + data + const)
    print(f"  {out}: microkernel 0x{len(data):x} + templates 0x{len(const):x} "
          f"bytes from IMGSGX543 ({build})")
    if len(args) == 3:
        with open(args[2], "wb") as fh:
            fh.write(kc)
        print(f"  {args[2]}: the decrypted kernelcache, 0x{len(kc):x} bytes")
    return 0


if __name__ == "__main__":
    sys.exit(main())
