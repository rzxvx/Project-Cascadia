#!/usr/bin/env python3
"""Which VSVFL reserved-pool slots of one CE Linux may own -- offline, from dumped context pages.

    k93-pool-plan.py [--special B,B,...] [--window0 48-87] [--window1 48-63] CTX.bin [CTX.bin ...]

Each CTX.bin is one decrypted VSVFL context page of the same CE (at least its first
2048 bytes), e.g. `k93nand read BUS CE BLOCK:PAGE -k meta -O CTX.bin` of the pages
`k93iosnand vfl` names, and as many older copies (other sets, other context
blocks) as you can get: a slot only ever goes free -> taken between two formats,
so a slot counts as free only if EVERY valid copy says 0xFFF0.  --special lists
the CE's special blocks (from `k93nand sig` / the DEVICEINFOBBT header); each must
sit in a slot marked 0xFFFF, or the model of docs/research/k93-nand-write.md
(section 5) does not hold for this iPad and nothing should be written.

Nothing here touches the iPad.  Geometry is the K93's: 4096 blocks/CE, 2 banks,
vendor type 0x150011 (slot bank*nres + i is block 2*(pool + i) + bank).
"""
import struct
import sys

BLOCKS, BANKS = 4096, 2
FREE = 0xfff0
NAMES = {0xfff0: 'free', 0xffff: 'dead/special', 0xfffe: 'BORROWED', 0xfffd: 'failed-borrow'}


def parse(path):
    d = open(path, 'rb').read()[:2048]
    if len(d) < 2048:
        return None, 'short file'
    w = struct.unpack_from('<512I', d)
    s, x = sum(w[:510]) & 0xffffffff, 0
    for v in w[:510]:
        x ^= v
    if w[510] != (s + 0xaabbccdd) & 0xffffffff or w[511] != x ^ 0xaabbccdd:
        return None, 'bad checksum'
    c = {
        'usn_inc': w[0], 'usn_dec': w[1], 'ftl': w[2],
        'cur': struct.unpack_from('<HH', d, 0xc),
        'next_free': struct.unpack_from('<%dH' % BANKS, d, 0x16),
        'ctxblk': struct.unpack_from('<4H', d, 0x68e),
        'usable': struct.unpack_from('<H', d, 0x696)[0],
        'pool': struct.unpack_from('<H', d, 0x698)[0],
        'scrub': struct.unpack_from('<20H', d, 0x6a2)[:struct.unpack_from('<H', d, 0x6a0)[0]],
    }
    if c['ftl'] != 2 or not 0 < c['pool'] <= BLOCKS // BANKS:
        return None, 'not a VSVFL/YAFTL context (ftl %d, pool %d)' % (c['ftl'], c['pool'])
    nres = BLOCKS // BANKS - c['pool']
    c['nres'] = nres
    c['map'] = struct.unpack_from('<%dH' % (nres * BANKS), d, 0x26)
    return c, None


def rng(s):
    a, b = s.split('-')
    return range(int(a, 0), int(b, 0) + 1)


def main(argv):
    special, win = [], {0: rng('48-87'), 1: rng('48-63')}
    files = []
    i = 0
    while i < len(argv):
        if argv[i] == '--special':
            special = [int(v, 0) for v in argv[i + 1].split(',') if v]
            i += 2
        elif argv[i] in ('--window0', '--window1'):
            win[int(argv[i][-1])] = rng(argv[i + 1])
            i += 2
        else:
            files.append(argv[i])
            i += 1
    if not files:
        sys.exit(__doc__)
    ctxs = []
    for f in files:
        c, err = parse(f)
        print('%-40s %s' % (f, err or 'usn_dec 0x%08x usn_inc %u, context block #%d page %d'
                            % (c['usn_dec'], c['usn_inc'], c['cur'][0], c['cur'][1])))
        if c:
            ctxs.append(c)
    if not ctxs:
        sys.exit('no valid context')
    if len({(c['pool'], c['nres']) for c in ctxs}) != 1:
        sys.exit('copies disagree on the pool geometry: stop')
    cur = min(ctxs, key=lambda c: c['usn_dec'])      # each write decrements usn_dec
    pool, nres = cur['pool'], cur['nres']
    taken = [any(c['map'][k] != FREE for c in ctxs) for k in range(nres * BANKS)]
    blk = lambda k: 2 * (pool + k % nres) + k // nres
    print('\nnewest copy: usn_dec 0x%08x; pool from vblock %d, %d slots per bank; context blocks %s; '
          'scrub list %s' % (cur['usn_dec'], pool, nres, list(cur['ctxblk']), list(cur['scrub'])))
    stop = False
    for b in special:
        k = [k for k in range(nres * BANKS) if blk(k) == b]
        if not k:
            print('special block %d is outside the pool' % b)
            continue
        v = cur['map'][k[0]]
        ok = v == 0xffff
        print('special block %d: slot %d = %#06x %s' % (b, k[0], v, 'ok' if ok else '<- NOT 0xFFFF: STOP'))
        stop |= not ok
    usable = []
    for bank in range(BANKS):
        sl = range(bank * nres, (bank + 1) * nres)
        nf = cur['next_free'][bank]
        used = [k - bank * nres for k in sl if cur['map'][k] < BLOCKS]
        top = [k - bank * nres for k in sl if cur['map'][k] >= 0xfff1]
        print('\nbank %d: next_free %d, replacements in slots %s, dead/borrowed slots %s'
              % (bank, nf, '%d-%d' % (min(used), max(used)) if used else '-',
                 ', '.join('%d=%s' % (i, NAMES.get(cur['map'][bank * nres + i], '?')) for i in top) or '-'))
        w = [i for i in win[bank] if i < nres]
        lo = [i for i in range(0, min(w) if w else 0) if not taken[bank * nres + i]]
        print('  window slots %d-%d (blocks %d-%d step 2); free slots left below it for iOS: %d'
              % (w[0], w[-1], blk(bank * nres + w[0]), blk(bank * nres + w[-1]), len(lo)) if w else '  empty window')
        if used and w and max(used) >= min(w):
            print('  replacements already reach the window: STOP')
            stop = True
        for i in w:
            k = bank * nres + i
            if taken[k] or blk(k) in special or blk(k) in cur['ctxblk'] or blk(k) in cur['scrub']:
                print('  skip slot %d (block %d): %s' % (i, blk(k), NAMES.get(cur['map'][k], 'replaces %d' % cur['map'][k])
                                                         if cur['map'][k] != FREE else 'taken in an older copy'))
                continue
            usable.append(blk(k))
    if stop:
        print('\nSTOP: the device does not match the model; write nothing')
        return 1
    print('\nLinux may claim (after the erased check of the doc, 5.5 item 4): %d blocks' % len(usable))
    print(' '.join(str(b) for b in sorted(usable)))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
