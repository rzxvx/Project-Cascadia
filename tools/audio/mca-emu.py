#!/usr/bin/env python3
"""What iOS writes to an MCA, computed by iOS's own code.

    KC841=build/firmware/kernelcache.12H321.macho python3 tools/audio/mca-emu.py

AppleAE2MCA (com.apple.driver.AppleAE2Audio, 12H321) programs an MCA port
from a 32-byte format descriptor the audio framework fills in (the ADT's
audio-data children carry templates of it in `reg`).  Its configure
(0x80cfa9a4, vtable 0x80cfc868 +0x348) turns the descriptor into register
values through a helper in AppleARMIISAudio (0x80cfbd4c's target) and a few
divisions; start (0x80cfb08c, +0x350) then enables TX/RX.  Reading all of
that by hand invites mistakes, so it is run instead, as
tools/audio/i2sswitch-emu.py runs the i2s-switch router.

The kernelcache is mapped into unicorn; the MCA object is built with the
fields its start() leaves (+0x84 the register window, +0x88 mca-version and
+0x8c busy-dma both absent on this SoC, the DMA channels and the mclk/route/
pclk platform functions as stand-in objects).  Every register access and
every call into a stand-in is printed, so the order is visible as well as
the values.  Nothing touches the iPad.
"""
import os
import struct
import sys

from unicorn import Uc, UcError, UC_ARCH_ARM, UC_MODE_THUMB, UC_HOOK_CODE, UC_HOOK_MEM_READ, UC_HOOK_MEM_WRITE, UC_PROT_ALL
from unicorn.arm_const import *

sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', 'iosgpu'))
import kctool as k  # noqa: E402  (reads $KC841)

CONFIGURE = 0x80cfa9a4
START = 0x80cfb08c
HEAP, HEAP_SZ = 0x10000000, 0x01000000
STACK, STACK_SZ = 0x20000000, 0x00100000
FAKE, FAKE_SZ = 0x30000000, 0x00100000   # stand-in methods: object n, slot s
MMIO = 0x40000000                        # the MCA's register window
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


fakes = []


def fake_obj(name):
    """an object whose every virtual method lands in FAKE + n*0x1000 + slot"""
    n = len(fakes)
    fakes.append(name)
    vt = alloc(0x400)
    for i in range(0x100):
        w32(vt + 4 * i, (FAKE + n * 0x1000 + 4 * i) | 1)
    o = alloc(0x40)
    w32(o, vt)
    return o


log = []


def on_fake(mu, addr, size, _):
    n, slot = (addr - FAKE) >> 12, (addr - FAKE) & 0xfff
    r = [mu.reg_read(x) for x in (UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3)]
    extra = ''
    if fakes[n] == 'mclk' and slot == 0x3c:      # callFunction(&hz, ...)
        extra = '  hz=%d' % u32(r[1])
    log.append('call %s+%#x(%#x, %#x, %#x)%s' % (fakes[n], slot, r[1], r[2], r[3], extra))
    mu.reg_write(UC_ARM_REG_R0, 1)
    mu.reg_write(UC_ARM_REG_PC, mu.reg_read(UC_ARM_REG_LR))


def on_kernel(mu, addr, size, _):
    lr = mu.reg_read(UC_ARM_REG_LR)
    log.append('kernel %#x from %#x' % (addr, lr))
    mu.reg_write(UC_ARM_REG_R0, 0)
    mu.reg_write(UC_ARM_REG_PC, lr)


mmio_state = {}


def on_read(mu, access, addr, size, value, _):
    off = addr - MMIO
    v = mmio_state.get(off, 0)
    # busy bits the driver waits on clear at once
    if off in (0x44, 0x48):
        v = 0
    w32(addr, v)
    log.append('  read  +%#04x -> %08x' % (off, v))


def on_write(mu, access, addr, size, value, _):
    off = addr - MMIO
    mmio_state[off] = value & 0xffffffff
    log.append('  write +%#04x = %08x' % (off, value & 0xffffffff))


ROUTE_CALL = 0x804c0964                  # AppleARMIISSwitch route function


def on_route(mu, addr, size, _):
    r = [mu.reg_read(x) for x in (UC_ARM_REG_R1, UC_ARM_REG_R2)]
    log.append('route(directions %#x, mode %#x)' % (r[0], r[1]))
    mu.reg_write(UC_ARM_REG_R0, 0)
    mu.reg_write(UC_ARM_REG_PC, mu.reg_read(UC_ARM_REG_LR))


uc.hook_add(UC_HOOK_CODE, on_fake, begin=FAKE, end=FAKE + FAKE_SZ - 1)
uc.hook_add(UC_HOOK_CODE, on_route, begin=ROUTE_CALL, end=ROUTE_CALL)
uc.hook_add(UC_HOOK_CODE, on_kernel, begin=0x80001000, end=0x80392000)
uc.hook_add(UC_HOOK_MEM_READ, on_read, begin=MMIO, end=MMIO + 0xfff)
uc.hook_add(UC_HOOK_MEM_WRITE, on_write, begin=MMIO, end=MMIO + 0xfff)


