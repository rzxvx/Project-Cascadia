#!/usr/bin/env python3
"""What iOS writes to the i2s-switch, computed by iOS's own code.

    KC841=build/firmware/kernelcache.12H321.macho python3 tools/audio/i2sswitch-emu.py

The switch (ADT i2s-switch,s5l8942x, 0x3fa01000) routes the SoC's audio ports
-- I2S aud0/1, MCA mca0/1, the DSP lanes -- to its four pin groups.  Its
driver, AppleAE2I2SSwitch2, keeps 21 registers whose values a generic graph
router in AppleARMIISSwitch (com.apple.iokit.AppleARMIISAudio) computes from
a connection matrix, through the subclass's per-edge encoder.  Reading that
router well enough to redo it by hand is error-prone; running it is not.

This maps the decrypted 12H321 kernelcache into unicorn, builds the switch
object the way the two start() methods do -- node count, register count,
the node table from the subclass's own name and type methods, a zeroed
connection matrix, pin-count 4, dsp-count 3, the hardware version 0x20001
the register at 0x3fa01ffc reads -- and calls the router (0x804c09a8) the
way iOS does: first the ADT's default-routes, then each port's
function-i2s_route with the arguments its driver passes when it starts a
stream (MCA: 0x80cfb020; I2S: 0x80df70c8).  The 21 words the subclass then
writes to its register window are printed.

Calls out of the two kexts into the kernel are not run: memory management,
locks and bzero are done here; anything else (setProperty, IOLog) returns 1.
Nothing touches the iPad.
"""
import os
import struct
import sys

from unicorn import Uc, UcError, UC_ARCH_ARM, UC_MODE_THUMB, UC_HOOK_CODE, UC_HOOK_BLOCK, UC_PROT_ALL
from unicorn.arm_const import *
import unicorn.arm_const as arm_const_mod

sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', 'iosgpu'))
import kctool as k  # noqa: E402  (reads $KC841)

SWITCH2_VTABLE = 0x80cfc0c8          # AppleAE2I2SSwitch2's vtable entries
ROUTER = 0x804c09a8                  # AppleARMIISSwitch's route()
BASE_INIT = (0x804c1114, 0x804c11da)  # start(): counts, node table, matrix
STUBS = {                            # the kext's stubs into the kernel
    0x804c9bc4: 'IOMalloc', 0x804c9ba4: 'IOFree', 0x804c9c34: 'bzero',
    0x804c9334: 'IOLockAlloc', 0x804c9354: 'IOLockLock', 0x804c9374: 'IOLockUnlock',
    0x804c99e4: 'OSArray::withCapacity',
}
HEAP, HEAP_SZ = 0x10000000, 0x01000000
STACK, STACK_SZ = 0x20000000, 0x00100000
FAKE = 0x30000000                    # fake objects' vtable target, "return 1"
SENTINEL = 0x30001000                # return address that ends a call

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
        raise SystemExit('alloc of %#x at pc %#x (heap at %#x)' % (n, uc.reg_read(UC_ARM_REG_PC), heap_top[0]))
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


SHARED = fake_obj()     # what every kernel call returns: they are thousands


def ret(v):
    uc.reg_write(UC_ARM_REG_R0, v & 0xffffffff)
    lr = uc.reg_read(UC_ARM_REG_LR)
    uc.reg_write(UC_ARM_REG_PC, lr)


def on_code(mu, addr, size, _):
    if addr in STUBS:
        name = STUBS[addr]
        r0, r1 = mu.reg_read(UC_ARM_REG_R0), mu.reg_read(UC_ARM_REG_R1)
        if name == 'IOMalloc':
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
    elif 0x80001000 <= addr < 0x80392000:   # kernel proper: not run
        # whatever it would return -- an OSString for a log line, true for
        # setProperty -- a fake object stands in: non-zero, and safe to call
        ret(SHARED)


uc.hook_add(UC_HOOK_CODE, on_code, begin=0x80001000, end=0x80392000)
uc.hook_add(UC_HOOK_CODE, on_code, begin=0x804c9000, end=0x804ca000)
uc.hook_add(UC_HOOK_CODE, on_code, begin=FAKE, end=FAKE + 4)


def call(fn, args=(), stack_args=(), regs=None):
    sp = STACK + STACK_SZ - 0x1000
    sp -= 4 * len(stack_args)
    for i, a in enumerate(stack_args):
        w32(sp + 4 * i, a)
    uc.reg_write(UC_ARM_REG_SP, sp)
    for r, a in zip((UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3), args):
        uc.reg_write(r, a & 0xffffffff)
    for r, a in (regs or {}).items():
        uc.reg_write(r, a)
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


