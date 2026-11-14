#!/usr/bin/env python3
"""What iOS writes to the iPad 2's CS42L63 codec, computed by iOS's own code.

    docker run --rm --user "$(id -u):$(id -g)" -v "$PWD":/cascadia -w /cascadia \
        cascadia-build python3 tools/audio/k93-cs42l63-emu.py [--sequences tools/audio/k93-cs42l63-sequences.py]

(from devices/a5; KC841 may point at another kernelcache, the default is
build/k93/firmware-12H321/kernelcache.12H321.k93.macho, iOS 8.4.1 for K93.)

AppleCS42L63Audio (com.apple.driver.AppleCS42L63Audio, text 0x80c7d4d0..)
is a subclass of AppleCSI2CAudio / AppleI2CAudio / AppleEmbeddedAudio
(com.apple.driver.AppleEmbeddedAudio), with a headset-detect helper object,
AppleCS42L63Mikey (an AppleCSCodecMikey, com.apple.driver.AppleCSEmbeddedAudio).
Its methods are run here in unicorn on a codec object built the way
AppleEmbeddedAudio::start (0x808ff7b0) and the codec's ADT parser
(+0x620, 0x80c7d620) leave it for K93's i2c0/audio0 node, and every I2C
transaction is logged:

  - the I2C nub at +0xb4 is a stand-in whose transaction method (vt+0x350:
    op 1 read / 2 write, address buffer, data buffer) logs and answers from a
    register model seeded with the live dump of the chip (reg 0x00..0x7f as
    found before Linux started, = the chip's reset values);
  - the platform functions function-reset (+0x8c, PMU GPIO 13) and
    function-spkr_mute (+0xc8, SoC GPIO 0x0407) are stand-ins that log the
    logical value iOS passes; the physical level follows from the GPIO
    drivers (AppleD1946PMU 0x80e9ab00, AppleS5L8930X 0x80debaa8), see
    docs/research/k93-audio.md;
  - the audio controls (IOAudio2 controls kept by AppleEmbeddedAudio) are a
    Python dict; getControlValue (0x80900a7c), getSelectorItem (0x80900aa0),
    getSelectorCount (0x80900af8), setControlValue (0x80900b9c) and
    isStreamRunning (0x80900c3c) are answered from it;
  - MCLK: the ADT has no function-mclk_control, so the codec asks its I2S
    nub for the master clock (0x80901424) and enables it (0x80901464); both
    are logged, the frequency answered is the audio0 template's 6 MHz;
  - IOSleep, the base's sleep (vt+0x46c, 0x809008e4) and IODelay are logged
    as delays; other kernel calls return 0.

Nothing touches the iPad.
"""
import os
import struct
import sys

from unicorn import Uc, UcError, UC_ARCH_ARM, UC_MODE_THUMB, UC_HOOK_CODE, UC_PROT_ALL
from unicorn.arm_const import *

HERE = os.path.dirname(os.path.abspath(__file__))
os.environ.setdefault('KC841', os.path.join(HERE, '..', '..', 'build', 'k93', 'firmware-12H321',
                                            'kernelcache.12H321.k93.macho'))
sys.path.insert(0, os.path.join(HERE, '..', 'iosgpu'))
import kctool as k  # noqa: E402

# --- addresses (12H321, K93) -------------------------------------------------
CODEC_VT = 0x80c800d8          # AppleCS42L63Audio vtable (vptr)
MIKEY_VT = 0x80c80628          # AppleCS42L63Mikey vtable (vptr)
F = {                          # codec methods run here
    'probe':          0x80c7d9d0,  # vt+0x4a8: device ID check
    'initHardware':   0x80c7da10,  # vt+0x3a8 (arg 0: cold, with reset; 1: wake)
    'powerDown':      0x80c7e020,  # vt+0x3ac (arg 1 = really power down)
    'setOutputActive': 0x80c7e164,  # vt+0x3b0 (output stream started/stopped)
    'setInputActive': 0x80c7e190,  # vt+0x3b4
    'route':          0x80c7dbd8,  # +0xbd8: the output routing (non-virtual)
    'setSampleRate':  0x80c7e738,  # vt+0x3c4 (0, rate)
    'controlChanged': 0x80c7ef40,  # vt+0x488 (id, value), after the value is stored
    'setHPLeft':      0x80c7f028,  # +0x2028 (forceMute)
    'setHPRight':     0x80c7f07c,  # +0x207c
    'setSpeakerVol':  0x80c7f0d0,  # +0x20d0
}
# AppleEmbeddedAudio base methods answered in Python
GET_CONTROL = 0x809009cc       # vt+0x404 (not expected to be reached)
GET_VALUE = 0x80900a7c         # vt+0x408 (id, default)
GET_SEL_ITEM = 0x80900aa0      # vt+0x40c (id, index, default)
GET_SEL_COUNT = 0x80900af8     # vt+0x410 (id)
SET_VALUE = 0x80900b9c         # vt+0x414 (id, value)
CTRL_NOTIFY = 0x80900bb4       # vt+0x428
IS_RUNNING = 0x80900c3c        # vt+0x420 (direction mask: 2 out, 1 in)
SLEEP_MS = 0x809008e4          # vt+0x46c (ms)
MCLK_FREQ = 0x80901424         # vt+0x450
MCLK_ENABLE = 0x80901464       # vt+0x460 (on)
# kernel functions with behaviour
K_IOSLEEP = 0x802e4658
K_SAFECAST = 0x802c3c50
K_NEW = 0x802c561c
K_PANIC = 0x8001ea30
K_GETPROVIDER = 0x802ed368
K_GETPROP3 = 0x802e8954        # IORegistryEntry::getProperty(const char*, plane, opts)
K_GETPROP1 = 0x802e8ccc        # IORegistryEntry::getProperty(const char*)


