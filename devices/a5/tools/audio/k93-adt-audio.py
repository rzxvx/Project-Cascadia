#!/usr/bin/env python3
"""The iPad 2's (K93) audio plumbing, decoded from its device tree.

    python3 tools/audio/k93-adt-audio.py [build/k93/firmware-12H321/DeviceTree.k93ap.img3.dec]

Prints, for the codec (i2c0/audio0, audio-control,cs42l63), the headset
switch, i2s0..i2s3 with their audio-data children, the i2s-switch and ae2:
every property, with the function-* ones split into phandle (and the node it
names), the platform-function fourcc and its arguments; dma-channels as
(channel, FIFO, transfer control, request line); and the audio-data `reg`
templates field by field (the 36-byte format descriptor AppleSamsungI2S and
AppleAE2MCA read: tools/audio/k93-i2s-emu.py).  Then the PMGR registers of
the audio blocks.  Reads the file only.
"""
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
DT = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, '..', '..', 'build', 'k93', 'firmware-12H321',
                                                        'DeviceTree.k93ap.img3.dec')


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
    return parse(0, '')[1]


nodes = adt_nodes(open(DT, 'rb').read())
by_phandle = {}
for path, props in nodes:
    if 'AAPL,phandle' in props:
        by_phandle[struct.unpack('<I', props['AAPL,phandle'][:4])[0]] = path


def fourcc_le(b):
    return b[::-1].decode('latin1')


def fmt_function(v):
    ph = struct.unpack_from('<I', v)[0]
    fn = fourcc_le(v[4:8])
    args = v[8:]
    words = [struct.unpack_from('<I', args, i)[0] for i in range(0, len(args) - len(args) % 4, 4)]
    s = 'phandle %#x (%s) %r' % (ph, by_phandle.get(ph, '?'), fn)
    if fn == 'GPIO' and len(words) >= 2:
        pin, fl = words[0], args[4:8]
        s += ' pin %#x' % pin
        if by_phandle.get(ph, '').endswith('/gpio'):
            s += ' (SoC GPIO %d)' % ((pin & 7) | (pin >> 5))
        s += ' flags %s (output %d, active-high %d, initial %d)' % (fl.hex(), fl[0], fl[1], fl[2])
    elif fn in ('i2sR',) and len(args) >= 11:
        s += ' %r -> %r, %s' % (fourcc_le(args[0:4]), fourcc_le(args[4:8]), args[8:].hex())
    else:
        s += ' args %s' % ' '.join('%#x' % w for w in words)
    return s


def fmt_template(r):
    if len(r) < 32:
        return r.hex()
    mclk, = struct.unpack_from('<I', r, 8)
    bpf, extra = struct.unpack_from('<HH', r, 0xc)
    num, den = struct.unpack_from('<HH', r, 0x10)
    tx, rx = struct.unpack_from('<II', r, 0x14)
    return ('[0] %#04x (bit 0 clock master, bit 4 must supply MCLK) [1] fmt %#04x [2] clock mode %d [3] %#04x '
            'slots %d slot bits %d (0 = sample bits) [6] %d mclk %d Hz bits/frame %d [0xe] %d '
            'rate %d/%d (filled at runtime) tx mask %#x rx mask %#x channels %d/%d bits %d/%d%s'
            % (r[0], r[1], r[2] & 3, r[3], r[4], r[5], r[6], mclk, bpf, extra, num, den, tx, rx,
               r[0x1c], r[0x1d], r[0x1e], r[0x1f], ' tail %s' % r[32:].hex() if len(r) > 32 else ''))


def show(path, props):
    print('[%s]' % path)
    for k in sorted(props):
        v = props[k]
        if k.startswith('function-') and len(v) >= 8:
            print('  %-28s %s' % (k, fmt_function(v)))
        elif k == 'dma-channels':
            for i in range(0, len(v), 16):
                ch, fifo, tc, req = struct.unpack_from('<4I', v, i)
                print('  %-28s ch %#x FIFO %#010x transfer %#010x (width %d, burst %d) request %d'
                      % (k if i == 0 else '', ch, fifo, tc, tc & 0xffff, tc >> 16, req))
        elif k == 'reg' and props.get('device_type', b'').startswith(b'audio-data'):
            print('  %-28s %s\n  %-28s %s' % (k, v.hex(), '', fmt_template(v)))
        elif k in ('reg', 'interrupts', 'clock-gates', 'i2s-version', 'headphone-pop-workaround',
                   'spkr-gain-db', 'slow-ramp-delay', 'interrupt-parent', 'AAPL,phandle') and len(v) % 4 == 0 and v:
            print('  %-28s %s' % (k, ' '.join('%#x' % w for w in struct.unpack('<%dI' % (len(v) // 4), v))))
        elif k == 'default-routes':
            for i in range(0, len(v), 12):
                print('  %-28s %r -> %r, %s' % (k, fourcc_le(v[i:i + 4]), fourcc_le(v[i + 4:i + 8]),
                                                v[i + 8:i + 12].hex()))
        else:
            txt = v.rstrip(b'\0')
            if txt and all(32 <= c < 127 for c in txt):
                print('  %-28s %r' % (k, txt.decode()))
            else:
                print('  %-28s %s' % (k, v.hex() if v else '(present)'))


WANT = ('i2c0/audio0', 'i2c0/headset-switch', 'i2s0', 'i2s0/audio0', 'i2s1', 'i2s1/highland-park', 'i2s2',
        'i2s2/audio2', 'i2s3', 'i2s3/audio3', 'i2s-switch', '3302753', 'cdma', 'gpio')
for path, props in nodes:
    tail = path.split('arm-io/', 1)[-1]
    if tail in WANT or props.get('compatible', b'').startswith((b'ae2,', b'cdma')):
        show(path, props)

pmgr = dict(nodes).get('device-tree/arm-io/pmgr')
if pmgr and 'device-clocks' in pmgr:
    dc = pmgr['device-clocks']
    size = 44 if len(dc) % 44 == 0 else 36
    print('[pmgr device-clocks, audio related]')
    for i in range(len(dc) // size):
        e = dc[i * size:(i + 1) * size]
        w = struct.unpack('<5I', e[:20])
        name = e[size - 16:].split(b'\0')[0].decode('latin1')
        if any(x in name for x in ('AUDIO', 'I2S', 'AE2', 'NCO', 'CDMA', 'I2C0', 'SPDIF', 'HPARK')):
            gate = 0x3f101000 + 4 * w[3] if 0 < w[3] < 0x400 else None
            clk = 0x3f100000 + 4 * w[4] if 0 < w[4] < 0x400 else None
            print('  %-12s id %3d  power-state %s  clock %s' % (name, e[3], '%#x' % gate if gate else '-',
                                                                 '%#x' % clk if clk else '-'))
