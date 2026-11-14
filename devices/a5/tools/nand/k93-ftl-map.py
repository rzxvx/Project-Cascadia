#!/usr/bin/env python3
"""k93-ftl-map.py -- YAFTL's LPN map, rebuilt from `k93nand scan` output.

    k93-ftl-map.py --cemap 0x0f0f --banks 2 scan-*.txt [-o map.tsv]

Each input is the stdout of `k93nand scan BUS CE FIRST N` (its "# FMIb CEc"
header says which CE it is).  Every page line carries the page's metadata after
unwhitening: LPN, the superblock's usn, byte 8 and the type byte.

The rule is iBEC's own (YAFTL_Open's restore, 0x9ff30616 / restoreBlock
0x9ff31c04, docs/research/k93-nand.md section 8): user superblocks newest usn
first, inside one the last-written page first, and the first sighting of an
LPN wins.  So the live copy of an LPN is max(usn, offset), with offset =
vpage * TB + vbank the position inside the superblock.  vbank needs the virtual
CE (h2fmiInitVirtToPhysMap: CE numbers walked in step over both buses, bus 0
first) and, with 2 banks per CE, which bank the block is: even blocks bank 0,
odd bank 1 (vendor types 0x100014, 0x120014, 0x140014, 0x150011).  A block the
VSVFL remapped can sit at the wrong parity; that only matters for the rare LPN
written twice into one superblock, and the count of such ties is printed.

Pages that are not user data (index 0x04, BTOC 0x08/0x0c, FTL context 0x20,
VSVFL context 0x80) are counted and listed by kind, since they are where the
next steps (the VSVFL context, the CX01 context) start.
"""

import argparse
import collections
import re
import sys

TYPES = {0x10: 'user', 0x04: 'index', 0x08: 'btoc', 0x0c: 'index-btoc',
         0x20: 'ftl-context', 0x80: 'vsvfl-context'}

HDR = re.compile(r'^# FMI(\d) CE(\d) ')
LINE = re.compile(r'^\s*(\d+)\s+(\d+)\s+0x([0-9a-f]+)\s+(\S+)\s+([0-9a-f]{20})\s+\|\s+(.*)$')


def vce_order(cemap):
    """(bus, ce) -> virtual CE number"""
    order = {}
    v = 0
    for k in range(8):
        for b in range(2):
            if cemap >> (b * 8 + k) & 1:
                order[(b, k)] = v
                v += 1
    return order


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--cemap', type=lambda s: int(s, 0), required=True,
                    help='the ce-bitmap `k93nand id` printed')
    ap.add_argument('--banks', type=int, default=2, help='VSVFL banks per CE (id prints it)')
    ap.add_argument('-o', '--out', help='write "lpn bus ce block page row usn" lines here')
    ap.add_argument('scans', nargs='+')
    a = ap.parse_args()

    order = vce_order(a.cemap)
    nce = len(order)
    tb = nce * a.banks
    best = {}                       # lpn -> (usn, off, bus, ce, blk, pg, row)
    kinds = collections.Counter()
    special = collections.defaultdict(list)
    usns = collections.defaultdict(set)     # usn -> {(bus, ce, blk)}
    ties = dupes = 0

    for name in a.scans:
        bus = ce = None
        with open(name) as f:
            for line in f:
                m = HDR.match(line)
                if m:
                    bus, ce = int(m.group(1)), int(m.group(2))
                    if (bus, ce) not in order:
                        sys.exit('%s: FMI%d CE%d is not in ce-bitmap 0x%04x' % (name, bus, ce, a.cemap))
                    continue
                m = LINE.match(line)
                if not m or bus is None:
                    if 'FAIL' in line:
                        kinds['read failed'] += 1
                    continue
                blk, pg, row, ecc = int(m.group(1)), int(m.group(2)), int(m.group(3), 16), m.group(4)
                rest = m.group(6).split()
                if rest[0] == 'erased':
                    kinds['erased'] += 1
                    continue
                if ecc not in ('ok',) and not ecc.startswith('ok:'):
                    kinds['ecc ' + ecc] += 1
                    continue
                lpn, usn, b8, typ = int(rest[0], 16), int(rest[1], 16), int(rest[2], 16), int(rest[3], 16)
                kind = TYPES.get(typ, 'type 0x%02x' % typ)
                kinds[kind] += 1
                if typ != 0x10:
                    special[kind].append((bus, ce, blk, pg, row, lpn, usn, b8))
                    continue
                usns[usn].add((bus, ce, blk))
                vbank = order[(bus, ce)] + (nce * (blk & 1) if a.banks == 2 else 0)
                off = pg * tb + vbank
                key = (usn, off, bus, ce, blk, pg, row)
                old = best.get(lpn)
                if old is not None:
                    dupes += 1
                    if old[0] == usn:
                        ties += 1
                if old is None or key[:2] > old[:2]:
                    best[lpn] = key

    print('pages by kind:')
    for k, n in kinds.most_common():
        print('  %-16s %d' % (k, n))
    print('user data: %d live LPNs, %d superseded copies (%d inside one superblock: ordered by '
          'the bank guess)' % (len(best), dupes, ties))
    print('superblocks seen (distinct usn): %d' % len(usns))
    for kind in sorted(special):
        rows = special[kind]
        print('%s pages: %d, first few:' % (kind, len(rows)))
        for r in rows[:8]:
            print('  FMI%d CE%d block %d page %d row 0x%06x  lpn/num 0x%08x usn 0x%08x b8 0x%02x' % r)
    if 0 in best:
        usn, off, bus, ce, blk, pg, row = best[0]
        print('LPN 0 (the LwVM header): FMI%d CE%d block %d page %d row 0x%06x usn 0x%08x' %
              (bus, ce, blk, pg, row, usn))
        print('  check: k93nand read %d %d 0x%06x -k user:0   (expect 6a 90 88 cf 8a fd 63 0a ...)' %
              (bus, ce, row))
    else:
        print('LPN 0 not in these scans')
    if a.out:
        with open(a.out, 'w') as f:
            for lpn in sorted(best):
                usn, off, bus, ce, blk, pg, row = best[lpn]
                f.write('%d\t%d\t%d\t%d\t%d\t0x%06x\t0x%08x\n' % (lpn, bus, ce, blk, pg, row, usn))
        print('map: %s' % a.out)


if __name__ == '__main__':
    main()
