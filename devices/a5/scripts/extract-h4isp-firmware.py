#!/usr/bin/env python3
"""The iPad 2's camera ISP firmware and its sensors' set-files, out of the IPSW.

    extract-h4isp-firmware.py IPSW OUTDIR

Writes, into OUTDIR (what drivers/media/platform/apple/apple-h4isp.c asks
for under /lib/firmware/apple/):

    h4isp-firmware.bin        the ISP CPU's firmware ("HERA-APPLE",
                              H4ISP-12.50.00): H4ISPServices' H4ISPFIRMWARE
                              section
    h4isp-setfile-9726.bin    the rear sensor's tuning (set-file 1011_04XX)
    h4isp-setfile-7739.bin    the front sensor's (0011_01XX)

Both come from iOS 8.4.1 (12H321) for the iPad2,1, the IPSW the K93 build
already uses: its root filesystem (the public 12H321 key) holds the dyld
shared cache, the cache holds the H4ISPServices framework, and that has the
firmware and the set-files as sections of their own.  docs/research/
k93-camera.md has how iOS picks a set-file (by the sensor's ID, the ADT's
sensor-type and two module IDs, 1 and 1 on the K93's); the two here are the
ones iOS loads on this iPad.  Every file is checked against its SHA-1.

Reads the root filesystem through scripts/rootfs-extract.py's layers -- only
the chunks it needs, without mounting anything -- so it runs in the build
image like the rest of ./cascadia k93 firmware.
"""
import hashlib
import importlib.util
import os
import shutil
import struct
import sys
import tempfile
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
_spec = importlib.util.spec_from_file_location("rootfs_extract",
                                               os.path.join(HERE, "rootfs-extract.py"))
rx = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(rx)

DMG = "058-24482-024.dmg"
KEY = "d33bfc10cc397ae008d2e56685acb172b74f0eccbe652ecd52e234ab12705980b6483b60"
CACHE = "/System/Library/Caches/com.apple.dyld/dyld_shared_cache_armv7"
IMAGE = b"/System/Library/PrivateFrameworks/H4ISPServices.framework/H4ISPServices"

FIRMWARE_SHA1 = "f653f9a294d95c5f32ee618a30598512d667005f"
# sensor ID: (offset in H4ISPSETFILE, size, first bytes -- the set-file's own
# name, 1011_04 and 0011_01 -- and SHA-1)
SETFILES = {
    0x9726: (0x88000, 0x4efc, bytes.fromhex("101104"),
             "21fe1c7bf43dea39a0ed12c28f8a8a8840a11f8f"),
    0x7739: (0x97000, 0x4840, bytes.fromhex("001101"),
             "034617a87f2913f9f77c390cf81f162af77d3e3a"),
}


def fail(msg):
    sys.exit("extract-h4isp-firmware: " + msg)


class Cache:
    """the dyld shared cache, read at random through its file's fork"""

    def __init__(self, fork):
        self.fork = fork
        hdr = fork.read(0, 0x20)
        if not hdr.startswith(b"dyld_v1"):
            fail("%s is not a dyld shared cache" % CACHE)
        moff, mcount, ioff, icount = struct.unpack_from("<4I", hdr, 0x10)
        self.maps = [struct.unpack_from("<3Q", fork.read(moff + 32 * i, 24))
                     for i in range(mcount)]
        self.images = [struct.unpack_from("<Q16xI", fork.read(ioff + 32 * i, 28))
                       for i in range(icount)]

    def offset(self, addr):
        for base, size, off in self.maps:
            if base <= addr < base + size:
                return off + addr - base
        fail("cache address %#x is in no mapping" % addr)

    def read(self, addr, n):
        return self.fork.read(self.offset(addr), n)

    def path(self, i):
        raw = self.fork.read(self.images[i][1], 256)
        return raw.split(b"\0", 1)[0]

    def sections(self, image):
        """{name: (address, size)} of an image's sections"""
        for i in range(len(self.images)):
            if self.path(i) == image:
                break
        else:
            fail("no %s in the cache" % image.decode())
        base = self.images[i][0]
        magic, _, _, _, ncmds, sizeofcmds = struct.unpack_from("<6I", self.read(base, 24))
        if magic != 0xfeedface:
            fail("%s is not a 32-bit Mach-O" % image.decode())
        cmds = self.read(base + 28, sizeofcmds)
        found, o = {}, 0
        for _ in range(ncmds):
            cmd, size = struct.unpack_from("<2I", cmds, o)
            if cmd == 1:                                # LC_SEGMENT
                nsects = struct.unpack_from("<I", cmds, o + 48)[0]
                for k in range(nsects):
                    s = o + 56 + 68 * k
                    name = cmds[s:s + 16].split(b"\0", 1)[0].decode()
                    found[name] = struct.unpack_from("<2I", cmds, s + 32)
            o += size
        return found


def check(name, data, sha1):
    got = hashlib.sha1(data).hexdigest()
    if got != sha1:
        fail("%s: SHA-1 %s, not %s -- not 12H321's iPad2,1 files?" % (name, got, sha1))


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    ipsw, outdir = sys.argv[1:]
    os.makedirs(outdir, exist_ok=True)
    # The DMG is deflated inside the zip, so it goes to a file of its own to
    # be read at random, and away again at the end (about 1 GB, for a minute).
    with tempfile.TemporaryDirectory(dir=outdir) as tmpdir:
        tmp = os.path.join(tmpdir, "rootfs.dmg")
        with zipfile.ZipFile(ipsw) as z, z.open(DMG) as src, open(tmp, "wb") as dst:
            shutil.copyfileobj(src, dst, 1 << 20)
        with open(tmp, "rb") as f:
            src = rx.Encrcdsa(f, bytes.fromhex(KEY)) if f.read(8) == b"encrcdsa" else rx.Plain(f)
            hfs = rx.Hfs(rx.Udif(src))
            rec = hfs.lookup(CACHE)
            if rec[41] & 0x20:
                fail("%s is compressed; this reads it uncompressed" % CACHE)
            cache = Cache(rx.Fork(hfs, rec[88:168], rx.u32(rec, 8), 0))
            sects = cache.sections(IMAGE)
            for name in ("H4ISPFIRMWARE", "H4ISPSETFILE"):
                if name not in sects:
                    fail("H4ISPServices has no %s section" % name)
            addr, size = sects["H4ISPFIRMWARE"]
            firmware = cache.read(addr, size)
            setbase, setsize = sects["H4ISPSETFILE"]
            setfiles = {}
            for sensor, (off, n, head, sha1) in SETFILES.items():
                if off + n > setsize:
                    fail("set-file %04x lies past H4ISPSETFILE's end" % sensor)
                data = cache.read(setbase + off, n)
                if not data.startswith(head):
                    fail("set-file %04x starts %s, not %s" % (sensor, data[:3].hex(), head.hex()))
                setfiles[sensor] = (data, sha1)

    check("h4isp-firmware.bin", firmware, FIRMWARE_SHA1)
    if firmware[0x1040:0x1043] != b"adc":
        fail("the firmware has no 'adc' at 0x1040")
    out = [("h4isp-firmware.bin", firmware)]
    for sensor, (data, sha1) in setfiles.items():
        name = "h4isp-setfile-%04x.bin" % sensor
        check(name, data, sha1)
        out.append((name, data))
    for name, data in out:
        with open(os.path.join(outdir, name), "wb") as o:
            o.write(data)
        print("    ok: apple/%s, %d bytes" % (name, len(data)))


if __name__ == "__main__":
    main()
