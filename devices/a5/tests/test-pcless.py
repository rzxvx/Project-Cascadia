#!/usr/bin/env python3
"""The PC-less pieces, read back the way iBEC reads them.

    python3 tests/test-pcless.py [--ibec build/firmware/iBEC.patched]

make-pcless.py: an independent LZSS decoder (ring buffer of spaces, as Apple's)
and Mach-O / img3 readers over synthetic input.  patch-ibec-upgrade.py runs
only with --ibec, the file being Apple's and not in the repository.  What this
cannot show: that the iPad's iBEC accepts the result (P3-P5 of the plan).
"""
import importlib.util
import os
import struct
import sys
import tempfile
import zlib
from pathlib import Path

HERE = Path(__file__).resolve().parent.parent / "scripts"


def load(name):
    spec = importlib.util.spec_from_file_location(name.replace("-", "_"), HERE / f"{name}.py")
    mod = importlib.util.module_from_spec(spec)
    sys.path.insert(0, str(HERE))
    spec.loader.exec_module(mod)
    return mod


def lzss_decode(src, limit):
    N, F, THRESH = 4096, 18, 2
    ring = bytearray(b" " * (N - F) + b"\0" * F)
    r, i, flags, out = N - F, 0, 0, bytearray()
    while True:
        flags >>= 1
        if not flags & 0x100:
            if i >= len(src):
                break
            flags = src[i] | 0xFF00
            i += 1
        if flags & 1:
            if i >= len(src):
                break
            c = src[i]
            i += 1
            out.append(c)
            ring[r] = c
            r = (r + 1) & (N - 1)
        else:
            if i + 1 >= len(src):
                break
            a, b = src[i], src[i + 1]
            i += 2
            pos = a | ((b & 0xF0) << 4)
            for k in range((b & 0x0F) + THRESH + 1):
                c = ring[(pos + k) & (N - 1)]
                out.append(c)
                ring[r] = c
                r = (r + 1) & (N - 1)
        assert len(out) <= limit, "decoder overran"
    return bytes(out)


def read_macho(m):
    magic, cpu, sub, ftype, ncmds, size, _ = struct.unpack_from("<7I", m)
    assert magic == 0xFEEDFACE and cpu == 12 and sub == 9 and ftype == 2 and ncmds == 2
    o, seg, pc = 28, None, None
    for _ in range(ncmds):
        cmd, csize = struct.unpack_from("<II", m, o)
        if cmd == 1:
            name = m[o + 8:o + 24].rstrip(b"\0")
            seg = (name,) + struct.unpack_from("<4I", m, o + 24)
        elif cmd == 5:
            assert struct.unpack_from("<II", m, o + 8) == (1, 17)
            pc = struct.unpack_from("<I", m, o + 16 + 15 * 4)[0]
        o += csize
    assert o == 28 + size
    return seg, pc


def template(ident):
    tags = b""
    for t, d in ((b"EPYT", ident[::-1]), (b"ATAD", b"\xaa" * 32), (b"SREV", b"v1  "),
                 (b"GABK", b"k" * 56), (b"HSHS", b"s" * 128), (b"TREC", b"c" * 64)):
        tags += struct.pack("<4sII", t, 12 + len(d), len(d)) + d
    return struct.pack("<4sIII4s", b"3gmI", 20 + len(tags), len(tags), 0, ident[::-1]) + tags


