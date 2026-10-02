#!/usr/bin/env python3
"""extract-sgx-firmware.py -- the GPU microkernel, out of the user's own IPSW.

    python3 scripts/extract-sgx-firmware.py <ipsw> <out.fw> [<kernelcache.out>]

The SGX543MP2 runs a microkernel that iOS ships inside its GPU kext,
com.apple.driver.IMGSGX543, in the kernelcache.  It is Apple's and
Imagination's firmware, so this repository does not carry it; like the touch
and Wi-Fi firmware, it comes out of the IPSW the user already has.

What is taken, from the kext in the 8.4.1 (12H321) kernelcache:

    __DATA,__data   0x16094 bytes   the microkernel and its boot program,
                                    tables and more USSE code
    __TEXT,__const  0x730 bytes     the templates of the small PDS programs
                                    the host fills in

docs/research/p105-gpu.md ("The microkernel, and how iOS boots it") has the
layout of both.  The driver (drivers/misc/apple-sgx.c) uses offsets into
these exact bytes, so both are checked against the hashes of the 12H321
build and anything else is refused rather than half-loaded.

With a third argument the whole decrypted kernelcache is written there too:
tools/sgx/mkpack.py runs the GPU kext's render-target code from it in an
emulator (docs/GPU.md).

Output: a 64-byte header, then the two sections.

    0x00  char[8]  "SGX543FW"
    0x08  u32      format version (1)
    0x0c  u32      __data offset in this file
    0x10  u32      __data size
    0x14  u32      __const offset in this file
    0x18  u32      __const size
    0x1c  u32      __data address in the kernelcache
    0x20  u32      __const address in the kernelcache
    0x24  char[16] build ("12H321")
    0x34  pad to 0x40
"""

import hashlib
import importlib.util
import os
import struct
import sys
import zipfile

from Crypto.Cipher import AES

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

KERNELCACHE = "kernelcache.release.p105"
# docs/kernelcache-keys.txt: the public key for iPad2,5 / 12H321.
KC_IV = "ada137bf6aa705925d8ac5ada6025c11"
KC_KEY = "0155c713f32ee5fb9f18187e0b87d19ef38b9e56af121821264163627f894b05"

BUILD = "12H321"
DATA_SIZE = 0x16094
DATA_SHA256 = "ee453025b21ac4633aebba7745532da96cb4909482f3d5fc2526423fd58561ba"
CONST_SIZE = 0x730
CONST_SHA256 = "b59b6d44138d90c08605d0cd01643986eca1dd35e6071694b01fc2d21166d751"

MH_MAGIC = 0xFEEDFACE
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


def kernelcache(ipsw):
    try:
        with zipfile.ZipFile(ipsw) as z:
            img3 = z.read(KERNELCACHE)
    except KeyError:
        fail(f"{KERNELCACHE} not in {ipsw} -- is this an iPad2,5 IPSW?")
    ciphertext, plain_len = img3decrypt().parse_img3(img3)
    plain = AES.new(bytes.fromhex(KC_KEY), AES.MODE_CBC,
                    bytes.fromhex(KC_IV)).decrypt(ciphertext)[:plain_len]
    if plain[:8] != b"complzss":
        fail("kernelcache did not decrypt to complzss -- not the 12H321 build?")
    # The uncompressed size is at +0x0c; +0x10 is the compressed size, and
    # taking that one instead truncates the kernel.
    outlen = struct.unpack_from(">I", plain, 0x0C)[0]
    return lzss(plain[0x180:], outlen)


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
    if len(sys.argv) not in (3, 4):
        print(__doc__.split("\n\n")[1], file=sys.stderr)
        return 2
    ipsw, out = sys.argv[1:3]
    kc = kernelcache(ipsw)
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
    data = kc[va2off(daddr):va2off(daddr) + dsize]
    const = kc[va2off(caddr):va2off(caddr) + csize]
    for name, blob, size, sha in (("__data", data, DATA_SIZE, DATA_SHA256),
                                  ("__const", const, CONST_SIZE, CONST_SHA256)):
        if len(blob) != size or hashlib.sha256(blob).hexdigest() != sha:
            fail(f"{name} is not the {BUILD} one (size 0x{len(blob):x}) -- "
                 "the driver's offsets are for that build only")

    hdr = struct.pack("<8s7I16s", b"SGX543FW", 1, 0x40, len(data),
                      0x40 + len(data), len(const), daddr, caddr,
                      BUILD.encode())
    hdr += b"\0" * (0x40 - len(hdr))
    with open(out, "wb") as fh:
        fh.write(hdr + data + const)
    print(f"  {out}: microkernel 0x{len(data):x} + templates 0x{len(const):x} "
          f"bytes from IMGSGX543 ({BUILD})")
    if len(sys.argv) == 4:
        with open(sys.argv[3], "wb") as fh:
            fh.write(kc)
        print(f"  {sys.argv[3]}: the decrypted kernelcache, 0x{len(kc):x} bytes")
    return 0


if __name__ == "__main__":
    sys.exit(main())
