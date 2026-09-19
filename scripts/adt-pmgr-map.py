#!/usr/bin/env python3
"""adt-pmgr-map.py -- every PMGR register on this SoC, named, out of the ADT.

    python3 scripts/adt-pmgr-map.py [DeviceTree.raw] [a peek dump of the PMGR]

The pmgr node carries a `device-clocks` property: 116 entries of 36 bytes, one
per clock and per gateable device, and each one says where its registers are.

    +0x00  4     byte 3 is the clock id -- the number device nodes use in
                 their `clock-gates` property -- bytes 0..2 are flags
    +0x04  u32   flags
    +0x08  4     up to four parent clock ids, one per byte
    +0x0c  u32   index of the power-state register: PMGR + 0x1000 + idx * 4
    +0x10  u32   index of the clock register:       PMGR + 0x0000 + idx * 4
    +0x14  16    name

PMGR is the pmgr node's first reg entry, 0x3f100000 here.

This is where the gate mapping comes from, and it settles a question that cost
this project three weeks: a device node's `clock-gates` id is NOT the register
index.  For the PWM the id is 83 and the register index is 73; for spi1, 68 and
58.  The kernelcache disassembly had been read as index = id, and every attempt
to power the touch stack wrote to a neighbour's register.  See
docs/research/p105-pmgr-gates.md; the brute-force sweep that found it first
agrees with this table register for register.

Given a second argument -- the output of `peek r 3f100000 7168`, which
tools/pmgr-map.sh leaves in logs/ -- each line also gets its live value, so the
state iBoot left the machine in reads out by name.
"""

import re
import struct
import sys

PMGR = 0x3f100000
ENTRY = 36


def adt_nodes(data):
    def parse(off, path):
        nprops, nchild = struct.unpack_from('<II', data, off)
        off += 8
        props = {}
        for _ in range(nprops):
            name = data[off:off + 32].split(b'\0')[0].decode('latin1')
            off += 32
            (ln,) = struct.unpack_from('<I', data, off)
            off += 4
            ln &= 0x7fffffff
            props[name] = data[off:off + ln]
            off += (ln + 3) & ~3
        nm = props.get('name', b'?').split(b'\0')[0].decode('latin1')
        p = path + '/' + nm if path else nm
        nodes = [(p, props)]
        for _ in range(nchild):
            off, sub = parse(off, p)
            nodes += sub
        return off, nodes
    return dict(parse(0, '')[1])


def peek_dump(path):
    """{address: 'xxxxxxxx'} out of `peek r` output"""
    words = {}
    for line in open(path):
        m = re.match(r'^([0-9a-f]{8}): ((?:[0-9a-f-]{8} ?)+)$', line.strip())
        if not m:
            continue
        base = int(m.group(1), 16)
        for i, w in enumerate(m.group(2).split()):
            words[base + 4 * i] = w
    return words


def state(v):
    if v is None:
        return ''
    if v == '--------':
        return 'unreadable'
    n = int(v, 16)
    if n == 0:
        return 'no register here'
    return '%s  target %x actual %x flags %x' % (
        'ON ' if (n & 0xf) == 0xf else 'off', n & 0xf, (n >> 4) & 0xf, n >> 8)


def main():
    dt = sys.argv[1] if len(sys.argv) > 1 else 'ibootfiles/DeviceTree.raw'
    live = peek_dump(sys.argv[2]) if len(sys.argv) > 2 else {}
    nodes = adt_nodes(open(dt, 'rb').read())
    pmgr = nodes.get('device-tree/arm-io/pmgr')
    if not pmgr or 'device-clocks' not in pmgr:
        sys.exit('no device-clocks in the pmgr node of %s' % dt)
    dc = pmgr['device-clocks']

    print('%-14s %3s  %-10s %-10s  %s' % ('name', 'id', 'gate', 'clock',
                                          'live' if live else ''))
    for k in range(len(dc) // ENTRY):
        e = dc[k * ENTRY:(k + 1) * ENTRY]
        w = struct.unpack('<5I', e[:20])
        name = e[20:36].split(b'\0')[0].decode('latin1', 'replace')
        cid = e[3]
        gate = PMGR + 0x1000 + w[3] * 4 if 0 < w[3] < 0x400 else None
        clock = PMGR + w[4] * 4 if 0 < w[4] < 0x400 else None
        note = ''
        if live and gate:
            note = state(live.get(gate))
        elif live and clock:
            v = live.get(clock)
            note = ('= %s' % v) if v and v != '00000000' else 'reads zero'
        print('%-14s %3d  %-10s %-10s  %s' % (
            name, cid,
            '%08x' % gate if gate else '-',
            '%08x' % clock if clock else '-',
            note))


if __name__ == '__main__':
    main()