TRAMP = SENTINEL                         # nop; nop; blx r12; b .
uc.mem_write(TRAMP, bytes.fromhex('00bf00bfe047fee7'))


def call(fn, args=()):
    # through a trampoline: a second emu_start skips the first instruction
    # it runs, and that must not be the function's push
    uc.reg_write(UC_ARM_REG_SP, STACK + STACK_SZ - 0x1000)
    for r, a in zip((UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3), args):
        uc.reg_write(r, a & 0xffffffff)
    uc.reg_write(UC_ARM_REG_R12, fn | 1)
    try:
        uc.emu_start(TRAMP | 1, TRAMP + 6, count=2_000_000)
    except UcError as e:
        print('\n'.join(log))
        sys.exit('emulation stopped: %s at pc %#x lr %#x' % (e, uc.reg_read(UC_ARM_REG_PC), uc.reg_read(UC_ARM_REG_LR)))
    return uc.reg_read(UC_ARM_REG_R0)


def new_mca():
    obj = alloc(0x100)
    w32(obj, 0x80cfc868)                 # AppleAE2MCA's vtable
    w32(obj + 0x7c, fake_obj('provider'))
    w32(obj + 0x84, MMIO)
    w32(obj + 0x88, 0)                   # mca-version: absent
    w32(obj + 0x8c, 0)                   # busy-dma: absent
    w32(obj + 0x94, 0)                   # no second register window
    w32(obj + 0x98, fake_obj('irq'))
    w32(obj + 0x9c, fake_obj('txdma'))
    w32(obj + 0xa0, fake_obj('rxdma'))
    w32(obj + 0xa4, fake_obj('mclk'))    # function-mclk_frequency (NCOf)
    w32(obj + 0xc8, fake_obj('pclk'))    # function-pclk_gate (advW)
    w32(obj + 0xcc, fake_obj('route'))   # function-i2s_route
    return obj


def desc(master, slots, slot_bits, tx_mask, rx_mask, txch, rxch, txbits, rxbits,
         rate_khz=48, flags1=0, mode=0, flags3=0, mclk=0, bpf=0, extra=0x10):
    d = bytearray(32)
    d[0] = master
    d[1] = flags1
    d[2] = mode
    d[3] = flags3
    d[4] = slots
    d[5] = slot_bits
    d[6] = 1
    d[7] = 0
    struct.pack_into('<I', d, 8, mclk)
    struct.pack_into('<HH', d, 0xc, bpf, extra)
    struct.pack_into('<HH', d, 0x10, rate_khz, 1)
    struct.pack_into('<II', d, 0x14, tx_mask, rx_mask)
    d[0x1c], d[0x1d], d[0x1e], d[0x1f] = txch, rxch, txbits, rxbits
    return bytes(d)


def run(title, d):
    global mmio_state
    mmio_state = {}
    log.clear()
    obj = new_mca()
    p = alloc(32)
    uc.mem_write(p, d)
    rc_c = call(CONFIGURE, (obj, p))
    log.append('-- start(TX)')
    w32(obj + 0xd0, 0)
    rc_s = call(START, (obj, 2))
    print('== %s\n   descriptor %s  configure -> %#x, start -> %#x' % (title, d.hex(' '), rc_c, rc_s))
    for line in log:
        print('   ' + line)
    print('   final: ' + ' '.join('+%02x=%08x' % (o, v) for o, v in sorted(mmio_state.items())))


# the ADT's own template for the speakers (mca0/audio-speaker0 reg), as-is
ADT_SPK = bytes.fromhex('00120000' '08000100' '00000000' '00001000' '30000100' '03000000' 'ff000000' '02081010')
run('ADT speaker template (slave, 8 x 16-bit slots, 48 kHz)', ADT_SPK)
run('same, as clock master', bytes([ADT_SPK[0] | 1]) + ADT_SPK[1:])
run('master, 2 x 32-bit slots, 16-bit samples', desc(1, 2, 0, 3, 3, 2, 2, 16, 16))
run('master, 2 x 32-bit slots, 32-bit samples', desc(1, 2, 0, 3, 3, 2, 2, 32, 32))
run('master, 2 slots, flags1 0x12 like the ADT', desc(1, 2, 0, 3, 3, 2, 2, 16, 16, flags1=0x12))
run('master, 2 x 32-bit slots, 32-bit samples, flags1 0x12', desc(1, 2, 0, 3, 3, 2, 2, 32, 32, flags1=0x12))
run('master, 2 slots, slot_bits 32, 16-bit samples, flags1 0x12', desc(1, 2, 32, 3, 3, 2, 2, 16, 16, flags1=0x12))
