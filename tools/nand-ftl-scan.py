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

Inputs must be complete regular files with one status byte per raw page.
Status 0x42 (corrected, refresh requested) is valid data, like 0x40.  MAP/OUT
files are published atomically with mode 0600 only after successful completion;
existing outputs are never overwritten. Choose a fresh path. Reads check
the mapped page's status, LBA and sequence against the dump on every access.
Unmapped LBAs still become zeros; this is recovery, not a writable FTL or a
proof that trimmed data remains live.  Partition output is exactly end-begin
bytes, including a partly filled final allocation chunk.
"""
import glob
import argparse
from contextlib import contextmanager
import os
import re
import stat
import struct
import sys
import tempfile

PAGE = 16448
CHUNK = 4112
PAGES_PER_FILE = 1064 * 256
NLBA = 16000000000 // 4096
GOOD_STATUS = (0x40, 0x42)  # PPN corrected data, including refresh requested.
LWVM_MAGIC = bytes.fromhex('6a9088cf8afd630ae351e24887e0b98b')


@contextmanager
def new_output(path):
    """Publish a complete private file, atomically, without replacing a path.

    The temp and hard link stay on one filesystem.  link() is atomic and
    refuses even a racing creator; unlike replace(), it cannot overwrite an
    old recovery image.  A failed read leaves neither a final nor temp file.
    """
    if os.path.lexists(path):
        raise FileExistsError('output already exists: %s' % path)
    directory = os.path.dirname(os.path.abspath(path))
    fd, tmp = tempfile.mkstemp(prefix='.' + os.path.basename(path) + '.',
                               suffix='.partial', dir=directory)
    try:
        with os.fdopen(fd, 'wb') as out:
            yield out
            out.flush()
            os.fsync(out.fileno())
        os.link(tmp, path)
    finally:
        os.unlink(tmp)


def regular_file(path):
    if not stat.S_ISREG(os.stat(path).st_mode):
        raise ValueError('%s: expected a regular offline dump file' % path)


def checked_dumps(d):
    files = dump_files(d)
    if not files:
        raise ValueError('no NAND dump files found in %s' % d)
    if len(files) > 16:
        raise ValueError('map format supports at most 16 dump files')
    for path, bus, cau, first in files:
        regular_file(path)
        regular_file(path + '.st')
        size = os.path.getsize(path)
        if size % PAGE:
            raise ValueError('%s: truncated raw page' % path)
        pages = size // PAGE
        if first >= 1064 or first * 256 + pages > PAGES_PER_FILE:
            raise ValueError('%s: dump exceeds die geometry' % path)
        if os.path.getsize(path + '.st') != pages:
            raise ValueError('%s: status count does not match page count' % path)
    return files


def dump_files(d):
    """[(path, bus, cau, first block)], in a fixed order."""
    out = []
    for path in sorted(glob.glob(os.path.join(d, 'bus[01]-cau[01]*.bin'))):
        m = re.match(r'bus(\d)-cau(\d)(?:\.b(\d+))?\.bin$', os.path.basename(path))
        if m:
            out.append((path, int(m.group(1)), int(m.group(2)), int(m.group(3) or 0)))
    return out


def scan(d, mapfile):
    files = checked_dumps(d)
    seqs = [-1] * NLBA
    where = [0] * NLBA
    kinds = {}
    for fi, (path, bus, cau, first) in enumerate(files):
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
                    if st[p] not in GOOD_STATUS:
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
    # Never truncate an input dump or an earlier recovery map by accident.
    with new_output(mapfile) as out:
        for lba in range(NLBA):
            out.write(struct.pack('<qI', seqs[lba], where[lba]))
    mapped = sum(1 for s in seqs if s >= 0)
    print('metadata types seen: %s' % {hex(k): v for k, v in sorted(kinds.items())}, file=sys.stderr)
    print('LBAs mapped: %d of %d (%.1f%%)' % (mapped, NLBA, 100.0 * mapped / NLBA), file=sys.stderr)


class Disk:
    def __init__(self, d, mapfile):
        files = checked_dumps(d)
        regular_file(mapfile)
        if os.path.getsize(mapfile) != NLBA * 12:
            raise ValueError('%s: incorrect map length' % mapfile)
        self.l2p = open(mapfile, 'rb')
        self.f = [open(f[0], 'rb') for f in files]
        self.status = [open(f[0] + '.st', 'rb') for f in files]

    def close(self):
        self.l2p.close()
        for f in self.f + self.status:
            f.close()

    def __enter__(self):
        return self

    def __exit__(self, *args):
        self.close()

    def read4k(self, lba):
        if not 0 <= lba < NLBA:
            raise ValueError('LBA %d outside disk0' % lba)
        self.l2p.seek(lba * 12)
        seq, w = struct.unpack('<qI', self.l2p.read(12))
        if seq < 0:
            if seq != -1:
                raise ValueError('LBA %d: invalid map sequence' % lba)
            return None
        fi, page, chunk = w >> 28, (w >> 2) & 0x3ffffff, w & 3
        if fi >= len(self.f):
            raise ValueError('LBA %d: map refers to a missing dump file' % lba)
        f = self.f[fi]
        self.status[fi].seek(page)
        st = self.status[fi].read(1)
        if not st or st[0] not in GOOD_STATUS:
            raise ValueError('LBA %d: map refers to an unreadable page' % lba)
        off = page * PAGE + chunk * CHUNK
        f.seek(off)
        c = f.read(CHUNK)
        if len(c) != CHUNK:
            raise ValueError('LBA %d: map refers beyond the dump' % lba)
        m = c[1024:1040]
        if (m[0] != 1 or struct.unpack_from('<I', m, 8)[0] != lba or
                int.from_bytes(m[2:8], 'little') != seq):
            raise ValueError('LBA %d: stale map or mismatched dump metadata' % lba)
        return c[:1024] + c[1040:]


LWVM_CHUNK = 16 << 20


def lwvm_info(disk, name):
    hdr = disk.read4k(0)
    if hdr is None or len(hdr) != 4096 or hdr[:16] != LWVM_MAGIC:
        raise ValueError('no valid LwVM header at LBA 0')
    media_size = struct.unpack_from('<Q', hdr, 0x20)[0]
    if media_size != NLBA * 4096:
        raise ValueError('unsupported LwVM media size: %d (expected %d)' %
                         (media_size, NLBA * 4096))
    count = struct.unpack_from('<I', hdr, 0x28)[0]
    if not 1 <= count <= 12:
        raise ValueError('invalid LwVM partition count')
    for i in range(count):
        o = 0x200 + i * 0x80
        if hdr[o + 56:o + 0x80].decode('utf-16le').rstrip('\0') == name:
            chunks = struct.unpack_from('<1024H', hdr, 0x800)
            phys = {}
            for p, c in enumerate(chunks):
                if c >> 12 != i:
                    continue
                logical = c & 0xfff
                if logical >= 1024 or logical in phys:
                    raise ValueError('invalid or duplicate LwVM chunk')
                if p * LWVM_CHUNK >= NLBA * 4096:
                    raise ValueError('LwVM chunk outside disk0')
                phys[logical] = p
            if not phys or sorted(phys) != list(range(len(phys))):
                raise ValueError('missing LwVM partition chunks')
            begin, end = struct.unpack_from('<QQ', hdr, o + 32)
            size = end - begin
            if size <= 0 or end > media_size or size > len(phys) * LWVM_CHUNK:
                raise ValueError('invalid LwVM partition size')
            return [phys[c] for c in range(len(phys))], size
    raise ValueError('no LwVM partition %r' % name)


def lwvm_part(disk, name):
    return lwvm_info(disk, name)[0]


def checked_range(start, n):
    if start < 0 or n < 0 or start > NLBA or n > NLBA - start:
        raise ValueError('requested range outside disk0')


def number(value):
    return int(value, 0)


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest='cmd', required=True)
    for cmd in ('scan', 'read', 'image', 'part'):
        p = sub.add_parser(cmd)
        p.add_argument('directory')
        p.add_argument('mapfile')
        if cmd in ('image', 'part'):
            p.add_argument('output')
        if cmd in ('read', 'image'):
            p.add_argument('start', type=number)
            p.add_argument('count', type=number, **({'nargs': '?', 'default': 1} if cmd == 'read' else {}))
        if cmd == 'part':
            p.add_argument('name')
    args = parser.parse_args()
    if args.cmd == 'scan':
        scan(args.directory, args.mapfile)
        return
    if args.cmd in ('read', 'image'):
        checked_range(args.start, args.count)
    with Disk(args.directory, args.mapfile) as disk:
        if args.cmd == 'read':
            for i in range(args.count):
                sys.stdout.buffer.write(disk.read4k(args.start + i) or b'\0' * 4096)
            return
        if args.cmd == 'image':
            ranges = [(args.start, args.count * 4096)]
        else:
            chunks, size = lwvm_info(disk, args.name)
            # A partition need not fill its final allocation chunk.  Emit
            # exactly end-begin bytes, as iosnand's NBD export does.
            ranges = []
            for p in chunks:
                n = min(size, LWVM_CHUNK)
                ranges.append((p * (LWVM_CHUNK // 4096), n))
                size -= n
                if not size:
                    break
        missing = 0
        for start, n in ranges:
            checked_range(start, (n + 4095) // 4096)
        with new_output(args.output) as out:
            for start, n in ranges:
                for i in range((n + 4095) // 4096):
                    b = disk.read4k(start + i)
                    if b is None:
                        missing += 1
                        b = b'\0' * 4096
                    out.write(b[:min(4096, n - i * 4096)])
        print('%d unmapped 4 KB blocks (zeros)' % missing, file=sys.stderr)


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, struct.error) as exc:
        raise SystemExit('nand-ftl-scan: %s' % exc)
