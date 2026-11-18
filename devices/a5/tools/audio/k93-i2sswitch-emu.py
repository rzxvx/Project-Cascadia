#!/usr/bin/env python3
"""What iOS writes to the iPad 2's i2s-switch, computed by iOS's own code.

    docker run --rm --user "$(id -u):$(id -g)" -v "$PWD":/cascadia -w /cascadia \
        cascadia-build python3 tools/audio/k93-i2sswitch-emu.py

(from devices/a5; KC841 may point at another kernelcache, the default is the
K93 12H321 one.)

K93's switch is i2s-switch,s5l8940x (0x3fa01000), driven by
AppleAE2I2SSwitch1 (com.apple.driver.AppleAE2Audio, vtable 0x80ce8498) -- not
the iPad mini's AppleAE2I2SSwitch2.  Its nodes (vt+0x350, 0x80ce5d34) are
hp0 hp1 hp2 hclk ap0 ap1 ap2 ap3 pin0..pin3 (12), it keeps 16 registers
(vt+0x358) and writes them all to its window (vt+0x364, 0x80ce5d14); there is
no version register.  The graph router is AppleARMIISSwitch's
(AppleARMPlatform 0x804c09a8, the same code and address as in the iPad mini's
kernelcache), with the subclass's per-edge encoder (vt+0x360, 0x80ce5e80).

Like tools/audio/i2sswitch-emu.py: the object is built the way
AppleAE2I2SSwitch1::start (0x80ce5cbc) and AppleARMIISSwitch::start
(0x804c10f0; its counts/node-table slice 0x804c1114..0x804c11da is run) leave
it, then the router is called the way iOS does: the ADT's default-routes
(hclk -> hp0, 01 01 00: router(src, dst, b0, b0, b1, b1, b2, b2)), then i2s0's
function-i2s_route (ap0 -> pin0, 03 03 33) with the arguments
AppleSamsungI2SController passes (AppleARMFunctionIISRoute 0x804c0964:
router(src, dst, b0, dirs, b1, dirs, b2, mode)): configure (dirs 3, mode 2),
the MCLK-only power-down (dirs 0, mode 0x20) and the plain power-down (0, 0).
Nothing touches the iPad.
"""
import os
import struct
import sys

from unicorn import Uc, UcError, UC_ARCH_ARM, UC_MODE_THUMB, UC_HOOK_CODE, UC_PROT_ALL
from unicorn.arm_const import *
import unicorn.arm_const as arm_const_mod

HERE = os.path.dirname(os.path.abspath(__file__))
os.environ.setdefault('KC841', os.path.join(HERE, '..', '..', 'build', 'k93', 'firmware-12H321',
                                            'kernelcache.12H321.k93.macho'))
sys.path.insert(0, os.path.join(HERE, '..', 'iosgpu'))
import kctool as k  # noqa: E402

SWITCH1_VTABLE = 0x80ce8498
ROUTER = 0x804c09a8
BASE_INIT = (0x804c1114, 0x804c11da)
STUBS = {                            # AppleARMPlatform's stubs into the kernel
    0x804c9bc4: 'new[]', 0x804c9ba4: 'delete[]', 0x804c9c34: 'bzero',
    0x804c9334: 'IOLockAlloc', 0x804c9354: 'lck_mtx_lock', 0x804c9374: 'lck_mtx_unlock',
    0x804c99e4: 'OSArray::withCapacity',
}
HEAP, HEAP_SZ = 0x10000000, 0x01000000
STACK, STACK_SZ = 0x20000000, 0x00100000
FAKE = 0x30000000
SENTINEL = 0x30001000

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
uc.mem_map(FAKE, 0x2000)
heap_top = [HEAP]


def alloc(n):
    if n > 0x100000:
        raise SystemExit('alloc of %#x at pc %#x' % (n, uc.reg_read(UC_ARM_REG_PC)))
    p = heap_top[0]
    heap_top[0] = (p + n + 15) & ~15
    uc.mem_write(p, b'\0' * n)
    return p


