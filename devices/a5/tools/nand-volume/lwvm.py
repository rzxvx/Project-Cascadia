#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""
LwVM headers of the iPad mini 1 NAND (spec section 6): read, plan a Linux
volume, plan its removal, check with iOS's own validator.

  lwvm.py show HEADER
  lwvm.py create HEADER OUT --size 8G [--uuid HEX32]   two volumes -> three
  lwvm.py remove HEADER OUT [--saved TWO_VOLUME_HEADER] three -> two
  lwvm.py validate HEADER                            iOS's CRC + validator
  lwvm.py selftest FIXTURES_DIR                      spec 8.1 cases

HEADER files are the 4096 bytes of LBA 0.  Nothing here touches a device.
"""
import argparse
import os
import struct
import sys
import uuid
import zlib

BLOCK = 4096
CHUNK = 16 << 20
MEDIA = 16000000000
NCHUNKS = MEDIA // CHUNK                      # 953 on the media
LOW = 0x4000 + 2 * BLOCK                      # lowest partition start
BOUND = LOW + (NCHUNKS - 8) * CHUNK           # 15854493696, highest end
MAGIC = bytes.fromhex('6a9088cf8afd630ae351e24887e0b98b')
HFS_TYPE = bytes.fromhex('48465300000011aaaa1100306543ecac')
LINUX_TYPE = bytes.fromhex('0fc63daf848347728e793d69d8477de4')
M_HEADER, M_FREE, M_PAST = 0xF000, 0xF111, 0xF3FF
ENTRIES, MAP = 0x200, 0x800
SYSTEM, DATA, LINUX = 0, 1, 2
MIN_HFS_BLOCKS = 524288                       # Data keeps at least 2 GiB


class LwvmError(Exception):
    pass


def ceil_chunks(n):
    return (n + CHUNK - 1) // CHUNK


class Part:
    def __init__(self, raw):
        self.type = raw[0:16]
        self.uuid = raw[16:32]
        self.start, self.end, self.attr = struct.unpack_from('<QQQ', raw, 32)
        self.name = raw[56:128].decode('utf-16le', 'replace').split('\0')[0]

    def pack(self):
        name = self.name.encode('utf-16le')[:72]
        return (self.type + self.uuid + struct.pack('<QQQ', self.start, self.end, self.attr)
                + name + bytes(72 - len(name)))

    def chunks(self):
        return ceil_chunks(self.end - self.start)


class Header:
    def __init__(self, raw):
        if len(raw) != BLOCK:
            raise LwvmError('a header is %d bytes, not %d' % (BLOCK, len(raw)))
        self.raw = bytes(raw)
        if raw[:16] != MAGIC:
            raise LwvmError('no LwVM magic')
        self.media_guid = raw[16:32]
        self.media_size, self.count, self.crc = struct.unpack_from('<QII', raw, 0x20)
        if self.media_size != MEDIA:
            raise LwvmError('media size %d, expected %d' % (self.media_size, MEDIA))
        if self.count > 8:
            raise LwvmError('%d partitions' % self.count)
        self.parts = [Part(raw[ENTRIES + 128 * i:ENTRIES + 128 * (i + 1)]) for i in range(self.count)]
        self.map = list(struct.unpack_from('<1024H', raw, MAP))

    def crc_ok(self):
        b = bytearray(self.raw)
        struct.pack_into('<I', b, 0x2c, 0)
        return zlib.crc32(b) & 0xffffffff == self.crc

    def pack(self):
        b = bytearray(self.raw)           # keeps the bytes the format leaves alone
        struct.pack_into('<QII', b, 0x20, self.media_size, len(self.parts), 0)
        for i in range(8):
            off = ENTRIES + 128 * i
            b[off:off + 128] = self.parts[i].pack() if i < len(self.parts) else bytes(128)
        struct.pack_into('<1024H', b, MAP, *self.map)
        struct.pack_into('<I', b, 0x2c, zlib.crc32(b) & 0xffffffff)
        return bytes(b)

    def chunks_of(self, p):
        """Physical chunks of partition p, as {index: physical}."""
        out = {}
        for c, v in enumerate(self.map):
            if v < 0xF000 and v >> 12 == p:
                if (v & 0xfff) in out:
                    raise LwvmError('partition %d index %d on two chunks' % (p, v & 0xfff))
                out[v & 0xfff] = c
        return out

    def free_chunks(self):
        return [c for c in range(1, NCHUNKS) if self.map[c] == M_FREE]


def check_shape(h):
    """The rules of spec 6.2, in Python, so a plan is refused before the emulator runs."""
    if not h.crc_ok():
        raise LwvmError('CRC mismatch')
    for p, part in enumerate(h.parts):
        if part.type == bytes(16) or part.uuid == bytes(16):
            raise LwvmError('partition %d: zero type or identity' % p)
        if part.start < LOW or part.end > BOUND or part.start >= part.end:
            raise LwvmError('partition %d: range %d..%d' % (p, part.start, part.end))
        if part.start % BLOCK or part.end % BLOCK:
            raise LwvmError('partition %d: not block aligned' % p)
        for q in range(p):
            o = h.parts[q]
            if part.start < o.end and o.start < part.end:
                raise LwvmError('partition %d overlaps %d' % (p, q))
        have = h.chunks_of(p)
        if sorted(have) != list(range(part.chunks())):
            raise LwvmError('partition %d: chunk indices do not cover 0..%d exactly'
                            % (p, part.chunks() - 1))
    if h.map[0] != M_HEADER:
        raise LwvmError('chunk 0 not the header')
    for c in range(1, NCHUNKS):
        v = h.map[c]
        if v != M_FREE and (v >= 0xF000 or v >> 12 >= len(h.parts)):
            raise LwvmError('chunk %d: %#x' % (c, v))
    for c in range(NCHUNKS, 1024):
        if h.map[c] != M_PAST:
            raise LwvmError('chunk %d past the media is %#x' % (c, h.map[c]))
    uu = [p.uuid for p in h.parts]
    if len(set(uu)) != len(uu):
        raise LwvmError('two partitions share a unique GUID')


def ios_check(raw, fixcrc=False):
    """(CRC check, validator, log lines) from iOS's code, both results signed."""
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import lwvm_validate
    c, v, msgs = lwvm_validate.check(raw, fixcrc=fixcrc)
    signed = lambda x: x - (1 << 32) if x >= 0x80000000 else x
    return signed(c), signed(v), msgs


