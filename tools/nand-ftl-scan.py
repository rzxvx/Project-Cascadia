#!/usr/bin/env python3
"""nand-ftl-scan.py -- rebuild the iPad's disk0 from a raw NAND dump, offline.

Input: the files `nandctl dump` writes, per bus and CAU: busB-cauC.bin from
block 0, and busB-cauC.bNNN.bin for a dump resumed at block NNN (256 pages x
16448 bytes a block, plus a status byte per page in .st).

A PPN page comes off the bus as four 4 KB logical pages, each laid out as
1024 data bytes, 16 bytes of FTL metadata, 3072 data bytes.  The metadata
(AppleSwissPPNFTL / iBEC's SFTL) is plain:

    0      u8   1 = user data
    1      u8   flags
    2..7   u48  write sequence (+1 per 4 KB written, interleaved over the dies)
    8..11  u32  LBA, in 4 KB units (disk0 is 3906250 of them)
    12..15 u32  sequence >> 16

No FTL context is needed to read the disk: for every LBA the copy with the
highest sequence is the live one.  (TRIMmed LBAs come back with stale data;
the filesystem above says they are free, so that does no harm.)

    nand-ftl-scan.py scan DIR MAP              -> MAP (LBA -> page), stats
    nand-ftl-scan.py read DIR MAP LBA [N]      -> N x 4 KB to stdout
    nand-ftl-scan.py image DIR MAP OUT START N -> disk0 [START*4K, +N*4K) into OUT
    nand-ftl-scan.py part DIR MAP OUT NAME     -> one LwVM partition (System, Data)

disk0 starts with an LwVM header (LBA 0): partition records at 0x200 (0x80
bytes: type, GUID, begin, end, attributes, UTF-16 name) and at 0x800 a map
of 1024 16 MB chunks, one u16 per physical chunk: 0xF000 the header, 0xF3FF
free, else partition << 12 | chunk within that partition.

MAP holds 12 bytes per LBA: the sequence (s64, -1 = never written) and
file << 28 | page << 2 | chunk.
"""
import glob
import os
import re
import struct
import sys

PAGE = 16448
CHUNK = 4112
PAGES_PER_FILE = 1064 * 256
NLBA = 16000000000 // 4096


def dump_files(d):
    """[(path, bus, cau, first block)], in a fixed order."""
    out = []
    for path in sorted(glob.glob(os.path.join(d, 'bus[01]-cau[01]*.bin'))):
        m = re.match(r'bus(\d)-cau(\d)(?:\.b(\d+))?\.bin$', os.path.basename(path))
        if m:
            out.append((path, int(m.group(1)), int(m.group(2)), int(m.group(3) or 0)))
    return out


def scan(d, mapfile):
    seqs = [-1] * NLBA
    where = [0] * NLBA
    kinds = {}
    for fi, (path, bus, cau, first) in enumerate(dump_files(d)):
        name = os.path.basename(path)
        st = open(path + '.st', 'rb').read()
        with open(path, 'rb') as f:
            pg = 0
            while True:
                buf = f.read(PAGE * 256)
                if not buf:
                    break
                for i in range(len(buf) // PAGE):
                    p = pg + i
                    if p >= len(st) or st[p] != 0x40:
                        continue
                    base = i * PAGE
                    for k in range(4):
                        m = buf[base + k * CHUNK + 1024:base + k * CHUNK + 1040]
                        kinds[m[0]] = kinds.get(m[0], 0) + 1
                        if m[0] != 1:
                            continue
                        seq = int.from_bytes(m[2:8], 'little')
                        lba = struct.unpack_from('<I', m, 8)[0]
                        if lba < NLBA and seq > seqs[lba]:
                            seqs[lba] = seq
                            where[lba] = fi << 28 | p << 2 | k
                pg += len(buf) // PAGE
        print('%s: %d pages' % (name, pg), file=sys.stderr)
    with open(mapfile, 'wb') as out:
        for lba in range(NLBA):
            out.write(struct.pack('<qI', seqs[lba], where[lba]))
    mapped = sum(1 for s in seqs if s >= 0)
    print('metadata types seen: %s' % {hex(k): v for k, v in sorted(kinds.items())}, file=sys.stderr)
    print('LBAs mapped: %d of %d (%.1f%%)' % (mapped, NLBA, 100.0 * mapped / NLBA), file=sys.stderr)


class Disk:
    def __init__(self, d, mapfile):
        self.l2p = open(mapfile, 'rb')
        self.f = [open(f[0], 'rb') for f in dump_files(d)]

    def read4k(self, lba):
        self.l2p.seek(lba * 12)
        seq, w = struct.unpack('<qI', self.l2p.read(12))
        if seq < 0:
            return None
        f = self.f[w >> 28]
        off = ((w >> 2) & 0x3ffffff) * PAGE + (w & 3) * CHUNK
        f.seek(off)
        c = f.read(CHUNK)
        return c[:1024] + c[1040:]


LWVM_CHUNK = 16 << 20


def lwvm_part(disk, name):
    hdr = disk.read4k(0)
    for i in range(struct.unpack_from('<I', hdr, 0x28)[0]):
        o = 0x200 + i * 0x80
        if hdr[o + 56:o + 0x80].decode('utf-16le').rstrip('\0') == name:
            chunks = struct.unpack_from('<1024H', hdr, 0x800)
            phys = {c & 0xfff: p for p, c in enumerate(chunks) if c >> 12 == i}
            return [phys[c] for c in range(len(phys))]
    raise SystemExit('no LwVM partition %r' % name)


def main():
    cmd, d, mapfile = sys.argv[1], sys.argv[2], sys.argv[3]
    if cmd == 'scan':
        scan(d, mapfile)
    elif cmd == 'read':
        disk = Disk(d, mapfile)
        lba, n = int(sys.argv[4], 0), int(sys.argv[5], 0) if len(sys.argv) > 5 else 1
        for i in range(n):
            sys.stdout.buffer.write(disk.read4k(lba + i) or b'\0' * 4096)
    elif cmd == 'image':
        disk = Disk(d, mapfile)
        start, n = int(sys.argv[5], 0), int(sys.argv[6], 0)
        missing = 0
        with open(sys.argv[4], 'wb') as out:
            for i in range(n):
                b = disk.read4k(start + i)
                if b is None:
                    missing += 1
                    b = b'\0' * 4096
                out.write(b)
        print('%d LBAs written, %d unmapped (zeros)' % (n, missing), file=sys.stderr)
    elif cmd == 'part':
        disk = Disk(d, mapfile)
        per = LWVM_CHUNK // 4096
        missing = 0
        with open(sys.argv[4], 'wb') as out:
            for p in lwvm_part(disk, sys.argv[5]):
                for i in range(per):
                    b = disk.read4k(p * per + i)
                    if b is None:
                        missing += 1
                        b = b'\0' * 4096
                    out.write(b)
        print('%s: %d unmapped 4 KB blocks (zeros)' % (sys.argv[5], missing), file=sys.stderr)


if __name__ == '__main__':
    main()
