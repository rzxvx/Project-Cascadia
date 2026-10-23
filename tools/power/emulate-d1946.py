#!/usr/bin/env python3
"""Run Apple's 12H321 D1946 current setters against a fake PMU, offline.

Requires unicorn==2.1.4 (available in the Cascadia build image). Input is
the user's locally extracted kernelcache, never downloaded by this program.
The firmware hash is pinned: addresses are not interchangeable between builds.
Only a synthetic 256-byte PMU and emulator RAM are written. No hardware bus,
device node, SSH, root privilege or network is used. This is research evidence,
not a charging driver or instructions for programming a real battery charger.

  python3 tools/power/emulate-d1946.py build/firmware/kernelcache.12H321.macho
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct

from unicorn import Uc, UcError, UC_ARCH_ARM, UC_MODE_THUMB, UC_HOOK_CODE
from unicorn.arm_const import (UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2,
                               UC_ARM_REG_R3, UC_ARM_REG_SP, UC_ARM_REG_LR,
                               UC_ARM_REG_PC)

EXPECTED_SHA256 = 'db1ddc8343aa9debc00e0a083fba59d9f555722db60957dc333b854fac21dd2c'
ADT_SHA256 = '5c346586116de1bb32606acb27cad3158bcaa8a1318a10ee9631de09884cfd72'
KLO, KHI = 0x80ecb000, 0x80ed8000
SOURCE, PMU, PMU_VTABLE = 0x10000000, 0x10001000, 0x10002000
STACK, TRAPS, END = 0x20000000, 0x30000000, 0x30000ffc
READ, WRITE = TRAPS, TRAPS + 4

# AppleD1946PMUPowerSource::init copies the first five bytes of p105's
# charger/ichg-tbat-max, whose complete property is 00003f3f3f000000.
# Temperature bucket meanings have not been established; do not alter them.
P105_THERMAL_LIMITS = bytes.fromhex('00003f3f3f')


def segments(data):
    if len(data) < 28 or struct.unpack_from('<I', data)[0] != 0xfeedface:
        raise ValueError('expected a 32-bit Mach-O kernelcache')
    off, out = 28, []
    for _ in range(struct.unpack_from('<I', data, 16)[0]):
        cmd, size = struct.unpack_from('<II', data, off)
        if cmd == 1:
            out.append(struct.unpack_from('<4I', data, off + 24))
        off += size
    return out


class Experiment:
    def __init__(self, data, initial_charge_control=0xbf, thermal_limits=P105_THERMAL_LIMITS,
                 fail_transfer=None):
        self.mu = Uc(UC_ARCH_ARM, UC_MODE_THUMB)
        self.transactions = []
        self.fail_transfer = fail_transfer
        self.registers = bytearray(256)
        # These starting values are synthetic, deliberately include unrelated
        # bits to check preservation, and are not claims about the real iPad.
        self.registers[0x1b] = 0xa1
        self.registers[0x1c] = 0x13
        self.registers[0x3d] = 0x54
        self.registers[0x3f] = initial_charge_control
        self.mu.mem_map(KLO, KHI - KLO)
        for va, vsize, fileoff, filesize in segments(data):
            start, end = max(va, KLO), min(va + filesize, KHI)
            if start < end:
                self.mu.mem_write(start, data[fileoff + start - va:fileoff + end - va])
        for start, size in ((SOURCE, 0x10000), (STACK, 0x10000), (TRAPS, 0x1000)):
            self.mu.mem_map(start, size)
        self.w32(SOURCE, 0x80ed3c68)
        self.w32(SOURCE + 0x18, PMU)
        self.mu.mem_write(SOURCE + 0x1d, bytes([initial_charge_control & 0x3f]))
        self.mu.mem_write(SOURCE + 0x24, bytes(thermal_limits))
        # Null input calibration selects Apple's no-calibration fallback. The
        # measured device calibration cannot be guessed from static firmware.
        self.w32(SOURCE + 0x20, 0)
        self.w32(PMU, PMU_VTABLE)
        self.w32(PMU_VTABLE + 0x378, READ | 1)
        self.w32(PMU_VTABLE + 0x37c, WRITE | 1)
        self.mu.mem_write(TRAPS, b'\x70\x47' * 8)
        self.mu.hook_add(UC_HOOK_CODE, self.io, begin=TRAPS, end=TRAPS + 8)

    def w32(self, address, value):
        self.mu.mem_write(address, struct.pack('<I', value))

    def io(self, mu, address, size, user):
        if address not in (READ, WRITE):
            raise RuntimeError(f'unexpected stub call 0x{address:x}')
        obj, reg, buffer, count = [mu.reg_read(r) for r in
                                  (UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3)]
        if obj != PMU or not 0 <= reg < 256 or not 1 <= count <= 256 - reg:
            raise RuntimeError('invalid synthetic PMU transaction')
        if len(self.transactions) == self.fail_transfer:
            self.transactions.append({'direction': 'read' if address == READ else 'write',
                                      'register': f'0x{reg:02x}', 'bytes': None,
                                      'error': '0xe00002c0 (synthetic transfer failure)'})
            mu.reg_write(UC_ARM_REG_R0, 0xe00002c0)
            mu.reg_write(UC_ARM_REG_PC, mu.reg_read(UC_ARM_REG_LR))
            return
        if address == READ:
            value = bytes(self.registers[reg:reg + count])
            mu.mem_write(buffer, value)
            direction = 'read'
        else:
            value = bytes(mu.mem_read(buffer, count))
            self.registers[reg:reg + count] = value
            direction = 'write'
        self.transactions.append({'direction': direction, 'register': f'0x{reg:02x}',
                                  'bytes': value.hex()})
        mu.reg_write(UC_ARM_REG_R0, 0)  # successful I2C transfer, exclusively fake
        mu.reg_write(UC_ARM_REG_PC, mu.reg_read(UC_ARM_REG_LR))

    def call(self, address, arg1=0, arg2=0, arg3=0, stack_args=()):
        stack = STACK + 0xff00
        if len(stack_args) > 16:
            raise ValueError('too many synthetic stack arguments')
        for offset, value in enumerate(stack_args):
            self.w32(stack + offset * 4, value)
        self.mu.reg_write(UC_ARM_REG_SP, stack)
        self.mu.reg_write(UC_ARM_REG_LR, END | 1)
        for register, value in zip((UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3),
                                   (SOURCE, arg1, arg2, arg3)):
            self.mu.reg_write(register, value)
        self.mu.emu_start(address | 1, END, count=20000)
        if self.mu.reg_read(UC_ARM_REG_PC) != END:
            raise RuntimeError('instruction budget exhausted before returning')
        return self.mu.reg_read(UC_ARM_REG_R0)

    def status(self, status_bytes):
        """Execute the status getter against synthetic registers 0x0a..0x0f.

        The three boolean API names are still unproven; retain argument names.
        Only the reason string has explicit firmware evidence. No event/read-
        clear behavior or real PMU safety is simulated by this byte array.
        """
        if len(status_bytes) != 6:
            raise ValueError('exactly six synthetic status bytes required')
        self.registers[0x0a:0x10] = bytes(status_bytes)
        output = SOURCE + 0x3000
        self.mu.mem_write(output, bytes(16))
        result = self.call(0x80ed0c24, output, output + 4, output + 8,
                           stack_args=(output + 12,))
        raw = bytes(self.mu.mem_read(output, 16))
        reason = struct.unpack_from('<I', raw, 12)[0]
        if reason:
            if reason != 0x80ed2be6:
                raise RuntimeError('unexpected synthetic status reason pointer')
            reason = 'HighOrLowTemperature'
        return {'synthetic_status_hex': bytes(status_bytes).hex(), 'return_code': result,
                'arg1_boolean': raw[0], 'arg2_boolean': raw[4], 'arg3_boolean': raw[8],
                'reason': reason or None, 'transactions': self.transactions}

    def criteria(self, status_bytes, qualifier=False):
        """Execute actual source criteria getter with fake OSSet allocation.

        Only USB/FireWire symbolic membership is proved by this method.
        It does not detect Tristar, emulate attach IRQs or negotiate USB power.
        """
        if len(status_bytes) != 6:
            raise ValueError('exactly six synthetic status bytes required')
        members = []
        set_object, set_vtable, set_add = SOURCE + 0x4000, SOURCE + 0x5000, TRAPS + 0x10
        if not hasattr(self, '_criteria_hooks'):
            self.mu.mem_map(0x804da000, 0x4000)
            # Imported symbol slots interned by AppleARMPlatform0x804c8558.
            self.w32(0x804db00c, SOURCE + 0x8000)
            self.w32(0x804db010, SOURCE + 0x8004)
            self.w32(set_object, set_vtable)
            self.w32(set_vtable + 0x74, set_add | 1)
            self.mu.mem_write(set_add, b'\x70\x47')

            def set_api(mu, address, size, user):
                if address == 0x80ed1f9c:
                    if mu.reg_read(UC_ARM_REG_R0) != 1:
                        raise RuntimeError('unexpected OSSet capacity')
                    mu.reg_write(UC_ARM_REG_R0, set_object)
                elif address == set_add:
                    if mu.reg_read(UC_ARM_REG_R0) != set_object:
                        raise RuntimeError('unexpected OSSet object')
                    symbol = mu.reg_read(UC_ARM_REG_R1)
                    names = {SOURCE + 0x8000: 'USB', SOURCE + 0x8004: 'FireWire'}
                    if symbol not in names:
                        raise RuntimeError('unexpected OSSet symbol')
                    self._criteria_members.append(names[symbol])
                    mu.reg_write(UC_ARM_REG_R0, 1)
                else:
                    raise RuntimeError('unexpected OSSet stub')
                mu.reg_write(UC_ARM_REG_PC, mu.reg_read(UC_ARM_REG_LR))

            self._criteria_hooks = [self.mu.hook_add(UC_HOOK_CODE, set_api, begin=a, end=a)
                                    for a in (0x80ed1f9c, set_add)]
        self._criteria_members = members
        self.registers[0x0a:0x10] = bytes(status_bytes)
        result = self.call(0x80ed0b70, int(qualifier))
        if result != set_object:
            raise RuntimeError('unexpected source-criteria return object')
        return {'synthetic_status_hex': bytes(status_bytes).hex(), 'qualifier': bool(qualifier),
                'criteria': list(members), 'transactions': list(self.transactions)}


def experiment(data, kind, requested, control=0xbf):
    e = Experiment(data, initial_charge_control=control)
    out = SOURCE + 0x3000
    e.w32(out, 0xdeadbeef)
    if kind == 'charge':
        result = e.call(0x80ed0768, 1, requested, out)
        actual = None
    elif kind == 'input':
        result = e.call(0x80ed086c, requested, out, 0)
        actual = e.call(0x80ed0a98, e.registers[0x3d])
    else:
        raise ValueError(kind)
    # These fields describe fake transactions and arithmetic, not measured
    # battery/input current and not a hardware-validated charging state.
    return {'kind': kind, 'requested_mA': requested, 'synthetic_initial_3f': f'0x{control:02x}',
            'return_code': result, 'output_pointer': bytes(e.mu.mem_read(out, 4)).hex(),
            'computed_input_mA': actual,
            'final_registers': {f'0x{r:02x}': f'0x{e.registers[r]:02x}'
                                for r in (0x1b, 0x1c, 0x3d, 0x3f, 0x4b, 0x4d, 0x4f, 0x51, 0x53)},
            'transactions': e.transactions}


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('kernelcache', type=Path)
    args = p.parse_args()
    data = args.kernelcache.read_bytes()
    if hashlib.sha256(data).hexdigest() != EXPECTED_SHA256:
        p.error('kernelcache hash does not match p105 12H321; refusing fixed addresses')
    currents = (0, 1, 39, 40, 74, 75, 99, 100, 500, 1000, 1450, 1499, 1500,
                2100, 2519, 2520, 2521, 3262, 5000)
    output = {'format': 'p105-d1946-emulation-v1', 'kernelcache_sha256': EXPECTED_SHA256,
              'adt_sha256': ADT_SHA256, 'thermal_limits_hex': P105_THERMAL_LIMITS.hex(),
              'hardware_access': False, 'input_calibration': 'synthetic absent/fallback',
              'limitations': ['not a complete charger policy or hardware simulation',
                              'register semantics require device validation',
                              'no analog protection, attach detection or IRQ emulation'],
              'experiments': [], 'status_experiments': [], 'criteria_experiments': []}
    try:
        for kind in ('charge', 'input'):
            for current in currents:
                output['experiments'].append(experiment(data, kind, current))
        # A smaller synthetic hardware ceiling tests clamping and preservation
        # independently of the 2520 mA ceiling used above.
        output['experiments'].append(experiment(data, 'charge', 2520, control=0x9f))
        for mask in (0, 1, 2, 4, 8, 16, 32, 64, 128, 255):
            output['status_experiments'].append(Experiment(data).status((0, mask, 0, 0, 0, 0)))
        for usb_status in (0, 8, 64, 72):
            for qualifier in (False, True):
                output['criteria_experiments'].append(Experiment(data).criteria(
                    (usb_status, 0, 0, 0, 0, 0), qualifier))
    except (UcError, RuntimeError) as exc:
        p.error(f'emulation failed: {exc}')
    print(json.dumps(output, indent=2))


if __name__ == '__main__':
    main()
