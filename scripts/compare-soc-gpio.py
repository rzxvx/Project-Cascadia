#!/usr/bin/env python3
"""Compare actual Linux GPIO callbacks and 12H321 firmware using fake RAM.

The current runtime callbacks are compiled as-is. A legacy-mask control is a
temporary source transform compiled only in a test directory. The tool never
writes the real driver. No device/MMIO backend exists. Register value
comparisons do not establish electrical behavior or authorize any GPIO change.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import shutil
import struct
import subprocess
import tempfile

from unicorn import Uc, UcError, UC_ARCH_ARM, UC_MODE_THUMB, UC_HOOK_CODE, UC_HOOK_MEM_WRITE
from unicorn.arm_const import UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_PC, UC_ARM_REG_SP, UC_ARM_REG_LR

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'patches/files-sandcastle-port/drivers/pinctrl/pinctrl-s5l8940x-gpio.c'
spec = importlib.util.spec_from_file_location('usb_phy_evidence', ROOT / 'scripts/usb-phy-plan.py')
evidence = importlib.util.module_from_spec(spec)
spec.loader.exec_module(evidence)
OBJECT, SHADOW, REGISTERS, LOCK, STACK, END = 0x10000000, 0x10001000, 0x10002000, 0x10003000, 0x20000000, 0x30000FFC


def firmware_mode(data, mode, shadow):
    if hashlib.sha256(data).hexdigest() != evidence.KC_SHA256:
        raise ValueError('kernelcache SHA256 mismatch; fixed addresses refused')
    if isinstance(mode, bool) or not isinstance(mode, int) or mode not in (0, 2, 3, 4, 14, 15):
        raise ValueError('only audited GPIO modes 0/2/3/4/14/15 supported')
    if isinstance(shadow, bool) or not isinstance(shadow, int) or not 0 <= shadow <= 0xFFFF:
        raise ValueError('synthetic firmware shadow must be a 16-bit integer')
    actual = evidence.pic_literal_pointer(data, 0x80DED0D8, 0x80DED0DA, 1) + 8
    if actual != 0x80DF1938:
        raise ValueError('audited GPIOIC instance vtable changed')
    for slot, target in ((0x378, 0x80DED989), (0x37C, 0x80DED991), (0x380, 0x80DED869)):
        if struct.unpack_from('<I', data, evidence.address_offset(data, actual + slot))[0] != target:
            raise ValueError('audited GPIOIC mode/register dispatch changed')
    mu = Uc(UC_ARCH_ARM, UC_MODE_THUMB)
    start, end = 0x80DEC000, 0x80DF6000
    first, last = evidence.address_offset(data, start), evidence.address_offset(data, end - 1)
    if last - first != end - start - 1:
        raise ValueError('firmware mapping not contiguous')
    mu.mem_map(start, end - start)
    mu.mem_write(start, data[first:last + 1])
    for address, size in ((OBJECT, 0x10000), (STACK, 0x10000), (END & ~0xFFF, 0x1000)):
        mu.mem_map(address, size)
    for address, value in ((OBJECT, actual), (OBJECT + 0x80, SHADOW),
                           (OBJECT + 0x64, REGISTERS), (OBJECT + 0x68, LOCK),
                           (REGISTERS + 132 * 4, shadow)):
        mu.mem_write(address, struct.pack('<I', value))
    mu.mem_write(SHADOW + 132 * 2, struct.pack('<H', shadow))
    writes, external_calls = [], []

    def code(machine, address, size, _):
        if address in (0x80DF0874, 0x80DF05A4, 0x80DF05B4):
            external_calls.append(hex(address))
            machine.reg_write(UC_ARM_REG_R0, 0)
            machine.reg_write(UC_ARM_REG_PC, machine.reg_read(UC_ARM_REG_LR))

    def write(machine, access, address, size, value, _):
        if REGISTERS <= address < REGISTERS + 224 * 4:
            if address != REGISTERS + 132 * 4 or size != 4:
                raise RuntimeError('unexpected synthetic GPIO register write')
            writes.append(value & 0xFFFFFFFF)

    mu.hook_add(UC_HOOK_CODE, code)
    mu.hook_add(UC_HOOK_MEM_WRITE, write)
    for register, value in ((UC_ARM_REG_R0, OBJECT), (UC_ARM_REG_R1, 132),
                           (UC_ARM_REG_R2, mode), (UC_ARM_REG_SP, STACK + 0xFF00),
                           (UC_ARM_REG_LR, END | 1)):
        mu.reg_write(register, value)
    mu.emu_start(0x80DED869, END, count=10000)
    if mu.reg_read(UC_ARM_REG_PC) != END:
        raise RuntimeError('firmware did not return within instruction budget')
    return {'mode': mode, 'initial_shadow': shadow, 'writes': writes,
            'final': struct.unpack('<I', mu.mem_read(REGISTERS + 132 * 4, 4))[0],
            'external_calls': external_calls, 'hardware_access': False}


def function(text, name):
    match = re.search(r'static (?:void|int|uint32_t) ' + re.escape(name) + r'\(', text)
    if not match:
        raise ValueError('required Linux callback missing: ' + name)
    start = match.start()
    brace = text.index('{', start)
    depth, end = 1, brace + 1
    while depth and end < len(text):
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    if depth:
        raise ValueError('unterminated Linux callback: ' + name)
    return text[start:end]


CALLBACKS = ('apple_s5l8940x_gpio_set_reg', 'apple_s5l8940x_gpio_refresh_reg',
             'apple_s5l8940x_gpio_get_reg', 'apple_s5l8940x_gpio_init_reg',
             'apple_s5l8940x_gpio_pinmux_enable', 'apple_s5l8940x_gpio_gpio_set',
             'apple_s5l8940x_gpio_gpio_direction_input', 'apple_s5l8940x_gpio_gpio_direction_output')


def linux_harness(text, legacy_control):
    macros = '\n'.join(line for line in text.splitlines() if re.match(r'#define\s+(PINCFG_STAT_|REG_GPIO)', line))
    callbacks = '\n'.join(function(text, name) for name in CALLBACKS)
    # Validate the narrow runtime fix before compiling either version. The
    # control changes only its four clear masks back to their historical form;
    # it is explicitly a mutation control, not the current runtime source.
    old = 'REG_GPIOx_IRQ_MASK | REG_GPIOx_DATA | REG_GPIOx_MUX_MASK,'
    if callbacks.count(old) != 4 or not re.search(
            r'#define\s+REG_GPIOx_MUX_MASK\s+\(3u << 5\)', macros):
        raise ValueError('Linux GPIO clear-mask shape changed; legacy control refused')
    if legacy_control:
        peripheral = old + ' REG_GPIOx_PERIPH'
        if callbacks.count(peripheral) != 1:
            raise ValueError('Linux peripheral clear-mask shape changed; legacy control refused')
        callbacks = callbacks.replace(peripheral,
                'REG_GPIOx_IRQ_MASK | REG_GPIOx_DATA, REG_GPIOx_PERIPH')
        callbacks = callbacks.replace(old,
                'REG_GPIOx_IRQ_MASK | REG_GPIOx_DATA | REG_GPIOx_PERIPH,')
    prefix = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#define __iomem
struct apple_s5l8940x_gpio_pincfg { uint8_t irqtype, stat; };
struct apple_s5l8940x_gpio_pinctrl { void *base; struct apple_s5l8940x_gpio_pincfg *pin_cfgs; };
struct gpio_chip { struct apple_s5l8940x_gpio_pinctrl *data; };
struct pinctrl_dev { struct apple_s5l8940x_gpio_pinctrl *data; };
static uint32_t registers[224], writes[8];
static unsigned count;
static uint32_t readl(void *address) {
 uintptr_t offset=(unsigned char *)address-(unsigned char *)registers;
 assert(offset < sizeof(registers) && !(offset%4));
 return registers[offset/4];
}
static void writel(uint32_t value, void *address) {
 uintptr_t offset=(unsigned char *)address-(unsigned char *)registers;
 assert(offset == 132*4 && count<8);
 writes[count++]=value; registers[offset/4]=value;
}
static struct apple_s5l8940x_gpio_pinctrl *gpiochip_get_data(struct gpio_chip *chip) { return chip->data; }
static struct apple_s5l8940x_gpio_pinctrl *pinctrl_dev_get_drvdata(struct pinctrl_dev *device) { return device->data; }
'''
    suffix = r'''
int main(int argc, char **argv) {
 assert(argc==3);
 struct apple_s5l8940x_gpio_pincfg configs[224]={{0}};
 struct apple_s5l8940x_gpio_pinctrl control={registers,configs};
 struct gpio_chip chip={&control}; struct pinctrl_dev pinctrl={&control};
 unsigned operation=(unsigned)strtoul(argv[1],NULL,0);
 registers[132]=(uint32_t)strtoul(argv[2],NULL,0);
 apple_s5l8940x_gpio_init_reg(&control,132);
 unsigned initial_stat=configs[132].stat;
 switch(operation) {
 case 0: assert(!apple_s5l8940x_gpio_gpio_direction_output(&chip,132,0)); break;
 case 1: assert(!apple_s5l8940x_gpio_gpio_direction_output(&chip,132,1)); break;
 case 2: apple_s5l8940x_gpio_gpio_set(&chip,132,0); break;
 case 3: apple_s5l8940x_gpio_gpio_set(&chip,132,1); break;
 case 4: assert(!apple_s5l8940x_gpio_gpio_direction_input(&chip,132)); break;
 case 5: assert(!apple_s5l8940x_gpio_pinmux_enable(&pinctrl,1,132)); break;
 case 6: assert(!apple_s5l8940x_gpio_pinmux_enable(&pinctrl,0,132)); break;
 default: abort();
 }
 printf("{\"initial_stat\":%u,\"final_stat\":%u,\"final\":%u,\"writes\":[",initial_stat,configs[132].stat,registers[132]);
 for(unsigned i=0;i<count;i++)printf("%s%u",i?",":"",writes[i]);
 printf("]}\n"); return 0;
}
'''
    return prefix + '\n' + macros + '\n' + callbacks + '\n' + suffix


class LinuxCallbacks:
    def __init__(self, text=None):
        if not shutil.which('cc'):
            raise RuntimeError('requires existing C compiler')
        self.text = SOURCE.read_text() if text is None else text
        self.source_sha256 = hashlib.sha256(self.text.encode()).hexdigest()
        self.directory = tempfile.TemporaryDirectory(prefix='cascadia-soc-gpio-')
        self.binaries = {}
        try:
            for legacy_control in (False, True):
                source = Path(self.directory.name) / ('legacy-mask-control.c' if legacy_control else 'actual-runtime.c')
                binary = source.with_suffix('')
                source.write_text(linux_harness(self.text, legacy_control))
                subprocess.run(['cc', '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                                '-fsanitize=undefined', str(source), '-o', str(binary)], check=True)
                self.binaries[legacy_control] = binary
        except BaseException:
            self.close()
            raise

    def close(self):
        self.directory.cleanup()

    def run(self, operation, initial, legacy_control=False):
        if isinstance(operation, bool) or not isinstance(operation, int) or not 0 <= operation <= 6:
            raise ValueError('unsupported Linux callback operation')
        if isinstance(initial, bool) or not isinstance(initial, int) or not 0 <= initial <= 0xFFFFFFFF:
            raise ValueError('synthetic Linux register must be a 32-bit integer')
        if type(legacy_control) is not bool:
            raise ValueError('legacy control flag must be boolean')
        completed = subprocess.run([str(self.binaries[legacy_control]), str(operation), hex(initial)],
                                   check=True, capture_output=True, text=True)
        result = json.loads(completed.stdout)
        result.update({'operation': operation, 'initial': initial, 'legacy_mask_control': legacy_control,
                       'linux_source_sha256': self.source_sha256, 'hardware_access': False})
        return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('kernelcache', type=Path)
    args = parser.parse_args()
    callbacks = None
    try:
        data = args.kernelcache.read_bytes()
        callbacks = LinuxCallbacks()
        rows = []
        for operation, mode, initial in ((0, 14, 0x2D0), (1, 15, 0x2D0),
                                         (2, 14, 0x2D2), (3, 15, 0x2D2),
                                         (4, 0, 0x2D0), (5, 2, 0x2D0), (4, 0, 0x2B0)):
            rows.append({'linux_runtime_source': callbacks.run(operation, initial),
                         'linux_legacy_mask_control': callbacks.run(operation, initial, True),
                         'firmware_oracle': firmware_mode(data, mode, initial)})
    except (OSError, ValueError, RuntimeError, UcError, subprocess.SubprocessError) as error:
        parser.exit(1, f'error: {error}\n')
    finally:
        if callbacks:
            callbacks.close()
    print(json.dumps({'hardware_access': False, 'runtime_source_modified_by_tool': False,
                      'comparisons': rows,
                      'limitations': ['firmware IRQ/locks mocked; Linux fake MMIO',
                                      'legacy-mask control exists only in temporary compiled source',
                                      'input IRQ policy, CFG_DONE and physical behavior are separate questions']}, indent=2))


if __name__ == '__main__':
    main()
