#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0+ OR MIT
# Copyright (c) 2026 kiki <kieran@lavato.net>
#
# Turn the touch controller firmware from the IPSW into the op list
# apple-mt-spi replays.
#
# Firmware/D331p_Multitouch.im4p is not encrypted. Its payload is an IOKit XML
# plist, one array per personality, and each entry is something for the host
# to do over HBPP: write a blob, read-modify-write a register, poll one. This
# keeps the entries for one personality, in order, and drops the rest:
#
#   Binary, Property   write the payload to Address. A Property's payload is
#                      the default for a syscfg key, which is all this uses
#   ReadModifyWrite    Address, Mask, Value, SkipCheck
#   Poll               Address, Mask, Value, TimeoutMs, ReadOnError
#   Metadata, Config   not for the boot
#
# Output, little endian: "MTFW", version 1, op count, then per op a u16 type,
# u16 flags and u32 length of what follows, padded to 4 bytes:
#
#   1 write   addr, payload
#   2 rmw     addr, mask, value; flag 1 means skip the acknowledge check
#   3 poll    addr, mask, value, timeout ms, read-on-error addr or ~0
#
# usage: mtfw.py <payload.plist> <out.bin>
# The payload comes out of the im4p with: ipsw img4 im4p extract

import base64
import re
import struct
import sys

OP_WRITE, OP_RMW, OP_POLL = 1, 2, 3


def parse_plist(text):
    """IOKit's XML, which has ID and IDREF and so is not plistlib's."""
    tokens = re.finditer(r'<(/?)(\w+)([^>]*?)(/?)>|([^<]+)', text)
    tokens = [t for t in tokens if t.group(5) is None or t.group(5).strip()]
    ids = {}
    pos = 0

    def attr(attrs, name):
        m = re.search(name + r'="([^"]*)"', attrs)
        return m and m.group(1)

    def value():
        nonlocal pos
        close, tag, attrs, empty, text = tokens[pos].groups()
        pos += 1
        ref = attr(attrs, 'IDREF')
        if ref is not None:
            return ids[ref]
        if empty:
            v = {'true': True, 'false': False, 'array': [], 'dict': {}}.get(tag, '')
        elif tag == 'dict':
            v = {}
            while tokens[pos].group(1) != '/':
                key = tokens[pos + 1].group(5) if tokens[pos + 1].group(5) else ''
                pos += 3 if key else 2
                v[key] = value()
            pos += 1
        elif tag == 'array':
            v = []
            while tokens[pos].group(1) != '/':
                v.append(value())
            pos += 1
        else:
            body = ''
            if tokens[pos].group(5) is not None:
                body = tokens[pos].group(5)
                pos += 1
            pos += 1
            if tag == 'integer':
                v = int(body, 0)
            elif tag == 'data':
                v = base64.b64decode(body)
            else:
                v = body
        if attr(attrs, 'ID') is not None:
            ids[attr(attrs, 'ID')] = v
        return v

    return value()


def op(kind, flags, body):
    body += b'\0' * (-len(body) % 4)
    return struct.pack('<HHI', kind, flags, len(body)) + body


def convert(entries):
    ops = []
    for e in entries:
        kind = e.get('Type')
        if kind in ('Binary', 'Property'):
            ops.append(op(OP_WRITE, 0, struct.pack('<I', e['Address']) + e['Payload']))
        elif kind == 'ReadModifyWrite':
            ops.append(op(OP_RMW, int(bool(e.get('SkipCheck'))),
                          struct.pack('<III', e['Address'], e['Mask'], e['Value'])))
        elif kind == 'Poll':
            ops.append(op(OP_POLL, 0,
                          struct.pack('<IIIII', e['Address'], e['Mask'], e['Value'],
                                      e['TimeoutMs'], e.get('ReadOnError', 0xffffffff))))
        elif kind not in ('Metadata', 'Config'):
            sys.exit(f'unknown entry type {kind!r}')
    return b'MTFW' + struct.pack('<II', 1, len(ops)) + b''.join(ops)


def count_ops(blob):
    assert blob[:4] == b'MTFW'
    version, count = struct.unpack_from('<II', blob, 4)
    pos, seen = 12, 0
    while pos < len(blob):
        _, _, size = struct.unpack_from('<HHI', blob, pos)
        pos += 8 + size
        seen += 1
    assert version == 1 and pos == len(blob) and seen == count
    return count


def main():
    if len(sys.argv) != 3:
        sys.exit('usage: mtfw.py <payload.plist> <out.bin>')
    personality = 'C1R1FC1,1'
    firmware = parse_plist(open(sys.argv[1], encoding='utf-8').read())
    if personality not in firmware:
        sys.exit(f'no {personality} in this firmware, it has: {", ".join(firmware)}')
    blob = convert(firmware[personality])
    open(sys.argv[2], 'wb').write(blob)
    print(f'{sys.argv[2]}: {count_ops(blob)} ops, {len(blob)} bytes')


if __name__ == '__main__':
    main()
