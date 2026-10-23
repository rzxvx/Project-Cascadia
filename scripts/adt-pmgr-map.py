#!/usr/bin/env python3
"""adt-pmgr-map.py -- every PMGR register on this SoC, named, out of the ADT.

    python3 scripts/adt-pmgr-map.py [DeviceTree.raw] [a peek dump of the PMGR]

The pmgr node carries a `device-clocks` property: 116 entries of 44 bytes, one
per clock and per gateable device, and each one says where its registers are.

    +0x00  4     byte 3 is the clock id -- the number device nodes use in
                 their `clock-gates` property -- bytes 0..2 are flags
    +0x04  u32   flags
    +0x08  4     up to four parent clock ids, one per byte
    +0x0c  u32   index of the power-state register: PMGR + 0x1000 + idx * 4
    +0x10   4    packed clock codes; direct code index uses PMGR + 0x38 + idx * 4
    +0x14   8    two additional words (not decoded here)
    +0x1c  16    name

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

import argparse
import re
import struct
import sys

PMGR = 0x3f100000
ENTRY = 44


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
    with open(path) as dump:
        for line in dump:
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


def device_clock_entries(data, entry_size=ENTRY):
    """Decode the P105 12H321 44-byte ABI; 36-byte format is explicit only."""
    if entry_size not in (36, 44):
        raise ValueError("device-clock entry size must be 36 or 44")
    if not data or len(data) % entry_size:
        raise ValueError("device-clocks is empty or contains a truncated entry")
    if len(data) // entry_size > 4096:
        raise ValueError("too many device-clock entries")
    entries = []
    for offset in range(0, len(data), entry_size):
        entry = data[offset:offset + entry_size]
        words = struct.unpack_from('<5I', entry)
        name = entry[-16:].split(b'\0', 1)[0].decode('ascii')
        entries.append((entry[3], words, name))
    return entries


def clock_registers(packed, entry_size=ENTRY):
    """Direct P105 44-byte clock path; encoded/older formats stay explicit."""
    if entry_size not in (36, 44) or not 0 <= packed <= 0xffffffff:
        raise ValueError("invalid clock word or device-clock ABI")
    if entry_size == 36:
        return (), 'unverified' if packed else None
    codes = packed.to_bytes(4, 'little')
    # Original startup checks every code, even after an earlier terminator.
    if any((code & 0x3c) >= 0x34 for code in codes):
        return (), 'invalid'
    addresses = []
    for code in codes:
        index = code & 0x3f
        if not index:
            break
        if code & 0xc0:
            return tuple(addresses), 'encoded:%02x' % code
        addresses.append(PMGR + 0x38 + 4 * index)
    return tuple(addresses), None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('dt', nargs='?', default='ibootfiles/DeviceTree.raw')
    parser.add_argument('peek', nargs='?')
    parser.add_argument('--entry-size', type=int, choices=(36, 44), default=44,
                        help='P105 12H321 uses 44; 36 requires explicit older-ABI selection')
    args = parser.parse_args()
    live = peek_dump(args.peek) if args.peek else {}
    with open(args.dt, 'rb') as source:
        nodes = adt_nodes(source.read())
    pmgr = nodes.get('device-tree/arm-io/pmgr')
    if not pmgr or 'device-clocks' not in pmgr:
        sys.exit('no device-clocks in the pmgr node of %s' % args.dt)
    try:
        entries = device_clock_entries(pmgr['device-clocks'], args.entry_size)
    except ValueError as error:
        sys.exit(str(error))

    print('%-14s %3s  %-10s %-10s  %s' % ('name', 'id', 'gate', 'clock',
                                          'live' if live else ''))
    for cid, w, name in entries:
        gate = PMGR + 0x1000 + w[3] * 4 if 0 < w[3] < 0x400 else None
        clocks, clock_annotation = clock_registers(w[4], args.entry_size)
        clock = clocks[0] if clocks else None
        clock_text = ','.join('%08x' % address for address in clocks)
        if clock_annotation:
            clock_text += ('/' if clock_text else '') + clock_annotation
        clock_text = clock_text or '-'
        gate_text = '%08x' % gate if gate else '-'
        if args.entry_size == 44 and clock_annotation == 'invalid':
            gate = None
            gate_text = 'invalid-row'
        note = ''
        if live and gate:
            note = state(live.get(gate))
        elif live and clock:
            v = live.get(clock)
            note = ('= %s' % v) if v and v != '00000000' else 'reads zero'
        if args.entry_size == 36 and gate:
            note += ('; ' if note else '') + '36-byte gate address unverified'
        print('%-14s %3d  %-10s %-10s  %s' % (
            name, cid,
            gate_text,
            clock_text,
            note))


if __name__ == '__main__':
    main()