def _kernel_symbols():
    d, off, syms = k.D, 28, {}
    for _ in range(struct.unpack_from('<I', d, 16)[0]):
        cmd, csz = struct.unpack_from('<II', d, off)
        if cmd == 2:
            symoff, nsyms, stroff, _ = struct.unpack_from('<4I', d, off + 8)
            for i in range(nsyms):
                sx, _, _, _, val = struct.unpack_from('<IBBhI', d, symoff + 12 * i)
                if val:
                    syms.setdefault(val & ~1, d[stroff + sx:stroff + sx + 200].split(b'\0')[0].decode('latin1'))
        off += csz
    return syms


KSYMS = _kernel_symbols()

SEQ_HEADER = '''#!/usr/bin/env python3
"""CS42L63 (iPad 2 K93, I2C0 0x4a) register sequences as iOS 8.4.1 runs them.

GENERATED by tools/audio/k93-cs42l63-emu.py --sequences (do not edit): each
entry is one call into AppleCS42L63Audio (12H321 kernelcache) run in unicorn
with K93's ADT and the chip's reset values.  Steps:

  {'op': 'write',  'reg', 'val'}          single-byte I2C write (map byte = reg)
  {'op': 'update', 'reg', 'mask', 'val'}  read-modify-write: new = old & ~mask | val & mask
  {'op': 'read',   'reg'}                 a read iOS does (ID, revision, status poll)
  {'op': 'sleep_ms', 'ms'}                delay
  {'op': 'gpio', 'name', 'assert', 'level'}
        codec_reset = PMU (D1946) GPIO 13, spkr_mute = SoC GPIO 39 (0x0407);
        'assert' is iOS's logical value, 'level' the pin level (both active low)
  {'op': 'mclk', 'on', 'hz'}              I2S0 must put 6 MHz on the codec's MCLK

Values that depend on a control (volume) are shown for the value in 'title';
docs/research/k93-audio.md gives the formulas.  power_down polls reg 0x62
bit 7 (read, then sleep 10 ms, until set) where it shows one read.
"""
import json

'''
SEQ_FOOTER = '''

if __name__ == '__main__':
    print(json.dumps(SEQUENCES, indent=1))
'''

HEAP, HEAP_SZ = 0x10000000, 0x01000000
STACK, STACK_SZ = 0x20000000, 0x00100000
FAKE, FAKE_SZ = 0x30000000, 0x00400000
SENTINEL = 0x3ffff000

# the live dump of the chip (reg 0x00..0x7f) as found before Linux started
DUMP = bytes.fromhex(''.join((
    '00 42 a6 30 00 b0 1f 7f fc 00 00 00 15 00 15 00',   # 00
    '15 00 e0 a4 00 00 00 00 00 00 00 00 20 00 00 00',   # 10
    '00 40 40 40 40 40 40 05 00 00 00 00 00 7f 00 00',   # 20
    '7f 00 00 3f 00 00 00 3f 00 00 00 00 3f 3f 3f 3f',   # 30
    '7f 7f 7f 3f 3f 3f 3f 3f 7f 7f 7f 3f 3f 3f 3f 3f',   # 40
    '7f 7f 7f 3f 3f 3f 3f 3f 7f 7f 7f 3f 45 10 00 77',   # 50
    '9f ff 00 00 00 00 00 00 00 00 00 00 00 00 00 00',   # 60
    '03 c0 51 02 03 1f 2f 16 d0 e0 ff 00 00 00 00 00',   # 70
)).replace(' ', ''))
assert len(DUMP) == 128, len(DUMP)

