#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""
Before the HFS grow of a volume removal (spec 6.5 step 3): Data's live fsck may
exit 8, and that is accepted only when

  - its findings are exactly: invalid free block count (should be F' instead of F),
    volume bitmap minor repair for orphaned blocks / under-allocation, volume header
    minor repair;
  - F' = F + (S - N) for some S with N <= S <= the container's size;
  - no bit of the allocation bitmap is set at or past N.

  hfs_undo_check.py extents HFS_BLOCK0            allocation file extents: "start count"
  hfs_undo_check.py check FSCK HFS_BLOCK0 BITMAP CONTAINER_BLOCKS

HFS_BLOCK0 is the first 4096 bytes of the partition, BITMAP the allocation file's
blocks in extent order, FSCK the text of `fsck_hfs -n -l`.  Exit 0 = accepted.
"""
import re
import struct
import sys

ALLOWED = [
    r'Volume bitmap needs minor repair for orphaned blocks',
    r'Volume bitmap needs minor repair for under-allocation',
    r'Invalid volume free block count',
    r'\(It should be \d+ instead of \d+\)',
    r'Volume header needs minor repair',
    r'The volume Data was found corrupt and needs to be repaired\.',
    r'ERROR:\s+output redirected',      # never accepted: see below
]
FAULT = re.compile(r'(?i)(needs|invalid|should be|corrupt|error|incorrect|missing|overlap|illegal|bad )')


def header(block0):
    h = block0[1024:1536]
    if h[:2] not in (b'H+', b'HX'):
        raise SystemExit('no HFS signature')
    total, free = struct.unpack_from('>II', h, 44)
    bs = struct.unpack_from('>I', h, 40)[0]
    size = struct.unpack_from('>Q', h, 0x70)[0]
    ext = [struct.unpack_from('>II', h, 0x80 + 8 * i) for i in range(8)]
    return bs, total, free, size, [e for e in ext if e[1]]


def check(fsck, block0, bitmap, container):
    bs, n, free, size, ext = header(block0)
    if bs != 4096:
        return 'block size %d' % bs
    if 'output redirected to target volume' in fsck:
        return 'fsck output went to Data itself: rerun with output to the computer'
    # the header dump fsck prints is not a finding
    fsck = '\n'.join(l for l in fsck.splitlines()
                      if not re.match(r'\s*([0-9a-f]{4}:|Offset \d+ length \d+:|\. \. \.|\(\d+, \d+\)$)', l))
    pairs = set(re.findall(r'\(It should be (\d+) instead of (\d+)\)', fsck))
    rest = fsck
    for pat in ALLOWED[:-1]:
        rest = re.sub(pat, '', rest)
    m = FAULT.search(rest)
    if m:
        line = rest[max(0, rest.rfind('\n', 0, m.start()) + 1):rest.find('\n', m.end())]
        return 'fsck reports something else: %r' % line.strip()
    if len(pairs) != 1:
        return 'expected one free-count finding, got %s' % sorted(pairs)
    fp, f = map(int, pairs.pop())
    s = n + (fp - f)
    if not n <= s <= container:
        return "F' - F = %d does not fit a size between N=%d and the container %d" % (fp - f, n, container)
    covered = min(len(bitmap) * 8, size * 8)
    for i in range(n, covered):
        if (bitmap[i >> 3] >> (7 - (i & 7))) & 1:
            return 'bitmap bit %d (at or past N=%d) is set' % (i, n)
    return 'ok: F=%d F\'=%d S=%d N=%d container=%d; no bitmap bit at or past N' % (f, fp, s, n, container)


def main():
    if len(sys.argv) == 3 and sys.argv[1] == 'extents':
        for start, count in header(open(sys.argv[2], 'rb').read(4096))[4]:
            print(start, count)
        return 0
    if len(sys.argv) == 6 and sys.argv[1] == 'check':
        fsck = open(sys.argv[2], errors='replace').read()
        r = check(fsck, open(sys.argv[3], 'rb').read(4096), open(sys.argv[4], 'rb').read(),
                  int(sys.argv[5]))
        print(r)
        return 0 if r.startswith('ok') else 1
    print(__doc__)
    return 2


if __name__ == '__main__':
    sys.exit(main())