def check_ios(h, raw):
    """iOS's own CRC check and validator (spec 6.6)."""
    c, v, msgs = ios_check(raw)
    if c != 0 or v != 0:
        raise LwvmError('iOS refuses it: CRC check %d, validator %d %s' % (c, v, '; '.join(msgs)))


def expect_stock_pair(h):
    if len(h.parts) < 2 or h.parts[SYSTEM].name != 'System' or h.parts[DATA].name != 'Data':
        raise LwvmError('partitions are not System, Data')
    if h.parts[DATA].type != HFS_TYPE:
        raise LwvmError('Data is not HFS')


def plan_create(h, size, new_uuid=None):
    """Spec 6.4 step 2: Data's end lowered, Linux at [Data_end, bound).

    From two volumes (Data ends at the bound), or as a grow of an existing Linux
    volume (Linux keeps its chunks for its old indices)."""
    check_shape(h)
    expect_stock_pair(h)
    data = h.parts[DATA]
    if len(h.parts) == 2:
        if data.end != BOUND:
            raise LwvmError('Data ends at %d, not at the bound' % data.end)
        old_linux = {}
    elif len(h.parts) == 3 and h.parts[LINUX].name == 'Linux' and h.parts[LINUX].end == BOUND \
            and h.parts[LINUX].start == data.end:
        old_linux = h.chunks_of(LINUX)
        if new_uuid is None:
            new_uuid = h.parts[LINUX].uuid
    else:
        raise LwvmError('unexpected partitions: %s' % ', '.join(p.name for p in h.parts))
    if size % BLOCK or size <= 0:
        raise LwvmError('size must be a positive multiple of %d' % BLOCK)
    data_end = BOUND - size
    if data_end >= data.end:
        raise LwvmError('Linux would not grow (Data ends at %d now)' % data.end)
    n_blocks = (data_end - data.start) // BLOCK
    if n_blocks < MIN_HFS_BLOCKS:
        raise LwvmError('Data would keep %d blocks, less than %d' % (n_blocks, MIN_HFS_BLOCKS))

    data_chunks = h.chunks_of(DATA)
    n_new = ceil_chunks(data_end - data.start)
    n_l = ceil_chunks(size)
    released = sorted(c for i, c in data_chunks.items() if i >= n_new)
    pool = released + h.free_chunks()
    need = n_l - len(old_linux)
    if need > len(pool):
        raise LwvmError('not enough chunks: need %d, have %d' % (need, len(pool)))
    m = list(h.map)
    for c in released:
        m[c] = M_FREE
    for k in range(need):
        m[pool[k]] = (LINUX << 12) | (len(old_linux) + k)

    out = Header(h.raw)
    out.map = m
    out.parts[DATA].end = data_end
    lin = Part(bytes(128))
    lin.type = LINUX_TYPE
    lin.uuid = new_uuid or uuid.uuid4().bytes
    lin.start, lin.end, lin.attr, lin.name = data_end, BOUND, 0, 'Linux'
    if len(out.parts) == 3:
        out.parts[LINUX] = lin
    else:
        out.parts.append(lin)
    raw = out.pack()
    out = Header(raw)
    check_shape(out)
    return out, raw, n_blocks