# K93 ADT, /arm-io/i2c0/audio0 (audio-control,cs42l63)
ADT_PROPS = {
    'vsp-voice': b'', 'disable-auto-mic-bias-clamp': b'', 'xsp-reference': b'',
    'lineout2-spkr': b'', 'dmicb-intmic': b'', 'mic2-extmic': b'',
    'headphone-pop-workaround': struct.pack('<I', 2),
    'spkr-gain-db': struct.pack('<II', 0, 6),    # 6.0 dB, 32.32 fixed point
    'compatible': b'audio-control,cs42l63\0', 'name': b'audio0\0',
}

uc = Uc(UC_ARCH_ARM, UC_MODE_THUMB)
for name, va, vs, fo, fs in k.SEGS:
    if vs == 0:
        continue
    base, end = va & ~0xfff, (va + vs + 0xfff) & ~0xfff
    try:
        uc.mem_map(base, end - base, UC_PROT_ALL)
    except UcError:
        pass
    if fs:
        uc.mem_write(va, k.D[fo:fo + min(fs, vs)])
uc.mem_map(HEAP, HEAP_SZ)
uc.mem_map(STACK, STACK_SZ)
uc.mem_map(FAKE, FAKE_SZ)
uc.mem_map(SENTINEL & ~0xfff, 0x1000)
heap_top = [HEAP]


def alloc(n):
    p = heap_top[0]
    heap_top[0] = (p + n + 15) & ~15
    uc.mem_write(p, b'\0' * n)
    return p


def u32(a):
    return struct.unpack('<I', uc.mem_read(a, 4))[0]


def w32(a, v):
    uc.mem_write(a, struct.pack('<I', v & 0xffffffff))


def w8(a, v):
    uc.mem_write(a, bytes([v & 0xff]))


def reg(r):
    return uc.reg_read(r)


def args():
    sp = reg(UC_ARM_REG_SP)
    return [reg(UC_ARM_REG_R0), reg(UC_ARM_REG_R1), reg(UC_ARM_REG_R2), reg(UC_ARM_REG_R3),
            u32(sp), u32(sp + 4), u32(sp + 8)]


def ret(v=0):
    uc.reg_write(UC_ARM_REG_R0, v & 0xffffffff)
    uc.reg_write(UC_ARM_REG_PC, reg(UC_ARM_REG_LR))


def fourcc(s):
    return struct.unpack('>I', s.encode())[0]


def cc(v):
    try:
        s = struct.pack('>I', v).decode('ascii')
        if all(32 <= ord(c) < 127 for c in s):
            return "'%s'" % s
    except Exception:
        pass
    return '%#x' % v


# --- the event log -------------------------------------------------------------
events = []


def ev(kind, **kw):
    kw['op'] = kind
    events.append(kw)


# --- stand-in objects ------------------------------------------------------------
fakes = []          # index -> (name, handler)


def fake_obj(name, handler=None, size=0x100):
    n = len(fakes)
    fakes.append((name, handler))
    vt = alloc(0x800)
    for i in range(0x200):
        w32(vt + 4 * i, (FAKE + n * 0x1000 + 4 * i) | 1)
    o = alloc(size)
    w32(o, vt)
    return o


def on_fake(mu, addr, size, _):
    n, slot = (addr - FAKE) >> 12, (addr - FAKE) & 0xfff
    name, handler = fakes[n]
    a = args()
    if handler:
        r = handler(slot, a)
        ret(0 if r is None else r)
    else:
        ev('call', obj=name, slot=slot)
        ret(0)


# register model
regs = list(DUMP)
UPDATE_REG = 0x80902200          # AppleCSI2CAudio vt+0x4e0: update(reg, mask, value)
pending = {'update': 0}


def on_update(mu, addr, size, _):
    a = args()
    ev('update', reg=a[1] & 0xff, mask=a[2] & 0xff, val=a[3] & 0xff)
    pending['update'] = 2         # its read and its write are inner


def model_read(r):
    if r == 0x62:                 # power-down done (bit 7) follows PDN (0x06 bit 0)
        return 0x80 if regs[0x06] & 1 else 0x00
    return regs[r]


