#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Actual bank0 initializer and optional Linux conf; no hardware or live settings."""
from pathlib import Path
import argparse
import hashlib
import importlib.util
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
PARENT = ROOT / 'experiments/p105-parent-pmu'
GUARD = '''\t/* i2c-dev can change adapter retries without taking the segment lock.
\t * Refuse both builtin and modular access, including forced builds. */
#if defined(CONFIG_I2C_CHARDEV) || defined(CONFIG_I2C_CHARDEV_MODULE)
\treturn -EOPNOTSUPP;
#endif
'''
STUB = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#define IS_ERR_OR_NULL(p) (!(p) || (uintptr_t)(p) >= (uintptr_t)-4095)
#define BANK0_ADDR 0x3c
#define BANK0_RUNNING 0
#define I2C_FUNC_I2C 1
struct i2c_adapter { void *algo; unsigned int retries; };
struct i2c_client { struct i2c_adapter *adapter; unsigned int addr, flags; };
struct d1946_bank0_bus { struct i2c_client *client; int map_lock, state, fault, off_ready; };
static unsigned int mutations;
static bool i2c_check_functionality(struct i2c_adapter *a, int mask)
{ assert(mask == I2C_FUNC_I2C); return a->algo != NULL; }
static void *memchr_inv(const void *p, int c, size_t n)
{ const unsigned char *s = p; for (size_t i=0;i<n;i++) if(s[i] != c) return (void *)(s+i); return NULL; }
static void mutex_init(int *p) { mutations++; *p=1; }
static void atomic_set(int *p, int v) { mutations++; *p=v; }
'''
MAIN = r'''
int main(void)
{
    struct d1946_bank0_bus bus = {0};
    struct i2c_adapter adapter = {.algo = &bus};
    struct i2c_client client = {.adapter = &adapter, .addr = BANK0_ADDR};
    int ret = d1946_bank0_init(&bus, &client);
    if (EXPECT_BLOCK) {
        if (ret != -EOPNOTSUPP) return 23;
        assert(!mutations && !memchr_inv(&bus, 0, sizeof(bus)));
    } else {
        assert(!ret && mutations == 4 && bus.client == &client);
    }
    return 0;
}
'''

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--conf', type=Path, help='existing Linux scripts/kconfig/conf; no build performed')
    args = parser.parse_args()
    spec = importlib.util.spec_from_file_location('binding_fixture', ROOT/'tests/test-experimental-parent-current-binding.py')
    helper = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(helper)
    current = helper.fn((PARENT/'apple-pmu-d1946-bank0.c').read_text(), 'd1946_bank0_init')
    assert current.count(GUARD) == 1
    historical = current.replace(GUARD, '')
    assert hashlib.sha256(historical.encode()).hexdigest() == '00b534d05597e48dd073b19b7d634eca46b066d3e20bf8cf5b1fb8682db2e14a'
    conf = args.conf.resolve() if args.conf else None
    with tempfile.TemporaryDirectory() as td:
        d = Path(td)
        for label, code, settings in [('fixed', current, ['n','y','m']), ('historical', historical, ['y','m'])]:
            for option in settings:
                source = d/'init.c'; source.write_text(STUB + code + MAIN)
                exe = d/'init-test'
                cmd = ['gcc','-std=gnu11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-pie','-no-pie',f'-DEXPECT_BLOCK={int(option != "n")}',str(source),'-o',str(exe)]
                if option != 'n': cmd.insert(1, '-D' + ('CONFIG_I2C_CHARDEV' if option == 'y' else 'CONFIG_I2C_CHARDEV_MODULE') + '=1')
                subprocess.run(cmd, check=True)
                result = subprocess.run([str(exe)], env={**os.environ,'ASAN_OPTIONS':'detect_leaks=1:abort_on_error=1','UBSAN_OPTIONS':'halt_on_error=1'})
                expected = 23 if label == 'historical' else 0
                assert result.returncode == expected, (label, option, result.returncode)
                print(f'PASS actual init {label} char={option} expected_exit={expected}', flush=True)
        if conf is None:
            print('SKIP Kconfig execution: supply --conf from an existing Linux build')
            return
        fragment = (PARENT/'Kconfig.fragment').read_text()
        assert fragment.count('depends on I2C_CHARDEV = n') == 1
        header = '''mainmenu "Parent transport fixture"
config MODULES
 bool "Modules"
 modules
 default y
config MFD_APPLE_PMU_I2C
 bool
 default y
config I2C_CHARDEV
 tristate "I2C character device"
'''
        # Real conf consumes the actual fragment. These historical/wrong guards
        # are explicit negative controls, not substitutes for Kconfig semantics.
        variants = [('fixed',fragment,['n','y','m']),('historical',fragment.replace('\tdepends on I2C_CHARDEV = n\n',''),['y','m']),('wrong_not',fragment.replace('I2C_CHARDEV = n','!I2C_CHARDEV'),['m'])]
        for label, text, settings in variants:
            (d/'Kconfig').write_text(header + text)
            for option in settings:
                config=d/'.config';config.write_text(f'CONFIG_MODULES=y\nCONFIG_I2C_CHARDEV={option}\nCONFIG_APPLE_PMU_D1946_BANK0_EXPERIMENT=y\n')
                subprocess.run([str(conf),'--olddefconfig',str(d/'Kconfig')],cwd=d,env={**os.environ,'KCONFIG_CONFIG':str(config)},check=True,stdout=subprocess.DEVNULL)
                lines=config.read_text().splitlines(); enabled='CONFIG_APPLE_PMU_D1946_BANK0_EXPERIMENT=y' in lines
                actual_char=next((x for x in lines if x.startswith('CONFIG_I2C_CHARDEV=')), '# CONFIG_I2C_CHARDEV is not set')
                assert actual_char == (f'CONFIG_I2C_CHARDEV={option}' if option!='n' else '# CONFIG_I2C_CHARDEV is not set'), lines
                assert enabled == (option=='n' or label!='fixed'), (label,option,lines)
                print(f'PASS real Kconfig {label} char={option} experiment={enabled}',flush=True)

if __name__ == '__main__':
    main()