def main():
    mk = load("make-pcless")

    bundle = os.urandom(70000) + bytes(range(256)) * 9 + b"tail"
    # whole bytes of the literal stream, every length mod 8
    for n in (0, 1, 7, 8, 9, 15, 16, 17, 4097):
        d = os.urandom(n)
        assert lzss_decode(mk.lzss_literal(d), n) == d, n

    image = mk.macho(bundle)
    seg, pc = read_macho(image)
    assert seg == (b"__TEXT", 0x80000000, (len(bundle) + 0xFFF) & ~0xFFF, 0x1000, len(bundle))
    assert pc == 0x80000000
    assert image[seg[3]:seg[3] + seg[4]] == bundle

    comp = mk.complzss(image)
    assert comp[:8] == b"complzss"
    adler, usize, csize = struct.unpack_from(">III", comp, 8)
    assert usize == len(image) and csize == len(comp) - 0x180
    out = lzss_decode(comp[0x180:], usize)
    assert out == image and (zlib.adler32(out) & 0xFFFFFFFF) == adler

    mk.check_limits(bundle, image, comp)
    for args in ((b"x" * (0x4000000 + 1), b"", b""),             # reaches the load buffer
                 (b"", b"x" * (0x3F00000 + 1), b""),             # over the load size
                 (b"", b"", b"x" * ((40 << 20) + 0x181))):       # over the heap
        try:
            mk.check_limits(*args)
        except SystemExit:
            continue
        raise AssertionError("limit not enforced")

    packed = mk.pack_plain(template(b"krnl"), comp)
    assert mk.ident(packed) == b"krnl"
    assert mk.tag_data(packed, b"ATAD") == comp
    assert not any(t == b"GABK" for _, t, _, _ in mk.parse_tags(packed))

    with tempfile.TemporaryDirectory() as tmp:
        t = template(b"dtre")
        raw = Path(tmp) / "adt"
        raw.write_bytes(b"\x11\0\0\0raw adt")
        assert mk.tag_data(mk.devicetree(raw, t), b"ATAD") == b"\x11\0\0\0raw adt"
        plain = Path(tmp) / "plain"
        plain.write_bytes(mk.pack_plain(t, b"plain adt"))
        assert mk.tag_data(mk.devicetree(plain, t), b"ATAD") == b"plain adt"
        enc = Path(tmp) / "enc"
        enc.write_bytes(t)
        try:
            mk.devicetree(enc, t)
        except SystemExit:
            pass
        else:
            raise AssertionError("an encrypted DeviceTree was accepted")
    print("make-pcless: LZSS, Mach-O, complzss, limits, img3: PASS")

    if len(sys.argv) == 3 and sys.argv[1] == "--ibec":
        pi = load("patch-ibec-upgrade")
        src = Path(sys.argv[2]).read_bytes()
        img = bytearray(src)
        pi.patch(img)
        again = bytearray(img)
        pi.patch(again)
        assert again == img, "not idempotent"
        changed = [i for i in range(len(src)) if src[i] != img[i]]
        sites = [(pi.SITE_AUTOBOOT, 2), (pi.LIT_CMD, 4), (pi.SITE_CMD_GETENV, 4),
                 (pi.NAME_UPDATE, 14), (pi.SITE_SAVEENV, 4), (pi.SITE_RECORD, 4)]
        allowed = {va - pi.BASE + k for va, n in sites for k in range(n)}
        assert changed and set(changed) <= allowed, "a byte outside the known sites changed"
        wrong = bytearray(src)
        wrong[pi.off(pi.SITE_SAVEENV)] ^= 1
        try:
            pi.patch(wrong)
        except SystemExit:
            pass
        else:
            raise AssertionError("a wrong original was accepted")
        st = bytearray(src)
        pi.patch(st, stay=True)
        diff = [i for i in range(len(src)) if st[i] != img[i]]
        assert diff and set(diff) <= {pi.off(pi.SITE_REBOOT) + k for k in range(4)}, "--stay touched something else"
        for stage, (va, _orig) in pi.STOPS.items():
            one = bytearray(src)
            pi.patch(one, stop=stage)
            d = [i for i in range(len(src)) if one[i] != img[i]]
            assert set(d) <= {pi.off(va), pi.off(va) + 1} and d, stage
        print(f"patch-ibec-upgrade: {len(changed)} bytes, only at the known sites, idempotent: PASS")
    else:
        print("patch-ibec-upgrade: skipped (no --ibec)")


if __name__ == "__main__":
    main()