def u32(a):
    return struct.unpack('<I', uc.mem_read(a, 4))[0]


def w32(a, v):
    uc.mem_write(a, struct.pack('<I', v & 0xffffffff))


fake_vt = alloc(0x400)
for i in range(0x100):
    w32(fake_vt + 4 * i, FAKE | 1)


def fake_obj():
    o = alloc(0x40)
    w32(o, fake_vt)
    return o


SHARED = fake_obj()


def ret(v):
    uc.reg_write(UC_ARM_REG_R0, v & 0xffffffff)
    uc.reg_write(UC_ARM_REG_PC, uc.reg_read(UC_ARM_REG_LR))


def on_code(mu, addr, size, _):
    if addr in STUBS:
        name = STUBS[addr]
        r0, r1 = mu.reg_read(UC_ARM_REG_R0), mu.reg_read(UC_ARM_REG_R1)
        if name == 'new[]':
            ret(alloc(r0))
        elif name == 'bzero':
            mu.mem_write(r0, b'\0' * r1)
            ret(0)
        elif name in ('IOLockAlloc', 'OSArray::withCapacity'):
            ret(SHARED)
        else:
            ret(0)
    elif addr == FAKE:
        ret(1)
    elif 0x80001000 <= addr < 0x80392000:
        ret(SHARED)


uc.hook_add(UC_HOOK_CODE, on_code, begin=0x80001000, end=0x80392000)
uc.hook_add(UC_HOOK_CODE, on_code, begin=0x804c9000, end=0x804ca000)
uc.hook_add(UC_HOOK_CODE, on_code, begin=FAKE, end=FAKE + 4)


def call(fn, a=(), stack_args=()):
    sp = STACK + STACK_SZ - 0x1000
    sp -= 4 * len(stack_args)
    for i, v in enumerate(stack_args):
        w32(sp + 4 * i, v)
    uc.reg_write(UC_ARM_REG_SP, sp)
    for r, v in zip((UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3), a):
        uc.reg_write(r, v & 0xffffffff)
    uc.reg_write(UC_ARM_REG_LR, SENTINEL | 1)
    try:
        uc.emu_start(fn | 1, SENTINEL, count=5_000_000)
    except UcError as e:
        names = ('R0', 'R1', 'R2', 'R3', 'R4', 'R5', 'R6', 'R7', 'R8', 'R9', 'R10', 'R11', 'R12', 'SP', 'LR', 'PC')
        state = ' '.join('%s=%08x' % (r, uc.reg_read(getattr(arm_const_mod, 'UC_ARM_REG_' + r))) for r in names)
        sys.exit('emulation stopped: %s\n  %s' % (e, state))
    return uc.reg_read(UC_ARM_REG_R0)


def fourcc(s):
    return struct.unpack('>I', s.encode())[0]


# the object as AppleAE2I2SSwitch1::start leaves it before calling its parent
obj = alloc(0x100)
mmio = alloc(0x1000)
w32(obj, SWITCH1_VTABLE)
w32(obj + 0x64, fake_obj())
w32(obj + 0x68, fake_obj())
w32(obj + 0x6c, mmio)
uc.reg_write(UC_ARM_REG_R11, obj)
uc.reg_write(UC_ARM_REG_R10, fake_obj())
uc.reg_write(UC_ARM_REG_R7, STACK + STACK_SZ - 0x800)
uc.reg_write(UC_ARM_REG_SP, STACK + STACK_SZ - 0x900)
uc.emu_start(BASE_INIT[0] | 1, BASE_INIT[1], count=1_000_000)
n, nregs = u32(obj + 0x50), u32(obj + 0x54)
table = u32(obj + 0x5c)
for i in range(n):
    w32(table + 12 * i + 4, fake_obj())
TEMPLATE = (bytes(uc.mem_read(obj, 0x100)), bytes(uc.mem_read(table, 12 * n)))


