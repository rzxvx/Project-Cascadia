#!/usr/bin/env python3
"""Copy one file out of an IPSW's root filesystem, without mounting anything.

    rootfs-extract.py IPSW DMG KEY PATH OUT [PATH OUT ...]

    IPSW   the stock .ipsw
    DMG    the root filesystem's name inside it (BuildManifest's "OS" entry)
    KEY    its 72-hex-digit key: AES-128 key, then HMAC-SHA1 key
    PATH   absolute path inside the root filesystem
    OUT    where to write the file (pairs repeat: one pass over the DMG)

Written for /usr/share/firmware/multitouch/P105.mtprops, the digitizer's
firmware: touch needs it, it is Apple's and cannot ship with the repository,
and the user already supplies the IPSW it is in.  ./cascadia firmware runs this
inside the build image, next to the iBSS/iBEC steps, so it needs only Python
and pycryptodome -- no dmg or hfsplus tools, no mounting, same on every host.

Three layers, each read only where it is needed:

  encrcdsa   the FileVault-style wrapper of iOS 8's root DMG: fixed-size
             chunks, AES-128-CBC, each chunk's IV the HMAC-SHA1 of its number
             (as vfdecrypt does it)
  UDIF       the DMG itself: a koly trailer, a plist of partitions, and per
             partition a mish table of chunks -- zlib here, raw, zero, ADC or
             bzip2 elsewhere
  HFS+       the partition: the catalog B-tree's leaves walked in order, the
             file's data fork read through its extents, or decmpfs-compressed
             data from its attribute or resource fork
"""
import bisect
import bz2
import hashlib
import hmac
import os
import plistlib
import shutil
import struct
import sys
import zipfile
import zlib

from Crypto.Cipher import AES


def fail(msg):
    sys.exit("rootfs-extract: " + msg)


def u16(b, o):
    return struct.unpack_from(">H", b, o)[0]


def u32(b, o):
    return struct.unpack_from(">I", b, o)[0]


def u64(b, o):
    return struct.unpack_from(">Q", b, o)[0]


class Plain:
    """The DMG as it is, for an image that is not wrapped."""

    def __init__(self, f):
        self.f = f
        f.seek(0, os.SEEK_END)
        self.size = f.tell()

    def read(self, off, n):
        self.f.seek(off)
        return self.f.read(n)


