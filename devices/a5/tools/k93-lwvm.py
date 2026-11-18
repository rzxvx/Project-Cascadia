#!/usr/bin/env python3
"""k93-lwvm.py -- the iPad 2's LwVM partition table, read and planned on the host.

iOS 5+ keeps its partitions in an LwVM table at byte 0 of the NAND's logical
disk (iOS's /dev/rdisk0 = YAFTL's LPN 0; docs/research/k93-nand.md 9).  The
disk is cut into 1024 chunks (32 MB on the 32 GB iPad 2); a u16 map at 0x800
says which partition and which of its chunks each physical chunk holds, so a
partition need not be contiguous.  Partition records at 0x200 (0x80 bytes:
type GUID, GUID, begin, end, attributes, UTF-16 name) place each partition in
the virtual disk iOS builds from them (/dev/disk0s1, a synthesized GPT);
partition offset X lives in physical chunk map[X / chunk], at X % chunk.
Map values: 0xF000 chunk 0 (the header's), 0xF111 a free chunk, 0xF3FF a
chunk past the end of the disk (from media / chunk on, the partial one
included), else (partition << 12) | its logical chunk.  A CRC32 of the first
4 KB (with the CRC field zeroed) sits at 0x2c.  iOS keeps an identical backup
of the header at the end of chunk 0 (LPN 4095 on the iPad 2), used only when
the main one cannot be read: write both.  check() is the kernel's own
LightweightVolumeManager sanity check (iOS 6.1.3, build/k93/research-write/
lwvm-re.md): a table failing it gets no partitions at all -- no disk0s1, no
root, an iOS that hangs at the Apple logo (2026-11-15: one 0xF3FF inside the
disk did exactly that).  GUIDs
are stored in plain (big-endian) byte order, not GPT's mixed one: iOS's
System and Data types read "HFS\0" + 0000-11AA-AA11-00306543ECAC.

    k93-lwvm.py show TABLE                 the partitions and the chunk map
    k93-lwvm.py check TABLE                iOS's own sanity check (exit 1: refused)
    k93-lwvm.py plan TABLE --data-hfs BYTES --data-used BYTES
                     --root MB --swap MB [--keep-free MB] --out NEW
                                           Data shrunk to make room for a
                                           Linux root and a swap partition:
                                           prints Data's new HFS+ size (for
                                           tools/ios/hfs-resize), writes NEW
    k93-lwvm.py room TABLE --data-hfs BYTES --data-used BYTES [--keep-free MB]
                                           the most Linux + swap can take
    k93-lwvm.py remove TABLE --out NEW     the Linux partitions dropped, their
                                           chunks back to Data (grow its HFS+
                                           with hfs-resize after the reboot)

TABLE and NEW are the 8192-byte first page of the disk (dd bs=8192 count=1 of
/dev/rdisk0 on iOS).  Nothing here touches the iPad: tools/k93-part.sh reads
and writes the table there.  Python's standard library only.
"""
import argparse
import struct
import sys
import uuid
import zlib

PAGE = 8192
HDR, FREE, GONE = 0xF000, 0xF111, 0xF3FF
MAGICS = (uuid.UUID("6a9088cf-8afd-630a-e351-e24887e0b98b").bytes,
          uuid.UUID("b189a519-4f59-4b1d-ad44-1e127aaf4539").bytes)
HFS = uuid.UUID("48465300-0000-11aa-aa11-00306543ecac")
LINUX = uuid.UUID("0fc63daf-8483-4772-8e79-3d69d8477de4")
SWAP = uuid.UUID("0657fd6d-a4ab-43c4-84e5-0933c84b4f4f")
TYPE_NAMES = {HFS: "HFS+", LINUX: "Linux", SWAP: "Linux swap"}
NAMES = ("Linux", "Swap")          # what k93lv / initramfs look the partitions up by
MB = 1 << 20


