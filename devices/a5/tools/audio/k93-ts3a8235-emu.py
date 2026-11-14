#!/usr/bin/env python3
"""What iOS writes to the iPad 2's TS3A8235 headset switch, computed by iOS's own code.

    docker run --rm --user "$(id -u):$(id -g)" -v "$PWD":/cascadia -w /cascadia \
        cascadia-build python3 tools/audio/k93-ts3a8235-emu.py

(from devices/a5; KC841 may point at another kernelcache, the default is the
K93 12H321 one.)

The jack's ground/mic switch (I2C0 0x3b, headset-switch,ts3a8235) is driven
by AppleTS3A8235HeadsetSwitch (com.apple.driver.AppleTS3A8235HeadsetSwitch,
vtable 0x80929068).  Its setState (vt+0x340, 0x80928d24) turns a state word
into two register values (0x80928bfc) and writes them over its I2C nub
(+0x50, vt+0x350 like the codec's).  The headset-detect code
(AppleCSCodecMikey, com.apple.driver.AppleCSEmbeddedAudio) asks for states
1, 2 (| 0x100 when the switch has a slow-ramp-delay: K93's is 50 ms), 3,
0x103 and 0x203.  K93's node has no switch-config (+0x60 = 0).  Each state
is run here on a fresh object with the I2C nub stubbed to log.  Nothing
touches the iPad.
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

VTABLE = 0x80929068
SET_STATE = 0x80928d24
ENCODE = 0x80928bfc
HEAP, STACK, FAKE, TRAMP = 0x10000000, 0x20000000, 0x30000000, 0x3ffff000

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
for a, n in ((HEAP, 0x100000), (STACK, 0x100000), (FAKE, 0x10000), (TRAMP & ~0xfff, 0x1000)):
    uc.mem_map(a, n)
uc.mem_write(TRAMP, bytes.fromhex('00bf00bfe047fee7'))
heap = [HEAP]
log = []


def alloc(n):
    p = heap[0]
    heap[0] = (p + n + 15) & ~15
    uc.mem_write(p, b'\0' * n)
    return p


def u32(a):
    return struct.unpack('<I', uc.mem_read(a, 4))[0]


def w32(a, v):
    uc.mem_write(a, struct.pack('<I', v & 0xffffffff))


def ret(v):
    uc.reg_write(UC_ARM_REG_R0, v)
    uc.reg_write(UC_ARM_REG_PC, uc.reg_read(UC_ARM_REG_LR))


def on_fake(mu, addr, size, _):
    slot = addr - FAKE
    r = [mu.reg_read(x) for x in (UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3)]
    sp = mu.reg_read(UC_ARM_REG_SP)
    if slot == 0x350 and r[0] == 2:              # write: op 2, address buffer, data buffer
        reg = uc.mem_read(r[2], r[1])[0]
        val = uc.mem_read(u32(sp + 4), u32(sp))[0]
        log.append('W  reg %#04x = %#04x' % (reg, val))
    else:
        log.append('call nub+%#x' % slot)
    ret(0)


def on_kernel(mu, addr, size, _):
    if addr == 0x802e4658:
        log.append('sleep %d ms' % mu.reg_read(UC_ARM_REG_R0))
    ret(0)


uc.hook_add(UC_HOOK_CODE, on_fake, begin=FAKE, end=FAKE + 0xffff)
uc.hook_add(UC_HOOK_CODE, on_kernel, begin=0x80001000, end=0x80391fff)
nub_vt = alloc(0x400)
for i in range(0x100):
    w32(nub_vt + 4 * i, (FAKE + 4 * i) | 1)


def call(fn, a):
    uc.reg_write(UC_ARM_REG_SP, STACK + 0x100000 - 0x1000)
    for r, v in zip((UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3), a):
        uc.reg_write(r, v & 0xffffffff)
    uc.reg_write(UC_ARM_REG_R12, fn | 1)
    uc.emu_start(TRAMP | 1, TRAMP + 6, count=1_000_000)
    return uc.reg_read(UC_ARM_REG_R0)


def new_switch(prev_state=0, switch_config=0):
    o = alloc(0x80)
    w32(o, VTABLE)
    nub = alloc(0x10)
    w32(nub, nub_vt)
    w32(o + 0x50, nub)
    w32(o + 0x54, 0)             # function-ext_switch: absent on K93
    w32(o + 0x58, prev_state)
    w32(o + 0x5c, 50)            # slow-ramp-delay
    w32(o + 0x60, switch_config)
    return o


print('state -> (reg 2, reg 3) as encoded (0x80928bfc), switch-config 0')
for st in (0, 1, 2, 3, 0x100, 0x101, 0x102, 0x103, 0x201, 0x202, 0x203):
    o = new_switch()
    b = alloc(8)
    call(ENCODE, (o, st, b, b + 4))
    print('  state %#05x: reg2 %#04x reg3 %#04x' % (st, uc.mem_read(b, 1)[0], uc.mem_read(b + 4, 1)[0]))
print('setState (0x80928d24), from state 0:')
for st in (1, 2, 3, 0x101, 0x102, 0x103, 0x203):
    log.clear()
    call(SET_STATE, (new_switch(), st))
    print('  setState(%#05x): %s' % (st, '; '.join(log)))