def i2c(slot, a):
    if slot != 0x350:
        ev('call', obj='i2c-nub', slot=slot)
        return 0
    _, op, alen, abuf, dlen, dbuf = a[:6]
    start = uc.mem_read(abuf, alen)[0]
    r = start & 0x7f
    if op == 2:
        data = bytes(uc.mem_read(dbuf, dlen))
        for i, v in enumerate(data):
            regs[(r + i) & 0x7f] = v
            ev('w', reg=(r + i) & 0x7f, val=v, inner=pending['update'] > 0)
            if pending['update']:
                pending['update'] -= 1
    elif op == 1:
        data = bytes(model_read((r + i) & 0x7f) for i in range(dlen))
        uc.mem_write(dbuf, data)
        for i, v in enumerate(data):
            ev('r', reg=(r + i) & 0x7f, val=v, inner=pending['update'] > 0)
            if pending['update']:
                pending['update'] -= 1
    else:
        ev('call', obj='i2c-nub', slot=slot, op_=op)
    return 0


def platform_function(fname):
    def h(slot, a):
        if slot == 0x3c:          # callFunction(param1 = &value, ...)
            v = u32(a[1]) if a[1] else None
            ev('gpio', func=fname, value=v)
            return 0
        ev('call', obj=fname, slot=slot)
        return 0
    return h


# properties of the codec's ADT node, as OSData stand-ins
prop_objs = {}


def osdata(name, data):
    buf = alloc(max(len(data), 4))
    uc.mem_write(buf, data)

    def h(slot, a):
        if slot == 0x68:          # getBytesNoCopy()
            return buf
        if slot == 0x4c:          # getLength()
            return len(data)
        return 0
    return fake_obj('OSData:' + name, h)


for pn, pd in ADT_PROPS.items():
    prop_objs[pn] = osdata(pn, pd)


def getprop_name(a):
    try:
        s = uc.mem_read(a, 64).split(b'\0')[0].decode()
    except Exception:
        return None
    return s


def provider_h(slot, a):
    if slot == 0x88:              # getProperty(const char*)
        n = getprop_name(a[1])
        ev('prop', name=n, present=n in prop_objs)
        return prop_objs.get(n, 0)
    ev('call', obj='provider', slot=slot)
    return 0


PROVIDER = fake_obj('provider(i2c0/audio0)', provider_h)

# controls
CTRL_NAMES = {
    0x12c: "jack present (outp)", 0x12d: "jack present (inpt)", 0x12e: "clock source",
    0x12f: "master mute", 0x130: "'dmut' (outp)", 0x131: "master volume (0..62 = -62..0 dB)",
    0x132: "volume elem1 (headphone L)", 0x133: "mute elem1 (headphone L)",
    0x134: "volume elem2 (headphone R)", 0x135: "mute elem2 (headphone R)",
    0x136: "data source elem2", 0x137: "volume elem3 (receiver)", 0x138: "mute elem3 (receiver)",
    0x139: "volume elem4 (speaker)", 0x13a: "mute elem4 (speaker)",
    0x13b: "data source elem4 ('codc'/'voic'/'xref')",
    0x16e: "input mute", 0x16f: "input volume", 0x170: "input data source",
    0x181: "'fm2r' passthrough", 0x182: "'bb2w' passthrough", 0x183: "'w2bb' passthrough",
    0x187: "'6wnf' (inpt)",
}
controls = {}
running = {'out': 0, 'in': 0}


def ctl_get(cid, default):
    if cid in controls:
        v = controls[cid]
        return v[0] if isinstance(v, list) else v
    return default


def base_hook(mu, addr, size, _):
    a = args()
    if addr == GET_VALUE:
        v = ctl_get(a[1], a[2])
        ev('ctl', id=a[1], value=v, default=a[2], known=a[1] in controls)
        ret(v)
    elif addr == GET_SEL_ITEM:
        cid, idx, default = a[1], a[2], a[3]
        v = controls.get(cid)
        if isinstance(v, list):
            val = v[idx] if idx < len(v) else default
        elif v is None:
            val = default
        else:
            val = v if idx == 0 else default
        ev('ctl', id=cid, index=idx, value=val, default=default, known=cid in controls)
        ret(val)
    elif addr == GET_SEL_COUNT:
        v = controls.get(a[1])
        n = len(v) if isinstance(v, list) else (1 if v is not None else 0)
        ev('ctl', id=a[1], count=n)
        ret(n)
    elif addr == SET_VALUE:
        controls[a[1]] = a[2]
        ev('setctl', id=a[1], value=a[2])
        ret(0)
    elif addr == CTRL_NOTIFY:
        ev('call', obj='device', slot=0x428, a1=a[1])
        ret(0)
    elif addr == GET_CONTROL:
        ev('call', obj='getControl', id=a[1])
        ret(0xe00002bc)
    elif addr == IS_RUNNING:
        d = a[1]
        on = ((d & 2) and running['out']) or ((d & 1) and running['in'])
        ev('running?', dir=d, value=int(bool(on)))
        ret(1 if on else 0)
    elif addr == SLEEP_MS:
        ev('sleep', ms=a[1], how='commandSleep')
        ret(0)
    elif addr == MCLK_FREQ:
        ev('mclk?', hz=6000000)
        ret(6000000)
    elif addr == MCLK_ENABLE:
        ev('mclk', on=a[1])
        ret(0)