# the object, as AppleAE2I2SSwitch2::start leaves it before calling its parent
obj = alloc(0x100)
mmio = alloc(0x1000)
w32(obj, SWITCH2_VTABLE)
w32(obj + 0x64, fake_obj())
w32(obj + 0x6c, mmio)
w32(obj + 0x70, 4)            # pin-count
w32(obj + 0x74, 3)            # dsp-count
w32(obj + 0x78, 0x20001)      # 0x3fa01ffc, read under Linux
w32(mmio + 0xffc, 0x20001)
# AppleARMIISSwitch::start from its counts to the default routes, once
uc.reg_write(UC_ARM_REG_R11, obj)
uc.reg_write(UC_ARM_REG_R10, fake_obj())
uc.reg_write(UC_ARM_REG_R7, STACK + STACK_SZ - 0x800)
uc.reg_write(UC_ARM_REG_SP, STACK + STACK_SZ - 0x900)
uc.emu_start(BASE_INIT[0] | 1, BASE_INIT[1], count=1_000_000)
n, nregs = u32(obj + 0x50), u32(obj + 0x54)
table = u32(obj + 0x5c)
# +4 of each entry is the node's name as an OSString, which the kernel would
# have made; the router only formats it into a debug log line
for i in range(n):
    w32(table + 12 * i + 4, fake_obj())
TEMPLATE = (bytes(uc.mem_read(obj, 0x100)), bytes(uc.mem_read(table, 12 * n)))


def new_switch():
    """a fresh copy of the started switch: own node table, zeroed matrix and registers"""
    global obj, mmio
    o, t = TEMPLATE
    obj, mmio = alloc(0x100), alloc(0x1000)
    uc.mem_write(obj, o)
    tab = alloc(12 * n)
    uc.mem_write(tab, t)
    w32(obj + 0x5c, tab)
    w32(obj + 0x60, alloc(2 * n * n))
    w32(obj + 0x6c, mmio)
    w32(mmio + 0xffc, 0x20001)
    return [struct.pack('>I', u32(tab + 12 * i)).decode('latin1') for i in range(n)]


names = new_switch()
print('nodes (%d): %s; %d registers' % (n, ' '.join(names), nregs))


def regs():
    return [u32(mmio + 4 * i) for i in range(nregs)]


def route(src, dst, b0, b1, b2, v0, v1, v2):
    r = call(ROUTER, (obj, fourcc(src), fourcc(dst), b0), (v0, b1, v1, b2, v2, 0, 0))
    return r


def show(title, before):
    now = regs()
    print('-- %s' % title)
    for i, v in enumerate(now):
        mark = '' if v == before[i] else '   <- was %08x' % before[i]
        if v or before[i]:
            print('   reg %2d (+0x%02x) = %08x%s' % (i, 4 * i, v, mark))
    return now


DEFAULT = ('dspc', 'dsp0', 1, 1, 0, 1, 1, 0)      # the ADT's default-routes
SCENARIOS = [
    ('speakers as iOS runs them: I2S1 master (p2=2), MCA0 TX slave',
     [('aud1', 'pin1', 3, 0, 0x30, 3, 3, 2), ('mca0', 'pin1', 3, 3, 3, 2, 2, 1)]),
    ('aud1 -> pin1 alone, master', [('aud1', 'pin1', 3, 0, 0x30, 3, 3, 2)]),
    ('aud1 -> pin1 alone, master, TX data out (p2=0x12)', [('aud1', 'pin1', 3, 0, 0x30, 3, 3, 0x12)]),
    ('aud0 -> pin0, master (the codec)', [('aud0', 'pin0', 3, 3, 0x33, 3, 3, 2)]),
    ('aud1 -> pin2, master, TX data out', [('aud1', 'pin2', 3, 0, 0x30, 3, 3, 0x12)]),
    ('mca0 -> pin1 alone', [('mca0', 'pin1', 3, 3, 3, 2, 2, 1)]),
    ('mca0 -> pin1, MCA0 TX as clock master (p2=2)', [('mca0', 'pin1', 3, 3, 3, 2, 2, 2)]),
    ('mca0 -> pin1, MCA0 TX+RX as clock master (p1=3, p2=2)', [('mca0', 'pin1', 3, 3, 3, 3, 3, 2)]),
    ('mca0 -> pin1, master, p1=1 (bit 0 direction)', [('mca0', 'pin1', 3, 3, 3, 1, 1, 2)]),
]
for title, routes in SCENARIOS:
    new_switch()
    route(*DEFAULT)
    rcs = [hex(route(*a)) for a in routes]
    print('-- %s (router: %s)' % (title, ', '.join(rcs)))
    for i, v in enumerate(regs()):
        if v:
            print('   reg %2d (+0x%02x) = %08x' % (i, 4 * i, v))