def new_switch():
    global obj, mmio
    o, t = TEMPLATE
    obj, mmio = alloc(0x100), alloc(0x1000)
    uc.mem_write(obj, o)
    tab = alloc(12 * n)
    uc.mem_write(tab, t)
    w32(obj + 0x5c, tab)
    w32(obj + 0x60, alloc(2 * n * n))
    w32(obj + 0x6c, mmio)
    return [(struct.pack('>I', u32(tab + 12 * i)).decode('latin1'), u32(tab + 12 * i + 8)) for i in range(n)]


nodes = new_switch()
print('nodes (%d): %s; %d registers' % (n, ' '.join('%s(type %d)' % x for x in nodes), nregs))


def regs():
    return [u32(mmio + 4 * i) for i in range(nregs)]


def route(src, dst, b0, b1, b2, v0, v1, v2):
    return call(ROUTER, (obj, fourcc(src), fourcc(dst), b0), (v0, b1, v1, b2, v2, 0, 0))


DEFAULT = ('hclk', 'hp0 ', 1, 1, 0, 1, 1, 0)        # the ADT's default-routes
I2S0 = ('ap0 ', 'pin0', 3, 3, 0x33)                  # i2s0's function-i2s_route
SCENARIOS = [
    ('default routes only', []),
    ('i2s0 configure: ap0 -> pin0, dirs 3, mode 2 (I2S0 master)', [I2S0 + (3, 3, 2)]),
    ('i2s0 configure after the codec asked for MCLK: dirs 3, mode 0x22 (what iOS runs)',
     [I2S0 + (0, 0, 0x20), I2S0 + (3, 3, 0x22)]),
    ('i2s0 TX only, MCLK asked: dirs 2, mode 0x22', [I2S0 + (2, 2, 0x22)]),
    ('i2s0 RX only, MCLK asked: dirs 1, mode 0x22', [I2S0 + (1, 1, 0x22)]),
    ('i2s0 configure for TX only (dirs 2, mode 2)', [I2S0 + (2, 2, 2)]),
    ('i2s0 idle with MCLK kept (setPower(0) after setMCLKEnable(1): dirs 0, mode 0x20)',
     [I2S0 + (3, 3, 2), I2S0 + (0, 0, 0x20)]),
    ('i2s0 idle, MCLK off (dirs 0, mode 0)', [I2S0 + (3, 3, 2), I2S0 + (0, 0, 0)]),
    ('i2s0 as clock slave (mode 1), for comparison', [I2S0 + (3, 3, 1)]),
    ('i2s0 as clock slave with MCLK (mode 0x21), for comparison', [I2S0 + (3, 3, 0x21)]),
    ('ap1 -> pin0 instead (03 03 33), dirs 3, mode 0x22', [('ap1 ', 'pin0', 3, 3, 0x33, 3, 3, 0x22)]),
    ('ap0 -> pin1 (03 03 33), dirs 3, mode 0x22', [('ap0 ', 'pin1', 3, 3, 0x33, 3, 3, 0x22)]),
    ('i2s3 (reference) ap3 -> pin3 03 03 03, dirs 3, mode 2', [('ap3 ', 'pin3', 3, 3, 3, 3, 3, 2)]),
    ('i2s1 (highland park) ap1 -> hp2 03 03 03, dirs 3, mode 2', [('ap1 ', 'hp2 ', 3, 3, 3, 3, 3, 2)]),
]
for title, routes in SCENARIOS:
    new_switch()
    rc0 = route(*DEFAULT)
    before = regs()
    rcs = [hex(route(*a)) for a in routes]
    print('-- %s (default %#x, router: %s)' % (title, rc0, ', '.join(rcs)))
    for i, v in enumerate(regs()):
        if v or before[i]:
            print('   reg %2d (+0x%02x) = %08x%s' % (i, 4 * i, v,
                                                   '' if v == before[i] else '   (default-routes: %08x)' % before[i]))