BASE_HOOKS = (GET_CONTROL, GET_VALUE, GET_SEL_ITEM, GET_SEL_COUNT, SET_VALUE, CTRL_NOTIFY,
              IS_RUNNING, SLEEP_MS, MCLK_FREQ, MCLK_ENABLE)


def kernel_hook(mu, addr, size, _):
    a = args()
    if addr == K_IOSLEEP:
        ev('sleep', ms=a[0], how='IOSleep')
        ret(0)
    elif addr == K_SAFECAST:
        ret(a[0])
    elif addr == K_NEW:
        ret(alloc(a[0]))
    elif addr == K_PANIC:
        s = getprop_name(a[0])
        raise SystemExit('panic: %s' % s)
    elif addr == K_GETPROVIDER:
        ret(PROVIDER)
    elif addr in (K_GETPROP1, K_GETPROP3):
        n = getprop_name(a[1])
        ev('prop', name=n, present=n in prop_objs)
        ret(prop_objs.get(n, 0))
    else:
        ev('kcall', addr=addr, lr=reg(UC_ARM_REG_LR))
        ret(0)


def other_kext_hook(mu, addr, size, _):
    ev('kcall', addr=addr, lr=reg(UC_ARM_REG_LR))
    ret(0)


uc.hook_add(UC_HOOK_CODE, on_fake, begin=FAKE, end=FAKE + FAKE_SZ - 1)
uc.hook_add(UC_HOOK_CODE, on_update, begin=UPDATE_REG, end=UPDATE_REG)
for h in BASE_HOOKS:
    uc.hook_add(UC_HOOK_CODE, base_hook, begin=h, end=h)
uc.hook_add(UC_HOOK_CODE, kernel_hook, begin=0x80001000, end=0x80391fff)
# code outside the three audio kexts is not run: such calls return 0 and are logged
RUN = [(0x808ff000, 0x80910000), (0x8091a000, 0x80922000), (0x80c7d000, 0x80c82000)]
lo = 0x80460000
for a0, a1 in RUN + [(0x80f21000, 0x80f21000)]:
    if a0 > lo:
        uc.hook_add(UC_HOOK_CODE, other_kext_hook, begin=lo, end=a0 - 1)
    lo = a1

TRAMP = SENTINEL                  # nop; nop; blx r12; b .
uc.mem_write(TRAMP, bytes.fromhex('00bf00bfe047fee7'))


def call(fn, a=()):
    uc.reg_write(UC_ARM_REG_SP, STACK + STACK_SZ - 0x1000)
    for r, v in zip((UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3), a):
        uc.reg_write(r, v & 0xffffffff)
    uc.reg_write(UC_ARM_REG_R12, fn | 1)
    try:
        uc.emu_start(TRAMP | 1, TRAMP + 6, count=5_000_000)
    except UcError as e:
        raise SystemExit('emulation stopped: %s at pc %#x lr %#x' % (e, reg(UC_ARM_REG_PC), reg(UC_ARM_REG_LR)))
    return reg(UC_ARM_REG_R0)


