#!/usr/bin/env python3
"""Original platform continuation/getProvider in strict process-local Fake RAM."""
import hashlib
import json
from pathlib import Path
import struct
import sys
import time
from unicorn import Uc, UC_ARCH_ARM, UC_MODE_ARM, UC_HOOK_CODE, UC_HOOK_MEM_WRITE
from unicorn.arm_const import *
from capstone import Cs, CS_ARCH_ARM, CS_MODE_THUMB
sys.path.insert(0, '/repo/tools/nand')
from ppn_12h321_inventory import segments

RAM = 0x10000000
OBJ, KEY, KEYVT, PROVIDER, PROVT, OUT = [RAM + n for n in [0x1000, 0x2000, 0x3000, 0x4000, 0x5000, 0x6000]]
STACK, STOP, COMPARE, PARENT_LOOKUP, DELEGATE = [RAM + n for n in [0x18000, 0x100, 0x200, 0x220, 0x240]]
REGS = [UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3]
profiles = [
    dict(name='12H321', path='/repo/build/firmware/kernelcache.12H321.macho', sha='db1ddc8343aa9debc00e0a083fba59d9f555722db60957dc333b854fac21dd2c',
         entry=0x80f418cc, end=0x80f41934, vtable=0x80f42088, ptr=0x80f4204c, parent=0x802ed664, parent_end=0x802ed696,
         getter=0x802ed368, getter_end=0x802ed3b0, gen=0x803f5d90, sentinel=0x803f5d84, plane=0x80405500, keyglob=0x80405580,
         require=0x80f41cd4, coherent_string=0x80f41f9a),
    dict(name='13G36', path='/references/p105-ios935-official/kernelcache.13G36.macho', sha='8eae16f060a71d4f3790385951588f8198d0f21cdb74146f9750941a3d764125',
         entry=0x811b48a8, end=0x811b4910, vtable=0x811b507c, ptr=0x811b504c, parent=0x80333c1c, parent_end=0x80333c4e,
         getter=0x8033390c, getter_end=0x80333950, gen=0x80458738, sentinel=0x8045872c, plane=0x80412490, keyglob=0x80412844,
         require=0x811b4cb0, coherent_string=0x811b4f99),
]
rows = []
for profile in profiles:
    data = Path(profile['path']).read_bytes()
    assert hashlib.sha256(data).hexdigest() == profile['sha']
    segs = segments(data, 0)
    def offset(address):
        found = [s['offset'] + address - s['address'] for s in segs if s['address'] <= address < s['address'] + s['file_size']]
        assert len(found) == 1
        return found[0]
    def word(address):
        return struct.unpack_from('<I', data, offset(address))[0]
    assert word(word(profile['ptr']) + 0x1f0) & ~1 == profile['parent']
    assert word(profile['vtable'] + 0x1cc) & ~1 == profile['getter']
    md = Cs(CS_ARCH_ARM, CS_MODE_THUMB)
    def exact(address, mnemonic, operands):
        ins = list(md.disasm(data[offset(address):offset(address) + 4], address, count=1))[0]
        assert (ins.mnemonic, ins.op_str) == (mnemonic, operands)
    if profile['name'] == '12H321':
        exact(0x80f41918, 'ldr', 'r0, [pc, #0x28]')
        exact(0x80f41920, 'add', 'r0, pc')
        assert word(0x80f41944) + 0x80f41924 == profile['ptr']
        exact(0x802ed376, 'ldr', 'r0, [r4, #0x1c]')
        exact(0x802ed3a8, 'dmb', 'ish')
        assert 0x107a44 + 0x802ed37c + 0xfd0 == profile['gen']
    else:
        exact(0x811b48f4, 'ldr', 'r0, [pc, #0x28]')
        exact(0x811b48fc, 'add', 'r0, pc')
        assert word(0x811b4920) + 0x811b4900 == profile['ptr']
        exact(0x8033391a, 'ldr', 'r0, [r4, #0x1c]')
        exact(0x80333948, 'dmb', 'ish')
        assert 0x124dd0 + 0x80333920 + 0x48 == profile['gen']
    registry_lookup = word(profile['vtable'] + 0xb8) & ~1
    ranges = [(profile['entry'], profile['end']), (profile['parent'], profile['parent_end']),
              (profile['getter'], profile['getter_end'])]
    u = Uc(UC_ARCH_ARM, UC_MODE_ARM)
    u.ctl_set_cpu_model(UC_CPU_ARM_CORTEX_A9)
    pages = set()
    def map_page(address):
        assert 0x80000000 <= address < 0x90000000, 'not a kernel virtual input'
        page = address & ~0xfff
        if page in pages:
            return
        pages.add(page)
        u.mem_map(page, 0x1000)
        try:
            u.mem_write(page, data[offset(page):offset(page) + 0x1000])
        except AssertionError:
            pass  # explicitly synthetic globals only, not MMIO
    for start, end in ranges:
        for address in range(start & ~0xfff, (end + 0xfff) & ~0xfff, 0x1000):
            map_page(address)
    for address in [registry_lookup, profile['vtable'], profile['vtable'] + 0x348, profile['ptr'],
                    word(profile['ptr']) + 0x1f0, profile['gen'], profile['sentinel'],
                    profile['plane'], profile['keyglob']]:
        map_page(address)
    u.mem_map(RAM, 0x20000)
    def put(address, fmt, *values):
        u.mem_write(address, struct.pack(fmt, *values))
    def get(address, fmt):
        return struct.unpack(fmt, u.mem_read(address, struct.calcsize(fmt)))
    trace = []
    instruction_count = 0
    def code(uc, address, size, opaque):
        global instruction_count
        instruction_count += 1
        assert instruction_count <= 600
        assert any(start <= address < end for start, end in ranges), ('unexpected instruction', hex(address))
    def write(uc, kind, address, size, value, opaque):
        assert (STACK - 256 <= address and address + size <= STACK + 128 or
                OBJ + 0x18 <= address and address + size <= OBJ + 0x20 or
                address == OUT and size == 1), ('unexpected write', hex(address), size)
        if OBJ + 0x18 <= address < OBJ + 0x20 or address == OUT:
            trace.append(['write', hex(address), size, value])
    u.hook_add(UC_HOOK_CODE, code)
    u.hook_add(UC_HOOK_MEM_WRITE, write)
    cases = [
        ('quiesce', 1, True, True, True, 0),
        ('quiesce', 0, True, True, False, 0),
        ('quiesce', 0, True, False, False, 0),
        ('quiesce', 0, True, True, True, 0),
        ('quiesce', 0, True, False, True, 0xe00002c7),
        ('quiesce', 0, False, True, True, 0x80000000),
        ('coherent', 1, True, True, True, 0),
        ('coherent', 0, False, False, False, 0),
        ('other', 1, True, True, True, 0x80000000),
        ('other', 1, False, False, True, 0xe00002c7),
        ('other', 0, True, True, False, 0),
        ('other', 0, False, False, False, 0),
    ]
    for key_kind, running, output, cached, provider_present, delegated in cases:
        u.mem_write(OBJ, bytes(0x100))
        put(OBJ, '<I', profile['vtable'])
        put(OBJ + 0x78, '<B', running)
        put(OBJ + 0x18, '<I', PROVIDER if provider_present else 0)
        put(OBJ + 0x1c, '<I', 0x12345678 if cached else 0)
        put(profile['gen'], '<I', 0x12345678)
        put(profile['sentinel'], '<I', 0xf0f0f0f0)
        put(profile['keyglob'], '<I', KEY if key_kind == 'quiesce' else KEY + 0x40)
        put(KEY, '<I', KEYVT)
        put(KEYVT + 0x58, '<I', COMPARE | 1)
        put(PROVIDER, '<I', PROVT)
        put(PROVT + 0x1e8, '<I', DELEGATE | 1)
        put(OUT, '<B', 0xa5)
        trace = []
        instruction_count = 0
        for register in range(UC_ARM_REG_R0, UC_ARM_REG_R12 + 1):
            u.reg_write(register, 0)
        for register, value in zip(REGS, [OBJ, KEY, 1, OUT if output else 0]):
            u.reg_write(register, value)
        u.reg_write(UC_ARM_REG_SP, STACK)
        u.reg_write(UC_ARM_REG_CPSR, 0x33)
        u.reg_write(UC_ARM_REG_LR, STOP | 1)
        pc = profile['entry']
        started = time.monotonic()
        outcome = 'returned'
        for step in range(601):
            assert time.monotonic() - started < 5, 'wall-time bound'
            if pc == STOP:
                break
            args = [u.reg_read(register) for register in REGS]
            if pc == profile['require']:
                trace.append(['require']);outcome = 'require';break
            if pc == COMPARE:
                assert args[0] == KEY and args[1] == profile['coherent_string']
                trace.append(['synthetic-symbol-compare', key_kind == 'coherent'])
                value = int(key_kind == 'coherent')
            elif pc == registry_lookup:
                assert args[0] == OBJ
                trace.append(['synthetic-registry-parent', PROVIDER if provider_present else 0])
                value = PROVIDER if provider_present else 0
            elif pc == DELEGATE:
                assert args[:4] == [PROVIDER, KEY, 1, OUT if output else 0]
                trace.append(['synthetic-provider-function', delegated])
                value = delegated
            else:
                try:
                    u.emu_start(pc | 1, 0, count=1, timeout=1000000)
                except Exception:
                    print('failed original boundary', profile['name'], hex(pc))
                    raise
                pc = u.reg_read(UC_ARM_REG_PC) & ~1
                continue
            pc = u.reg_read(UC_ARM_REG_LR) & ~1
            u.reg_write(UC_ARM_REG_R0, value)
            u.reg_write(UC_ARM_REG_CPSR, u.reg_read(UC_ARM_REG_CPSR) & ~0x0600fc00)
        else:
            raise AssertionError('bounded step cap')
        expected_assert = key_kind == 'quiesce' and running
        if expected_assert:
            assert outcome == 'require' and trace == [['require']]
        else:
            expected = (0 if output else 0xe00002c2) if key_kind == 'coherent' else delegated if provider_present else 0xe00002c7
            assert outcome == 'returned' and u.reg_read(UC_ARM_REG_R0) == expected
            assert get(OUT, '<B')[0] == (1 if key_kind == 'coherent' and output else 0xa5)
            assert get(OBJ + 0x78, '<B')[0] == running
            if key_kind != 'coherent':
                assert get(OBJ + 0x18, '<I')[0] == (PROVIDER if provider_present else 0)
                assert get(OBJ + 0x1c, '<I')[0] == 0x12345678
                assert bool([item for item in trace if item[0] == 'synthetic-registry-parent']) == (not cached)
            else:
                assert not any(item[0].startswith('synthetic-provider') or item[0].startswith('synthetic-registry') for item in trace)
        rows.append(dict(profile=profile['name'], key=key_kind, software_running=running,
                         output_present=output, cached_provider=cached, provider_present=provider_present,
                         synthetic_provider_status=delegated, outcome=outcome, trace=trace,
                         original_instructions=instruction_count, physical_fence='UNPROVEN'))
Path('/out/continuation-traces.json').write_text(json.dumps(dict(result='PASS', cases=len(rows), rows=rows, hardware_calls=0), indent=2) + '\n')
print('PASS', len(rows), 'original full platform/getProvider continuation controls; hardware_calls=0; physical fence UNPROVEN')