def plan_remove(h, saved=None):
    """Spec 6.5 step 2: back to System + Data, Data to the bound."""
    check_shape(h)
    expect_stock_pair(h)
    if len(h.parts) != 3 or h.parts[LINUX].name != 'Linux':
        raise LwvmError('no Linux volume to remove')
    data = h.parts[DATA]
    keep = {p: h.chunks_of(p) for p in (SYSTEM, DATA)}
    if saved is not None:
        s = Header(saved)
        check_shape(s)
        expect_stock_pair(s)
        if len(s.parts) != 2 or s.parts[DATA].end != BOUND:
            raise LwvmError('the saved header is not a two-volume header ending at the bound')
        same_but_end = lambda a, b: a.pack()[:40] + a.pack()[48:] == b.pack()[:40] + b.pack()[48:]
        if s.parts[SYSTEM].pack() != h.parts[SYSTEM].pack() or \
                not same_but_end(s.parts[DATA], h.parts[DATA]):
            raise LwvmError('the saved header has different System or Data entries')
        for p in (SYSTEM, DATA):
            have = s.chunks_of(p)
            for i, c in keep[p].items():
                if have.get(i) != c:
                    raise LwvmError('%s index %d is on chunk %d now, %s in the saved header'
                                    % (s.parts[p].name, i, c, have.get(i)))
        if s.media_guid != h.media_guid:
            raise LwvmError('the saved header has another media GUID')
        return s, bytes(saved)
    n_now = data.chunks()
    n_old = ceil_chunks(BOUND - data.start)
    pool = sorted(h.chunks_of(LINUX).values()) + h.free_chunks()
    m = list(h.map)
    for c in h.chunks_of(LINUX).values():
        m[c] = M_FREE
    if n_old - n_now > len(pool):
        raise LwvmError('not enough chunks to give Data back')
    for k, i in enumerate(range(n_now, n_old)):
        m[pool[k]] = (DATA << 12) | i
    out = Header(h.raw)
    out.map = m
    out.parts = out.parts[:2]
    out.parts[DATA].end = BOUND
    raw = out.pack()
    out = Header(raw)
    check_shape(out)
    return out, raw


def describe(h):
    lines = ['media GUID %s, %d partitions, CRC %08x (%s)'
             % (h.media_guid.hex(), len(h.parts), h.crc, 'ok' if h.crc_ok() else 'BAD')]
    for p, part in enumerate(h.parts):
        lines.append('  %d %-7s %12d .. %12d  %5d chunks  %8.2f GiB  attr %#x  uuid %s'
                     % (p, part.name, part.start, part.end, part.chunks(),
                        (part.end - part.start) / (1 << 30), part.attr, part.uuid.hex()))
    lines.append('  free chunks: %s' % (h.free_chunks() or 'none'))
    return '\n'.join(lines)


def parse_size(s):
    mul = {'K': 1 << 10, 'M': 1 << 20, 'G': 1 << 30}
    if s[-1:].upper() in mul:
        return int(float(s[:-1]) * mul[s[-1].upper()])
    return int(s)