# --- the codec and its Mikey, as start() and the ADT parser leave them ---------
codec = alloc(0x200)
w32(codec, CODEC_VT)
NUB = fake_obj('i2c-nub', i2c)
w32(codec + 0x50, fake_obj('iis-nub(i2s0/audio0)'))   # _iisNub, only used via hooks
w32(codec + 0x60, fake_obj('device'))                 # the IOAudio2 device
w32(codec + 0x8c, fake_obj('function-reset', platform_function('reset (PMU GPIO 13)')))
w32(codec + 0xb4, NUB)
w32(codec + 0xb8, 0x7e)          # AppleCSI2CAudio::init (0x80902568): max register 0x7e,
w8(codec + 0xbc, 1)              # registers from 1,
w8(codec + 0xbd, 0x7f)           # address mask,
w8(codec + 0xbe, 0x80)           # auto-increment bit for multi-byte transfers
w32(codec + 0xc0, 0)             # function-mclk_control: absent on K93
w32(codec + 0xc4, 0)             # function-hpout_ref_control: absent
w32(codec + 0xc8, fake_obj('function-spkr_mute', platform_function('spkr_mute (SoC GPIO 0x0407 = 39)')))
w8(codec + 0xcc, 1)              # lineout2-spkr
w8(codec + 0xcd, 1)              # xsp-reference
w32(codec + 0xd0, 0x10)          # mic flags: dmicb-intmic
w32(codec + 0xd4, 0)             # at-scale: absent
w32(codec + 0xd8, fake_obj('interrupt-source'))
mikey = alloc(0x200)
w32(mikey, MIKEY_VT)
w32(mikey + 0xbc, codec)         # AppleCS42L63Mikey's codec (set by its vt+0x340)
w32(mikey + 0xb4, 2)             # headphone-pop-workaround (0x8091c170)
w32(mikey + 0x7c, fake_obj('mikey-detect-timer'))   # its event sources: not run
w32(mikey + 0x78, fake_obj('mikey+0x78'))
w32(mikey + 0x80, fake_obj('mikey+0x80'))
w32(codec + 0xdc, mikey)
w32(codec + 0x90, mikey)
w32(codec + 0x54, 4)             # power stage: on


def run(title, fn, a=()):
    events.clear()
    rc = call(F[fn] if isinstance(fn, str) else fn, (codec,) + tuple(a))
    out = {'title': title, 'function': fn, 'args': list(a), 'rc': rc, 'events': list(events)}
    print('== %s: %s(%s) -> %#x' % (title, fn, ', '.join('%#x' % x for x in a), rc))
    for e in events:
        print('   ' + fmt(e))
    return out


def fmt(e):
    o = e['op']
    if o == 'w':
        return '%sW  %#04x = %#04x' % ('  ' if e.get('inner') else '', e['reg'], e['val'])
    if o == 'r':
        return '%sR  %#04x -> %#04x' % ('  ' if e.get('inner') else '', e['reg'], e['val'])
    if o == 'update':
        return 'U  %#04x mask %#04x value %#04x' % (e['reg'], e['mask'], e['val'])
    if o == 'sleep':
        return 'sleep %d ms (%s)' % (e['ms'], e['how'])
    if o == 'gpio':
        return 'function %s(%s)' % (e['func'], e['value'])
    if o == 'ctl':
        nm = CTRL_NAMES.get(e['id'], '')
        rest = ' '.join('%s=%s' % (kk, cc(v) if kk == 'value' else v) for kk, v in e.items()
                        if kk not in ('op', 'id'))
        return 'control %#x %s: %s' % (e['id'], nm, rest)
    if o == 'setctl':
        return 'set control %#x %s = %s' % (e['id'], CTRL_NAMES.get(e['id'], ''), e['value'])
    if o == 'kcall':
        return 'call %s (from %#x), not run' % (KSYMS.get(e['addr'], '%#x' % e['addr']), e['lr'])
    return ' '.join('%s=%s' % (kk, v) for kk, v in e.items())


GPIO_LEVEL = {   # logical value -> pin level, from the GPIO drivers (see the docstring)
    'reset (PMU GPIO 13)': lambda v: 0 if v else 1,               # flags byte 1 = 0: inverted
    'spkr_mute (SoC GPIO 0x0407 = 39)': lambda v: 0 if v else 1,  # likewise
}


def clean(evs):
    """the driver's view: writes, masked updates, reads that matter, delays, GPIOs, MCLK"""
    out = []
    for e in evs:
        o = e['op']
        if o == 'w' and not e.get('inner'):
            out.append({'op': 'write', 'reg': e['reg'], 'val': e['val']})
        elif o == 'update':
            out.append({'op': 'update', 'reg': e['reg'], 'mask': e['mask'], 'val': e['val']})
        elif o == 'r' and not e.get('inner'):
            out.append({'op': 'read', 'reg': e['reg']})
        elif o == 'sleep':
            out.append({'op': 'sleep_ms', 'ms': e['ms']})
        elif o == 'gpio':
            name = 'codec_reset' if e['func'].startswith('reset') else 'spkr_mute'
            out.append({'op': 'gpio', 'name': name, 'assert': e['value'],
                        'level': GPIO_LEVEL[e['func']](e['value'])})
        elif o == 'mclk':
            out.append({'op': 'mclk', 'on': e['on'], 'hz': 6000000})
    return out


SEQ = {}


def scenario(key, title, fn, a=()):
    r = run(title, fn, a)
    if key:
        SEQ[key] = {'title': title, 'ios_function': '%s %#x' % (fn, F[fn]) if isinstance(fn, str) else '%#x' % fn,
                    'args': list(a), 'steps': clean(r['events'])}
    return r