class Encrcdsa:
    def __init__(self, f, key):
        self.f = f
        f.seek(0)
        h = f.read(0x48)
        if u32(h, 8) != 2:
            fail("encrcdsa version %d, only 2 is known" % u32(h, 8))
        self.bs = u32(h, 52)
        self.size = u64(h, 56)
        self.base = u64(h, 64)
        self.aes, self.mac = key[:16], key[16:36]
        self.cache = {}

    def chunk(self, n):
        c = self.cache.get(n)
        if c is None:
            self.f.seek(self.base + n * self.bs)
            ct = self.f.read(self.bs)
            iv = hmac.new(self.mac, struct.pack(">I", n), hashlib.sha1).digest()[:16]
            c = AES.new(self.aes, AES.MODE_CBC, iv).decrypt(ct)
            if len(self.cache) > 256:
                self.cache.clear()
            self.cache[n] = c
        return c

    def read(self, off, n):
        out = bytearray()
        n = min(n, self.size - off)
        while n > 0:
            c = self.chunk(off // self.bs)
            piece = c[off % self.bs:off % self.bs + n]
            if not piece:
                fail("encrcdsa: read past the last chunk")
            out += piece
            off += len(piece)
            n -= len(piece)
        return bytes(out)


def adc(data, outlen):
    out = bytearray()
    i = 0
    while i < len(data) and len(out) < outlen:
        b = data[i]
        if b & 0x80:
            n = (b & 0x7F) + 1
            out += data[i + 1:i + 1 + n]
            i += 1 + n
            continue
        if b & 0x40:
            n, dist = (b & 0x3F) + 4, (data[i + 1] << 8 | data[i + 2]) + 1
            i += 3
        else:
            n, dist = ((b & 0x3C) >> 2) + 3, ((b & 3) << 8 | data[i + 1]) + 1
            i += 2
        for _ in range(n):          # byte by byte: the copy may overlap itself
            out.append(out[-dist])
    return bytes(out)


class Udif:
    """One partition of a UDIF image, as a flat byte range."""

    def __init__(self, src):
        self.src = src
        koly = src.read(src.size - 512, 512)
        if koly[:4] != b"koly":
            fail("no koly trailer -- wrong key, or not a DMG")
        self.dfork = u64(koly, 24)
        pl = plistlib.loads(src.read(u64(koly, 216), u64(koly, 224)))
        parts = pl["resource-fork"]["blkx"]
        hfs = [p for p in parts if "Apple_HFS" in p.get("Name", "") or "Apple_HFS" in p.get("CFName", "")]
        if len(hfs) != 1:
            fail("expected one Apple_HFS partition, found: %s" % [p.get("Name") for p in parts])
        mish = hfs[0]["Data"]
        if mish[:4] != b"mish":
            fail("partition table entry is not a mish block")
        self.doff = u64(mish, 24)
        self.chunks = []
        for i in range(u32(mish, 200)):
            o = 204 + 40 * i
            kind, sect, cnt, coff, clen = struct.unpack_from(">IxxxxQQQQ", mish, o)
            if kind in (0x7FFFFFFE, 0xFFFFFFFF):
                continue
            self.chunks.append((sect * 512, cnt * 512, kind, coff, clen))
        self.chunks.sort()
        self.starts = [c[0] for c in self.chunks]
        self.cache = {}

    def chunk(self, i):
        c = self.cache.get(i)
        if c is not None:
            return c
        start, size, kind, coff, clen = self.chunks[i]
        if kind in (0, 2):
            c = bytes(size)
        else:
            raw = self.src.read(self.dfork + self.doff + coff, clen)
            if kind == 1:
                c = raw
            elif kind == 0x80000005:
                c = zlib.decompress(raw)
            elif kind == 0x80000006:
                c = bz2.decompress(raw)
            elif kind == 0x80000004:
                c = adc(raw, size)
            else:
                fail("DMG chunk type 0x%08x is not handled" % kind)
        if len(self.cache) > 64:
            self.cache.clear()
        self.cache[i] = c
        return c

    def read(self, off, n):
        out = bytearray()
        while n > 0:
            i = bisect.bisect_right(self.starts, off) - 1
            if i < 0:
                fail("read before the partition's first chunk")
            start, size = self.chunks[i][:2]
            if off >= start + size:
                fail("read in a gap between chunks at 0x%x" % off)
            piece = self.chunk(i)[off - start:off - start + n]
            out += piece
            off += len(piece)
            n -= len(piece)
        return bytes(out)


class Fork:
    def __init__(self, hfs, rec):
        self.hfs = hfs
        self.size = u64(rec, 0)
        self.ext = [(u32(rec, 16 + 8 * i), u32(rec, 20 + 8 * i)) for i in range(8)]
        covered = sum(c for _, c in self.ext) * hfs.bs
        if covered < self.size:
            fail("a fork with more than eight extents -- the overflow file is not read")

    def read(self, off, n):
        out = bytearray()
        n = min(n, self.size - off)
        bs = self.hfs.bs
        for start, count in self.ext:
            span = count * bs
            if n > 0 and off < span:
                take = min(n, span - off)
                out += self.hfs.img.read(start * bs + off, take)
                n -= take
                off = 0
            else:
                off -= span
        return bytes(out)

    def all(self):
        return self.read(0, self.size)


class Hfs:
    def __init__(self, img):
        self.img = img
        vh = img.read(1024, 512)
        if vh[:2] not in (b"H+", b"HX"):
            fail("the partition is not HFS+ (signature %r)" % vh[:2])
        self.bs = u32(vh, 40)
        self.catalog = Fork(self, vh[272:352])
        self.attributes = Fork(self, vh[352:432])

    def leaves(self, fork):
        """(node, record offset) for every leaf record, in key order."""
        hdr = fork.read(0, 512)
        ns = u16(hdr, 32)
        n = u32(hdr, 24)
        while n:
            node = fork.read(n * ns, ns)
            for i in range(u16(node, 10)):
                yield node, u16(node, ns - 2 * (i + 1))
            n = u32(node, 0)

    def lookup(self, path):
        names = [p for p in path.split("/") if p]
        want = set(names)
        found = {}
        for node, o in self.leaves(self.catalog):
            klen = u16(node, o)
            nlen = u16(node, o + 6)
            name = node[o + 8:o + 8 + 2 * nlen].decode("utf-16-be")
            if name in want:
                found[(u32(node, o + 2), name)] = node[o + 2 + klen:o + 2 + klen + 248]
        parent = 2                                  # kHFSRootFolderID
        for i, name in enumerate(names):
            rec = found.get((parent, name))
            last = i == len(names) - 1
            if rec is None:
                fail("%s: no '%s'" % (path, name))
            kind = struct.unpack_from(">h", rec, 0)[0]
            if not last:
                if kind != 1:
                    fail("%s: '%s' is not a folder" % (path, name))
                parent = u32(rec, 8)
            elif kind != 2:
                fail("%s is not a file" % path)
        return rec

    def xattr(self, cnid, want):
        for node, o in self.leaves(self.attributes):
            klen = u16(node, o)
            if u32(node, o + 4) != cnid:
                continue
            nlen = u16(node, o + 12)
            if node[o + 14:o + 14 + 2 * nlen].decode("utf-16-be") != want:
                continue
            d = o + 2 + klen
            if u32(node, d) != 0x10:
                fail("%s of file %d is not inline data" % (want, cnid))
            return node[d + 16:d + 16 + u32(node, d + 12)]
        return None

    def read_file(self, rec):
        data = Fork(self, rec[88:168])
        if not rec[41] & 0x20:                      # UF_COMPRESSED
            return data.all()
        cnid = u32(rec, 8)
        hdr = self.xattr(cnid, "com.apple.decmpfs")
        if hdr is None or hdr[:4] != b"fpmc":
            fail("compressed file without a decmpfs header")
        kind, size = struct.unpack_from("<IQ", hdr, 4)
        if kind == 3:
            body = hdr[16:]
            out = body[1:] if body[:1] == b"\xff" else zlib.decompress(body)
        elif kind == 4:
            rsrc = Fork(self, rec[168:248]).all()
            base = u32(rsrc, 0) + 4
            nblk = struct.unpack_from("<I", rsrc, base)[0]
            out = bytearray()
            for i in range(nblk):
                off, ln = struct.unpack_from("<II", rsrc, base + 4 + 8 * i)
                blk = rsrc[base + off:base + off + ln]
                out += blk[1:] if blk[:1] == b"\xff" else zlib.decompress(blk)
            out = bytes(out)
        else:
            fail("decmpfs type %d is not handled" % kind)
        if len(out) != size:
            fail("decompressed %d bytes, the header says %d" % (len(out), size))
        return out


def main():
    if len(sys.argv) < 6 or len(sys.argv) % 2:
        sys.exit(__doc__)
    ipsw, dmg, keyhex = sys.argv[1:4]
    pairs = list(zip(sys.argv[4::2], sys.argv[5::2]))
    key = bytes.fromhex(keyhex)
    if len(key) != 36:
        fail("the key is %d bytes, not 36" % len(key))

    # The DMG is deflated inside the zip, and a deflate stream cannot be read
    # at random -- so it goes to a file of its own first, next to the output,
    # and away again at the end.
    tmp = pairs[0][1] + ".dmg.tmp"
    try:
        with zipfile.ZipFile(ipsw) as z, z.open(dmg) as src, open(tmp, "wb") as dst:
            shutil.copyfileobj(src, dst, 1 << 20)
        with open(tmp, "rb") as f:
            src = Encrcdsa(f, key) if f.read(8) == b"encrcdsa" else Plain(f)
            hfs = Hfs(Udif(src))
            for path, out in pairs:
                data = hfs.read_file(hfs.lookup(path))
                with open(out, "wb") as o:
                    o.write(data)
                print("    %s: %d bytes -> %s" % (path, len(data), out))
    finally:
        if os.path.exists(tmp):
            os.remove(tmp)


if __name__ == "__main__":
    main()
