#!/usr/bin/env python3
"""What iOS writes to the iPad 2's I2S0 (the CS42L63 port), computed by iOS's own code.

    docker run --rm --user "$(id -u):$(id -g)" -v "$PWD":/cascadia -w /cascadia \
        cascadia-build python3 tools/audio/k93-i2s-emu.py

(from devices/a5; KC841 may point at another kernelcache, the default is the
K93 12H321 one.)

AppleSamsungI2SController (com.apple.driver.AppleSamsungI2S, vtable
0x80df60c8) drives the S5L8900-heritage I2S blocks ("i2s-1,samsung",
i2s-version 3).  Run here, on the K93 i2s0/audio0 template
(ADT reg = 11000000 02000100 808d5b00 ...):

    setPower        vt+0x340  0x80df4968   (on/off; off keeps MCLK if asked)
    isFormatOK      vt+0x344  0x80df4a94
    configure       vt+0x348  0x80df4d78
    start           vt+0x350  0x80df5408   (direction mask: 2 TX, 1 RX)
    stop            vt+0x354  0x80df5460
    setMCLKEnable   vt+0x35c  0x80df5534   (what the codec's enableMasterClock
                                            reaches, via AppleARMIISDevice
                                            vt+0x370 0x804ad528 and the
                                            controller's +0x6c action)

The object carries what start() (0x80df45dc) leaves: +0x84 the register
window, +0x88 i2s-version 3, +0x8c i2s-multi 0, +0x90/+0x94 the TX/RX DMA
event sources, +0xa8 function-mclk_frequency (NCO 0), +0xb8
function-pclk_gate (AE2 index 0), +0xbc function-i2s_route.  The provider's
clock "nclk" (AppleARMIODevice vt+0x34c -> AppleS5L8940XIO) is
gPEClockFrequencyInfo.fix_frequency_hz, the 24 MHz fixed clock; --nclk
changes it.  Calls into AppleARMPlatform (the format helper +0x3f28) and the
kext's own division helpers are run; the kernel and the platform-function
stand-ins are logged.  Nothing touches the iPad.
"""
import os
import struct
import sys

from unicorn import Uc, UcError, UC_ARCH_ARM, UC_MODE_THUMB, UC_HOOK_CODE, UC_HOOK_MEM_READ, \
    UC_HOOK_MEM_WRITE, UC_PROT_ALL
from unicorn.arm_const import *

HERE = os.path.dirname(os.path.abspath(__file__))
os.environ.setdefault('KC841', os.path.join(HERE, '..', '..', 'build', 'k93', 'firmware-12H321',
                                            'kernelcache.12H321.k93.macho'))
sys.path.insert(0, os.path.join(HERE, '..', 'iosgpu'))
import kctool as k  # noqa: E402

VTABLE = 0x80df60c8
SET_POWER, IS_FORMAT_OK, CONFIGURE, START, STOP, SET_MCLK = \
    0x80df4968, 0x80df4a94, 0x80df4d78, 0x80df5408, 0x80df5460, 0x80df5534
ROUTE_CALL = 0x804c0964            # AppleARMPlatform+0x17964: function-i2s_route
REGNAMES = {0x00: 'CLKCON', 0x04: 'TXCON', 0x08: 'TXCOM', 0x30: 'RXCON', 0x34: 'RXCOM',
            0x3c: 'STATUS', 0x40: '+0x40', 0x410: '+0x410'}

# the K93 ADT's /arm-io/i2s0/audio0 (audio-data,cs42l63) reg, 36 bytes
TEMPLATE = bytes.fromhex('1100000002000100808d5b00000000000000000003000000030000000202101000000000')

NCLK = 24000000
if '--nclk' in sys.argv:
    NCLK = int(sys.argv[sys.argv.index('--nclk') + 1], 0)

HEAP, HEAP_SZ = 0x10000000, 0x01000000
STACK, STACK_SZ = 0x20000000, 0x00100000
FAKE, FAKE_SZ = 0x30000000, 0x00100000
MMIO = 0x40000000
SENTINEL = 0x3ffff000

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
uc.mem_map(MMIO, 0x1000)
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