# ---- 1. probe and cold init -----------------------------------------------------
scenario('probe', 'probe: the device ID', 'probe')
# the framework's initial control values: toggles from the codec's hasControl
# (vt+0x3d0, 0x80c7e7f8: mutes start at 1), levels from getControlRange
# (vt+0x3d8, 0x80c7e94c), selectors on their first item (vt+0x3e4, 0x80c7ecac)
controls.update({0x12f: 0, 0x130: 0, 0x131: 0x3e, 0x132: 0, 0x133: 1, 0x134: 0, 0x135: 1,
                 0x138: 1, 0x139: 0, 0x13a: 1, 0x13b: fourcc('codc'),
                 0x16e: 0, 0x16f: 0, 0x170: fourcc('imic'), 0x181: 0, 0x182: 0, 0x183: 0, 0x187: 1})
scenario('init_cold', 'cold init (boot: AppleEmbeddedAudio::start -> initHardware(0))', 'initHardware', (0,))

# ---- 2. playback on the speaker --------------------------------------------------
scenario('rate_48000', 'sample rate 48000 (vt+0x3c4)', 'setSampleRate', (0, 48000))
scenario('rate_44100', 'sample rate 44100 (vt+0x3c4)', 'setSampleRate', (0, 44100))
controls.update({0x13a: 0, 0x139: 0x3e, 0x131: 0x3e})   # speaker unmuted, volumes up
running['out'] = 1
scenario('speaker_start', 'output stream starts, speaker selected (0x13a = 0, 0x139 = 0x3e, 0x131 = 0x3e)',
         'setOutputActive', (1,))

# ---- 3. volume ------------------------------------------------------------------
for v in (0x3e, 0x30, 0x0d, 0x00):
    controls[0x139] = v
    scenario('speaker_volume_%#04x' % v, 'speaker volume 0x139 = %#x' % v, 'controlChanged', (0x139, v))
controls[0x139] = 0x3e
for v in (0x3e, 0x20, 0x00):
    controls[0x131] = v
    scenario('master_volume_%#04x' % v, 'master volume 0x131 = %#x, speaker playing' % v,
             'controlChanged', (0x131, v))
controls[0x131] = 0x3e

# ---- 4. switch to headphones while playing ---------------------------------------
controls[0x13a] = 1
scenario('speaker_mute_while_playing', 'speaker mute 0x13a = 1', 'controlChanged', (0x13a, 1))
controls[0x133] = 0
scenario('hp_left_unmute_while_playing', 'HP left mute 0x133 = 0', 'controlChanged', (0x133, 0))
controls[0x135] = 0
scenario('hp_right_unmute_while_playing', 'HP right mute 0x135 = 0', 'controlChanged', (0x135, 0))
for v in (0x3e, 0x30, 0x00):
    controls[0x132] = v
    scenario('hp_left_volume_%#04x' % v, 'HP left volume 0x132 = %#x' % v, 'controlChanged', (0x132, v))
    controls[0x134] = v
    scenario('hp_right_volume_%#04x' % v, 'HP right volume 0x134 = %#x' % v, 'controlChanged', (0x134, v))
controls[0x132] = controls[0x134] = 0x3e

# headphones from a stopped state
running['out'] = 0
scenario('hp_stop', 'output stream stops (headphones selected)', 'setOutputActive', (0,))
running['out'] = 1
scenario('hp_start', 'output stream starts, headphones selected (0x133 = 0x135 = 0, 0x13a = 1, HP volumes 0x3e)',
         'setOutputActive', (1,))

# back to the speaker
controls[0x133] = 1
scenario('hp_left_mute_while_playing', 'HP left mute 0x133 = 1', 'controlChanged', (0x133, 1))
controls[0x135] = 1
scenario('hp_right_mute_while_playing', 'HP right mute 0x135 = 1', 'controlChanged', (0x135, 1))
controls[0x13a] = 0
scenario('speaker_unmute_while_playing', 'speaker mute 0x13a = 0 (also mutes the receiver, 0x138)',
         'controlChanged', (0x13a, 0))

# both at once (iOS never asks for this, but it shows the bits)
controls.update({0x133: 0, 0x135: 0, 0x13a: 0})
scenario('route_speaker_and_hp', 'route with speaker and headphones on (not an iOS state)', 'route')
controls.update({0x133: 1, 0x135: 1, 0x13a: 0})