class Table:
    def __init__(self, raw):
        if len(raw) != PAGE:
            sys.exit("k93-lwvm: %d bytes; expected the 8192-byte first page of /dev/rdisk0" % len(raw))
        if raw[:16] not in MAGICS:
            sys.exit("k93-lwvm: no LwVM header (magic %s)" % raw[:16].hex())
        self.raw = bytearray(raw)
        self.media, self.n, self.crc = struct.unpack_from("<QII", raw, 0x20)
        if crc_of(raw) != self.crc:
            sys.exit("k93-lwvm: CRC mismatch (%08x stored, %08x computed): not touching it" % (self.crc, crc_of(raw)))
        if not 1 <= self.n <= 12:
            sys.exit("k93-lwvm: %d partitions?" % self.n)
        self.shift = ((self.media - 1) >> 10).bit_length()      # FUN_8069f6f0
        self.chunk = 1 << self.shift
        self.nchunks = self.media >> self.shift                 # whole chunks only
        self.first = 2 * PAGE + 0x4000                          # 0x8000
        self.last = self.first + ((self.nchunks - 8) << self.shift)
        self.map = list(struct.unpack_from("<1024H", raw, 0x800))
        self.parts = []
        for i in range(self.n):
            r = raw[0x200 + i * 0x80:0x280 + i * 0x80]
            begin, end, attr = struct.unpack_from("<QQQ", r, 32)
            self.parts.append(dict(type=uuid.UUID(bytes=bytes(r[:16])), guid=uuid.UUID(bytes=bytes(r[16:32])),
                                   begin=begin, end=end, attr=attr,
                                   name=r[56:128].decode("utf-16-le").rstrip("\0")))
        self.check()

    def phys(self, i):
        """physical chunk of each logical chunk of partition i"""
        got = {}
        for c, v in enumerate(self.map):
            if v >> 12 == i:
                if (v & 0xfff) in got:
                    sys.exit("k93-lwvm: partition %d logical chunk %d mapped twice" % (i, v & 0xfff))
                got[v & 0xfff] = c
        return [got[k] for k in sorted(got)]

    def check(self):
        bad = kernel_check(bytes(self.raw), self.media)
        if bad:
            sys.exit("k93-lwvm: iOS would refuse this table: %s" % bad)

    def show(self):
        print("LwVM: %d partitions, media %d bytes (%.2f GiB), chunks of %d MB, CRC %08x ok"
              % (self.n, self.media, self.media / 2**30, self.chunk // MB, self.crc))
        for i, p in enumerate(self.parts):
            print("  %d  %-6s %-10s %9.1f MB  virtual %#x-%#x  %d chunks  attr %#x"
                  % (i, p["name"], TYPE_NAMES.get(p["type"], str(p["type"])), (p["end"] - p["begin"]) / MB,
                     p["begin"], p["end"], len(self.phys(i)), p["attr"]))
        free = [c for c in range(1, self.nchunks) if self.map[c] == FREE]
        print("  %d chunks on the disk (%d past it), %d free; partitions may span %#x-%#x; passes iOS's check"
              % (self.nchunks, 1024 - self.nchunks, len(free), self.first, self.last))

    def pack(self):
        raw = self.raw
        struct.pack_into("<I", raw, 0x28, len(self.parts))
        raw[0x200:0x800] = bytes(0x600)
        for i, p in enumerate(self.parts):
            name = p["name"].encode("utf-16-le")[:72]
            rec = p["type"].bytes + p["guid"].bytes + struct.pack("<QQQ", p["begin"], p["end"], p["attr"]) \
                + name + bytes(72 - len(name))
            raw[0x200 + i * 0x80:0x280 + i * 0x80] = rec
        struct.pack_into("<1024H", raw, 0x800, *self.map)
        struct.pack_into("<I", raw, 0x2c, crc_of(raw))
        return bytes(raw)


def crc_of(raw):
    return zlib.crc32(bytes(raw[:0x2c]) + b"\0\0\0\0" + bytes(raw[0x30:0x1000])) & 0xffffffff


def kernel_check(h, media, block=PAGE):
    """iOS 6.1.3 LightweightVolumeManager's checks (kext 0x8069f000: the CRC 0x8069fa04, the sanity
    check 0x8069fac4, initFromMaster's panics 0x806a3dcc); the first failure, as the kernel logs it, or None"""
    u64 = lambda o: struct.unpack_from("<Q", h, o)[0]
    if h[:16] not in MAGICS:
        return "LwVM: media header mismatch"
    if h[:16] == MAGICS[0] and crc_of(h) != struct.unpack_from("<I", h, 0x2c)[0]:
        return "media header failed validation (CRC)"
    if u64(0x20) != media:
        return "LwVM: media size mismatch"
    shift = ((media - 1) >> 10).bit_length()
    ch, n = 1 << shift, media >> shift
    first = 2 * block + 0x4000
    last = first + ((n - 8) << shift)
    m = struct.unpack_from("<1024H", h, 0x800)
    cnt = struct.unpack_from("<I", h, 0x28)[0]
    if cnt > 8:
        return "LwVM: illegal partition count"
    for i in range(cnt):
        r = 0x200 + i * 0x80
        b, e = u64(r + 0x20), u64(r + 0x28)
        if not any(h[r:r + 16]):
            return "LwVM: partition %d has type zero" % i
        if not any(h[r + 16:r + 32]):
            return "LwVM: partition %d has identity zero" % i
        if b < first:
            return "LwVM: partition %d starts too low" % i
        if e > last:
            return "LwVM: partition %d ends too high" % i
        if e <= b:
            return "LwVM: partition %d start/end not in order" % i
        if b % block or e % block:
            return "LwVM: partition %d start/end not block aligned" % i
        for j in range(i):
            if e > u64(0x220 + j * 0x80) and b < u64(0x228 + j * 0x80):
                return "LwVM: partition %d overlaps partition %d" % (i, j)
        need = (e - 1 - b + ch) >> shift
        for L in range(need):
            want = (L & 0x3ff) | (i & 0xf) << 12
            if n < 2 or sum(1 for c in range(1, n) if m[c] == want) != 1:
                return "LwVM: partition %d missing chunk 0x%04x" % (i, L)
        for c, v in enumerate(m):                       # initFromMaster: a panic, not a refusal
            if v >> 12 == i and (v & 0x3ff) >= need:
                return "kernel PANIC at boot: partition %d's logical chunk %d >= its %d chunks" % (i, v & 0x3ff, need)
    for i in range(cnt, 8):
        r = 0x200 + i * 0x80
        if any(h[r:r + 0x30]):
            return "LwVM: partition %d orphaned" % i
    if m[0] != HDR:
        return "LwVM: chunk zero not marked reserved"
    for c in range(1, n):
        v = m[c]
        if v == FREE:
            continue
        p = v >> 12
        if p >= cnt:
            return "LwVM: chunk 0x%04x claims nonexistent partition (0x%04x)" % (c, v)
        r = 0x200 + p * 0x80
        if (v & 0x3ff) << shift > u64(r + 0x28) - u64(r + 0x20):
            return "LwVM: chunk 0x%04x assigned to invalid partition offset (0x%04x)" % (c, v)
    for c in range(n, 1024):
        if m[c] != GONE:
            return "LwVM: chunk 0x%04x not invalid (0x%04x)" % (c, m[c])
    return None


def load(path):
    return Table(open(path, "rb").read())


def plan(t, a):
    if t.n != 2 or [p["name"] for p in t.parts] != ["System", "Data"] or any(p["type"] != HFS for p in t.parts):
        sys.exit("k93-lwvm: expected exactly System and Data (iOS's own table); remove the Linux partitions first")
    c = t.chunk
    if a.root <= 0 or a.swap <= 0:
        sys.exit("k93-lwvm: both sizes must be > 0")
    a.root = -(-a.root * MB // c) * c // MB              # whole chunks (32 MB on the 32 GB iPad 2)
    a.swap = -(-a.swap * MB // c) * c // MB
    data = t.parts[1]
    size = data["end"] - data["begin"]
    if a.data_hfs != size:
        sys.exit("k93-lwvm: Data's HFS+ is %d bytes but its partition %d: resize state unknown, stopping"
                 % (a.data_hfs, size))
    ph = t.phys(1)
    keep = (size - (a.root + a.swap) * MB) // c          # whole chunks Data keeps
    new_size = keep * c
    if new_size < a.data_used + a.keep_free * MB:
        sys.exit("k93-lwvm: Data would be %.0f MB with %.0f MB used: less than %d MB left for iOS"
                 % (new_size / MB, a.data_used / MB, a.keep_free))
    spare = ph[keep:]
    nr, ns = a.root * MB // c, a.swap * MB // c
    assert len(spare) >= nr + ns
    if data["begin"] + new_size + (a.root + a.swap) * MB > t.last:
        sys.exit("k93-lwvm: the partitions would end past %#x, where iOS stops" % t.last)
    for j, p in enumerate(spare):
        t.map[p] = (2 << 12 | j) if j < nr else (3 << 12 | (j - nr)) if j < nr + ns else FREE
    data["end"] = data["begin"] + new_size
    rb = data["end"]
    t.parts.append(dict(type=LINUX, guid=uuid.uuid4(), begin=rb, end=rb + a.root * MB, attr=0, name=NAMES[0]))
    t.parts.append(dict(type=SWAP, guid=uuid.uuid4(), begin=rb + a.root * MB, end=rb + (a.root + a.swap) * MB,
                        attr=0, name=NAMES[1]))
    out = t.pack()
    t2 = Table(out)                                       # parse it back: every check again
    assert [p["name"] for p in t2.parts] == ["System", "Data"] + list(NAMES)
    assert t2.parts[0] == t.parts[0] and t2.parts[1]["end"] - t2.parts[1]["begin"] == new_size
    assert t2.phys(0) == t.phys(0) and t2.phys(1) == ph[:keep]
    open(a.out, "wb").write(out)
    print("data_hfs_bytes=%d" % new_size)
    print("Data %.0f MB -> %.0f MB (%.0f MB used); new: Linux %d MB, Swap %d MB; %d chunk(s) left free"
          % (size / MB, new_size / MB, a.data_used / MB, a.root, a.swap, len(spare) - nr - ns))
    t2.show()


def room(t, a):
    data = t.parts[-1] if t.n == 2 else None
    if data is None or data["name"] != "Data":
        print("room_mb=0")
        return
    size = data["end"] - data["begin"]
    keep = -(-(a.data_used + a.keep_free * MB) // t.chunk)
    print("room_mb=%d" % max(0, (size // t.chunk - keep) * t.chunk // MB))
    print("chunk_mb=%d" % (t.chunk // MB))
    print("data_partition_bytes=%d" % size)


def remove(t, a):
    names = [p["name"] for p in t.parts]
    if names[:2] != ["System", "Data"] or names[2:] != list(NAMES[:len(names) - 2]):
        sys.exit("k93-lwvm: not a table this tool made (%s)" % ", ".join(names))
    data = t.parts[1]
    ph = t.phys(1)
    gone = [c for c, v in enumerate(t.map) if v >> 12 != 0xF and 2 <= v >> 12 < t.n]
    # Data's chunks beyond its end: the Linux ones, then free ones -- on the disk, never past it
    free = [c for c in range(1, t.nchunks) if t.map[c] == FREE]
    back = sorted(gone)
    end = t.parts[-1]["end"]
    t.parts = t.parts[:2]
    total = end - data["begin"]
    need = -(-total // t.chunk)
    extra = back + [c for c in free if c not in back]
    while len(ph) < need and extra:
        ph.append(extra.pop(0))
    if len(ph) < need:
        sys.exit("k93-lwvm: not enough chunks to give Data its old end back")
    for c in back:
        t.map[c] = FREE
    for j, c in enumerate(ph):
        t.map[c] = 1 << 12 | j
    data["end"] = end
    out = t.pack()
    Table(out)
    open(a.out, "wb").write(out)
    print("data_partition_bytes=%d" % total)
    Table(out).show()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("show")
    s.add_argument("table")
    k = sub.add_parser("check")
    k.add_argument("table")
    p = sub.add_parser("plan")
    p.add_argument("table")
    p.add_argument("--data-hfs", type=int, required=True, help="Data's HFS+ size now, bytes (df's blocks x 1024)")
    p.add_argument("--data-used", type=int, required=True, help="bytes used on Data")
    p.add_argument("--root", type=int, required=True, help="Linux root partition, MB")
    p.add_argument("--swap", type=int, required=True, help="swap partition, MB")
    p.add_argument("--keep-free", type=int, default=2048, help="MB that must stay free on Data (default 2048)")
    p.add_argument("--out", required=True)
    m = sub.add_parser("room")
    m.add_argument("table")
    m.add_argument("--data-hfs", type=int, required=True)
    m.add_argument("--data-used", type=int, required=True)
    m.add_argument("--keep-free", type=int, default=2048)
    r = sub.add_parser("remove")
    r.add_argument("table")
    r.add_argument("--out", required=True)
    a = ap.parse_args()
    if a.cmd == "check":
        raw = open(a.table, "rb").read()
        bad = kernel_check(raw[:0x1000], struct.unpack_from("<Q", raw, 0x20)[0])
        print("%s: %s" % (a.table, bad or "passes iOS's check"))
        sys.exit(1 if bad else 0)
    t = load(a.table)
    if a.cmd == "show":
        t.show()
    elif a.cmd == "plan":
        plan(t, a)
    elif a.cmd == "room":
        room(t, a)
    else:
        remove(t, a)


main()