log = []
fakes = []


def fake_obj(name):
    n = len(fakes)
    fakes.append(name)
    vt = alloc(0x800)
    for i in range(0x200):
        w32(vt + 4 * i, (FAKE + n * 0x1000 + 4 * i) | 1)
    o = alloc(0x40)
    w32(o, vt)
    return o


def ret(r0, r1=None):
    uc.reg_write(UC_ARM_REG_R0, r0 & 0xffffffff)
    if r1 is not None:
        uc.reg_write(UC_ARM_REG_R1, r1 & 0xffffffff)
    uc.reg_write(UC_ARM_REG_PC, uc.reg_read(UC_ARM_REG_LR))


def on_fake(mu, addr, size, _):
    n, slot = (addr - FAKE) >> 12, (addr - FAKE) & 0xfff
    r = [mu.reg_read(x) for x in (UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3)]
    name = fakes[n]
    if name == 'provider' and slot == 0x34c:          # getClockFrequency(name) -> 64 bit
        s = uc.mem_read(r[1], 16).split(b'\0')[0].decode()
        log.append('provider clock %r -> %d Hz' % (s, NCLK if s == 'nclk' else 0))
        ret(NCLK if s == 'nclk' else 0, 0)
        return
    if slot == 0x3c and name.startswith('function-'):  # callFunction(param1, ...)
        # param1 is a pointer to the value (mclk: Hz) or the value itself (pclk gate)
        v = u32(r[1]) if r[1] >= 0x1000 else r[1]
        log.append('%s(%s)' % (name, v))
        ret(0)
        return
    if name == 'provider' and slot == 0x354:
        log.append('provider power/clock gate (I2S0 PMGR state) -> %d' % r[1])
        ret(0)
        return
    if name.endswith('dma') and slot == 0x88:
        log.append('%s set width code %d' % (name, r[1]))
        ret(1)
        return
    if name.endswith('dma') and slot == 0x80:
        log.append('%s set FIFO watermark/burst %d' % (name, r[1]))
        ret(0)
        return
    log.append('call %s+%#x(%#x, %#x, %#x)' % (name, slot, r[1], r[2], r[3]))
    ret(1)


def on_kernel(mu, addr, size, _):
    log.append('kernel %#x from %#x' % (addr, mu.reg_read(UC_ARM_REG_LR)))
    ret(0)


def on_route(mu, addr, size, _):
    r = [mu.reg_read(x) for x in (UC_ARM_REG_R1, UC_ARM_REG_R2)]
    log.append('function-i2s_route(directions %#x, mode %#x)' % (r[0], r[1]))
    ret(0)


mmio = {}


def on_read(mu, access, addr, size, value, _):
    off = addr - MMIO
    v = mmio.get(off, 0)
    if off == 0x00:
        v |= 2                     # CLKCON bit 1: "stopped" reads back set
    w32(addr, v)
    log.append('  read  %-7s -> %08x' % (REGNAMES.get(off, '+%#x' % off), v))


def on_write(mu, access, addr, size, value, _):
    off = addr - MMIO
    mmio[off] = value & 0xffffffff
    log.append('  write %-7s = %08x' % (REGNAMES.get(off, '+%#x' % off), value & 0xffffffff))


uc.hook_add(UC_HOOK_CODE, on_fake, begin=FAKE, end=FAKE + FAKE_SZ - 1)
uc.hook_add(UC_HOOK_CODE, on_route, begin=ROUTE_CALL, end=ROUTE_CALL)
uc.hook_add(UC_HOOK_CODE, on_kernel, begin=0x80001000, end=0x80391fff)
uc.hook_add(UC_HOOK_MEM_READ, on_read, begin=MMIO, end=MMIO + 0xfff)
uc.hook_add(UC_HOOK_MEM_WRITE, on_write, begin=MMIO, end=MMIO + 0xfff)