# ---- 5. stop, power down, wake ---------------------------------------------------
running['out'] = 0
scenario('speaker_stop', 'output stream stops (speaker selected)', 'setOutputActive', (0,))
scenario('power_down', 'power down (power stage 5: powerDown(1, 0))', 'powerDown', (1, 0))
scenario(None, 'power down, stage 6 (the codec declines)', 'powerDown', (0, 0))
scenario('init_wake', 'wake (power stage 3: initHardware(1), no reset)', 'initHardware', (1,))

# ---- 6. inputs, for completeness --------------------------------------------------
running['in'] = 1
scenario('input_internal_mic', 'input stream started, internal mic (DMIC B)', 'setInputActive', (1,))
controls[0x170] = fourcc('emic')
scenario('input_headset_mic', 'input stream, headset mic (0x170 = emic)', 'setInputActive', (1,))
running['in'] = 0
scenario('input_stop', 'input stream stopped', 'setInputActive', (0,))

# ---- 7. the input volume (0x16f): iOS's gain mapping, per source -----------------
# controlChanged(0x16f, v) while an input runs writes the mic path's boost (0x14
# bits 2:0), analog stage (0x16-0x18, one value) and digital volume (0x19-0x1b,
# one value, two's complement), differently for each source.  On iOS's dB scale
# the boost is +20 dB, the analog stage 0.5 dB a step from -6 dB (bits 5:0) with
# +10 dB more in bit 7, the digital volume 1 dB a step up to +12 dB.  The control's range depends on the source (getControlRange,
# 0x80c7e94c): the internal mic 0..128 = -96..+32 dB in 1 dB steps, the
# headset's 0..312 = -92..+64 dB in 0.5 dB steps; both default to +32 dB.
# GAINS[src][i] = [boost, analog, digital] for i = 0..128 = -96..+32 dB, one
# scale for both: v = GAIN_V[src][i] (the internal mic's i, the headset's 2i - 8).
# The emulation's own default (0x16f = 0) is iOS's quietest: -96 dB.
GAIN_V = {'imic': list(range(129)), 'emic': [max(0, 2 * i - 8) for i in range(129)]}
GAINS = {}
for _mic in ('imic', 'emic'):
    controls[0x170] = fourcc(_mic)
    running['in'] = 1
    _rows = []
    for _v in GAIN_V[_mic]:
        controls[0x16f] = _v
        _regs = {'boost': 0}
        for _s in clean(run('input %s, volume 0x16f = %d' % (_mic, _v), 'controlChanged', (0x16f, _v))['events']):
            if _s['op'] == 'write':
                _regs[_s['reg']] = _s['val']
            elif _s['op'] == 'update' and _s['reg'] == 0x14:
                _regs['boost'] = _s['val'] & _s['mask'] & 0x07
        assert _regs[0x16] == _regs[0x17] == _regs[0x18], (_mic, _v, _regs)
        assert _regs[0x19] == _regs[0x1a] == _regs[0x1b], (_mic, _v, _regs)
        _rows.append([_regs['boost'], _regs[0x16], _regs[0x19]])
    GAINS[_mic] = _rows
running['in'] = 0
controls[0x170] = fourcc('imic')
controls[0x16f] = 0


def pyrepr(obj, ind=0):
    """a Python literal with hex register numbers"""
    pad = ' ' * ind
    if isinstance(obj, dict):
        if 'op' in obj:
            return '{' + ', '.join('%r: %s' % (k, ('%#04x' % v) if k in ('reg', 'val', 'mask') else repr(v))
                                   for k, v in obj.items()) + '}'
        return '{\n' + ''.join('%s    %r: %s,\n' % (pad, k, pyrepr(v, ind + 4)) for k, v in obj.items()) + pad + '}'
    if isinstance(obj, list) and obj and isinstance(obj[0], dict):
        return '[\n' + ''.join('%s    %s,\n' % (pad, pyrepr(v, ind + 4)) for v in obj) + pad + ']'
    return repr(obj)


if '--sequences' in sys.argv:
    path = sys.argv[sys.argv.index('--sequences') + 1]
    with open(path, 'w') as f:
        f.write(SEQ_HEADER)
        f.write('SEQUENCES = ' + pyrepr(SEQ) + '\n')
        f.write('\n# controlChanged(0x16f, v): [boost (0x14 bits 2:0), analog (0x16-0x18), '
                'digital (0x19-0x1b)] for v = GAIN_V[src][i], i = 0..128 = -96..+32 dB\n')
        f.write('GAIN_V = {\n' + ''.join('    %r: %r,\n' % (k, v) for k, v in GAIN_V.items()) + '}\n')
        f.write('GAINS = {\n' + ''.join('    %r: %r,\n' % (k, v) for k, v in GAINS.items()) + '}\n')
        f.write(SEQ_FOOTER)
    print('wrote', path)