def selftest(d):
    import hashlib
    rd = lambda n: open(os.path.join(d, n), 'rb').read()
    two, mid, now = rd('two-w0c.bin'), rd('three-96m.bin'), rd('three-8g.bin')
    sha = lambda b: hashlib.sha256(b).hexdigest()
    fails = 0

    def case(name, ok):
        nonlocal fails
        print('%-62s %s' % (name, 'ok' if ok else 'FAIL'))
        fails += not ok

    case('w0c header CRC 12c6adda, iOS accepts',
         Header(two).crc == 0x12c6adda and ios_check(two, fixcrc=False)[:2] == (0, 0))
    case("today's header CRC d32fc8ee, iOS accepts",
         Header(now).crc == 0xd32fc8ee and ios_check(now, fixcrc=False)[:2] == (0, 0))
    b = bytearray(now); struct.pack_into('<Q', b, ENTRIES + 128 * 2 + 40, BOUND + BLOCK)
    c, v, msgs = ios_check(bytes(b))
    case("Linux end + 4096: iOS refuses 'ends too high'", v == -1 and any('ends too high' in m for m in msgs))
    b = bytearray(now); b[0x900] ^= 1
    case('one byte changed, CRC stale: CRC check -1', ios_check(bytes(b), fixcrc=False)[0] == -1)
    case('all-zero block: CRC check -2', ios_check(bytes(BLOCK), fixcrc=False)[0] == -2)
    _, g, _ = plan_create(Header(mid), BOUND - 7264559104)
    case("grow of the 96 MiB header = today's header byte for byte", g == now)
    lin_uuid = Header(now).parts[LINUX].uuid
    _, one, n = plan_create(Header(two), BOUND - 7264559104, lin_uuid)
    h1 = Header(one)
    case('one-step create from w0c: CRC f45e7c76, SHA-256 afd27ea1',
         h1.crc == 0xf45e7c76 and sha(one).startswith('afd27ea1') and n == 1230568)
    case('  ... iOS accepts', ios_check(one, fixcrc=False)[:2] == (0, 0))
    case('  ... 947-952 still free', h1.free_chunks() == list(range(947, 953)))
    _, back = plan_remove(Header(now), two)
    case("remove today's Linux by restoring w0c gives w0c", back == two)
    _, inv = plan_remove(h1)
    hi, hw = Header(inv), Header(two)
    case('remove by the inverse rule after a create: iOS accepts, entries as w0c',
         ios_check(inv)[:2] == (0, 0) and inv[ENTRIES:MAP] == two[ENTRIES:MAP]
         and hi.chunks_of(SYSTEM) == hw.chunks_of(SYSTEM)
         and all(hi.chunks_of(DATA)[i] == c for i, c in hw.chunks_of(DATA).items() if i < 301)
         and hi.free_chunks() == hw.free_chunks())
    _, inv2 = plan_remove(Header(now))
    case("remove today's by the inverse rule: iOS accepts",
         ios_check(inv2, fixcrc=False)[:2] == (0, 0))
    for size in (1 << 30, 4 << 30, 8 << 30, 10 << 30, (1 << 30) + BLOCK):
        _, r, _ = plan_create(Header(two), size)
        case('create %d bytes from w0c: iOS accepts' % size,
             ios_check(r, fixcrc=False)[:2] == (0, 0))
    for bad in (0, BLOCK - 1, BOUND - LOW - BLOCK * (MIN_HFS_BLOCKS - 1)):
        try:
            plan_create(Header(two), bad)
            case('create %d refused' % bad, False)
        except LwvmError:
            case('create %d refused' % bad, True)
    try:
        plan_create(Header(now), 1 << 30)
        case('create on a header with a bigger Linux refused', False)
    except LwvmError:
        case('create on a header with a bigger Linux refused', True)
    try:
        plan_remove(Header(two))
        case('remove on two volumes refused', False)
    except LwvmError:
        case('remove on two volumes refused', True)
    print('%d failed' % fails)
    return 1 if fails else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd', required=True)
    s = sub.add_parser('show'); s.add_argument('header')
    s = sub.add_parser('validate'); s.add_argument('header')
    s = sub.add_parser('create'); s.add_argument('header'); s.add_argument('out')
    s.add_argument('--size', required=True); s.add_argument('--uuid')
    s = sub.add_parser('remove'); s.add_argument('header'); s.add_argument('out')
    s.add_argument('--saved')
    s = sub.add_parser('selftest'); s.add_argument('fixtures')
    a = ap.parse_args()
    try:
        if a.cmd == 'selftest':
            sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
            return selftest(a.fixtures)
        raw = open(a.header, 'rb').read()
        h = Header(raw)
        if a.cmd == 'show':
            print(describe(h))
        elif a.cmd == 'validate':
            check_shape(h)
            check_ios(h, raw)
            print('accepted by iOS: CRC 0, validator 0')
        elif a.cmd == 'create':
            out, r, n = plan_create(h, parse_size(a.size), bytes.fromhex(a.uuid) if a.uuid else None)
            check_ios(out, r)
            open(a.out, 'wb').write(r)
            print(describe(out))
            print('Data HFS target: %d blocks (%d bytes); iOS accepts the header' % (n, n * BLOCK))
        elif a.cmd == 'remove':
            out, r = plan_remove(h, open(a.saved, 'rb').read() if a.saved else None)
            check_ios(out, r)
            open(a.out, 'wb').write(r)
            print(describe(out))
            print('Data HFS grows afterwards to %d blocks; iOS accepts the header'
                  % ((out.parts[DATA].end - out.parts[DATA].start) // BLOCK))
    except (LwvmError, OSError) as e:
        print('lwvm: %s' % e, file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