TRAMP = SENTINEL
uc.mem_write(TRAMP, bytes.fromhex('00bf00bfe047fee7'))


def call(fn, a=()):
    uc.reg_write(UC_ARM_REG_SP, STACK + STACK_SZ - 0x1000)
    for r, v in zip((UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3), a):
        uc.reg_write(r, v & 0xffffffff)
    uc.reg_write(UC_ARM_REG_R12, fn | 1)
    try:
        uc.emu_start(TRAMP | 1, TRAMP + 6, count=2_000_000)
    except UcError as e:
        print('\n'.join(log))
        sys.exit('emulation stopped: %s at pc %#x lr %#x' % (e, uc.reg_read(UC_ARM_REG_PC),
                                                            uc.reg_read(UC_ARM_REG_LR)))
    return uc.reg_read(UC_ARM_REG_R0)


def new_controller():
    o = alloc(0x100)
    w32(o, VTABLE)
    w32(o + 0x60, fake_obj('command-gate'))
    w32(o + 0x7c, fake_obj('provider'))
    w32(o + 0x84, MMIO)
    w32(o + 0x88, 3)                       # i2s-version
    w32(o + 0x8c, 0)                       # i2s-multi
    w32(o + 0x90, fake_obj('tx-dma'))
    w32(o + 0x94, fake_obj('rx-dma'))
    w32(o + 0xa8, fake_obj('function-mclk_frequency (NCO 0, Hz)'))
    w32(o + 0xb8, fake_obj('function-pclk_gate (AE2 0)'))
    w32(o + 0xbc, fake_obj('function-i2s_route'))
    w32(o + 0xc8, 0)                       # no i2s-slave
    return o


def descriptor(rate_hz, bits=16):
    d = bytearray(TEMPLATE)
    struct.pack_into('<HH', d, 0x10, rate_hz // 10, 100)   # rate = num * 1000 / den Hz
    d[0x1e] = d[0x1f] = bits
    p = alloc(len(d))
    uc.mem_write(p, bytes(d))
    return p, bytes(d)


def section(title):
    print('== ' + title)
    for line in log:
        print('   ' + line)
    log.clear()


def nco_value(ctl):
    # what the start() handed function-mclk_frequency
    for line in reversed(ctl):
        if line.startswith('function-mclk_frequency'):
            return line
    return None


print('nclk = %d Hz' % NCLK)
for rate in (48000, 44100):
    for bits in (16,):
        mmio.clear()
        log.clear()
        c = new_controller()
        p, d = descriptor(rate, bits)
        ok = call(IS_FORMAT_OK, (c, p))
        log.append('isFormatSupported -> %d' % ok)
        section('%d Hz, %d-bit: descriptor %s' % (rate, bits, d.hex(' ')))
        call(SET_MCLK, (c, p, 1))
        section('setMCLKEnable(1) at codec init, before any stream (the codec asking for 6 MHz)')
        call(SET_POWER, (c, 1))
        section('setPower(1)')
        call(CONFIGURE, (c, p))
        log.append('object: +0xa0 TXCOM-on %08x  +0xa4 RXCOM-on %08x  +0xac MCLK/NCO %d Hz  +0xc0 slave %d'
                   % (u32(c + 0xa0), u32(c + 0xa4), u32(c + 0xac), uc.mem_read(c + 0xc0, 1)[0]))
        section('configure')
        call(START, (c, 2))
        section('start(TX)')
        call(START, (c, 1))
        section('start(RX)')
        call(STOP, (c, 3))
        section('stop(TX|RX)')
        call(SET_POWER, (c, 0))
        section('setPower(0): stream over (MCLK still requested by the codec)')
        call(SET_MCLK, (c, p, 1))
        section('setMCLKEnable(1) while idle (the codec asking for MCLK)')
        call(SET_MCLK, (c, p, 0))
        section('setMCLKEnable(0)')
        print('   final: ' + ' '.join('%s=%08x' % (REGNAMES.get(o, '+%#x' % o), v)
                                      for o, v in sorted(mmio.items())))
