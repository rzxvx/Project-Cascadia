#!/usr/bin/env python3
"""Diagnose real Linux PHY refcounts with real S5L callbacks, offline.

This records the original core error path against exact baseline source.
Passing this historical control does not describe the separately patched core.
Set CASCADIA_LINUX_SOURCE to a local pinned Linux 6.12 tree if build/linux is
absent. No dependency download, hardware access or production source edit.
"""
import hashlib
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
LINUX = Path(os.environ.get('CASCADIA_LINUX_SOURCE', ROOT / 'build/linux'))
CORE = LINUX / 'drivers/phy/phy-core.c'
PINNED_CORE_SHA256 = '259e56ec9a9064cd120ab6f46e1457a5d78e089d56dbf989a0bb51297fe143f8'



LINUX_COMMIT = 'adc218676eef25575469234709c2d87185ca223a'


def baseline_source(relative, expected_hash):
    """Read exact baseline bytes from a pristine tree or its local pinned Git object.

    Candidate tests read separately applied patch output. This function never
    downloads sources or reverses edits in the user's kernel tree.
    """
    data = (LINUX / relative).read_bytes()
    if hashlib.sha256(data).hexdigest() == expected_hash:
        return data
    result = subprocess.run(['git', '-c', 'protocol.allow=never', '-C', str(LINUX), 'show',
                             LINUX_COMMIT + ':' + relative],
                            capture_output=True, check=False)
    if result.returncode or hashlib.sha256(result.stdout).hexdigest() != expected_hash:
        raise ValueError('requires exact pinned Linux 6.12 baseline: ' + relative)
    return result.stdout


def extract(text, declaration):
    start = text.index(declaration + '(')
    brace = text.index('{', start)
    depth, end = 1, brace + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]


def source_harness(off_policy='current'):
    core_bytes = baseline_source('drivers/phy/phy-core.c', PINNED_CORE_SHA256)
    if hashlib.sha256(core_bytes).hexdigest() != PINNED_CORE_SHA256:
        raise ValueError('requires the unmodified pinned Linux 6.12 phy-core.c')
    driver = (ROOT / 'patches/files/drivers/phy/phy-apple-s5l-usb.c').read_text()
    macros = '\n'.join(line for line in driver.splitlines() if re.match(
        r'#define\s+(PMGR_USB_EHCI|PMGR_USB_OHCI0|USBCPLX_HSIC_EN)\b', line))
    if len(macros.splitlines()) != 3:
        raise ValueError('unexpected S5L PHY gate definitions')
    harness = r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define EPROBE_DEFER 517
#define ENOTSUPP 524
#define BIT(n) (1u << (n))
#define IS_ERR(p) ((intptr_t)(p) < 0)
#define PTR_ERR(p) ((int)(intptr_t)(p))
#define ERR_PTR(e) ((void *)(intptr_t)(e))
#define dev_warn(...) ((void)0)
#define dev_info(...) ((void)0)
#define dev_err(...) ((void)0)
#define mutex_lock(p) ((void)(p))
#define mutex_unlock(p) ((void)(p))
/* SOURCE_MACROS */
struct device { int unused; };
struct regulator { unsigned id; };
struct s5l_usbphy {
 void *pmgr, *usbcplx; struct device *dev;
 struct regulator *hsic_supply, *lpo_supply;
 bool hsic_supply_on, lpo_supply_on;
};
struct phy;
struct phy_ops { int (*power_on)(struct phy *); int (*power_off)(struct phy *); };
struct phy {
 struct s5l_usbphy *data; struct regulator *pwr; unsigned power_count;
 const struct phy_ops *ops; int mutex; struct device dev;
};
static struct regulator supplies[2]={{0},{1}};
static int disable_errors[2];
static unsigned enables[2], disables[2], references[2], gates, writes, runtime_refs;
static struct s5l_usbphy *phy_get_drvdata(struct phy *p) { return p->data; }
static struct regulator *devm_regulator_get_optional(struct device *d, const char *name) {
 (void)d; unsigned i=!strcmp(name,"hsic"); assert(i || !strcmp(name,"lpo"));
 return &supplies[i];
}
static int regulator_enable(struct regulator *r) { enables[r->id]++; references[r->id]++; return 0; }
static int regulator_disable(struct regulator *r) {
 disables[r->id]++;
 if(disable_errors[r->id]) return disable_errors[r->id];
 assert(references[r->id]); references[r->id]--; return 0;
}
static void msleep(unsigned n) { assert(n==10); }
static int s5l_pmgr_on(struct s5l_usbphy *p, unsigned gate) {
 (void)p; assert(gate==PMGR_USB_EHCI || gate==PMGR_USB_OHCI0); gates++; return 0;
}
static uint32_t readl(void *p) { return *(uint32_t *)p; }
static void writel(uint32_t v, void *p) { writes++; *(uint32_t *)p=v; }
static int phy_pm_runtime_get_sync(struct phy *p) { (void)p; runtime_refs++; return 0; }
static void phy_pm_runtime_put_sync(struct phy *p) { (void)p; assert(runtime_refs); runtime_refs--; }
static void phy_pm_runtime_put(struct phy *p) { phy_pm_runtime_put_sync(p); }
'''.replace('/* SOURCE_MACROS */', macros)
    harness += '\n'.join(extract(driver, 'static int ' + name) for name in (
        's5l_supply_disable', 's5l_hostphy_power_on'))
    off = extract(driver, 'static int s5l_hostphy_power_off')
    if off_policy == 'historical':
        # Compile the exact archived callback; do not maintain a handwritten
        # historical implementation alongside the actual production source.
        repository = (ROOT / 'patches/files/drivers/phy/phy-apple-s5l-usb.c').resolve().parents[4]
        result = subprocess.run(['git', '-c', 'protocol.allow=never', '-C', str(repository), 'show',
            'a9bf28f20e5ce908fd5805e7d58b3ce9d9f0e4cf:patches/files/drivers/phy/phy-apple-s5l-usb.c'],
            capture_output=True, check=False)
        expected = '116636c6d8cf4cec10a7b9f02154e9084a77894e254e11204f2a7c25a01718f1'
        if result.returncode or hashlib.sha256(result.stdout).hexdigest() != expected:
            raise ValueError('requires exact archived Cascadia PHY baseline')
        off = extract(result.stdout.decode(), 'static int s5l_hostphy_power_off')
    elif off_policy == 'hidden_error_control':
        checked = 'return ret ? ret : lpo_ret;'
        if checked not in off:
            raise ValueError('power-off implementation changed; review temporary control')
        off = off.replace(checked, '(void)ret; (void)lpo_ret; return 0;')
    elif off_policy != 'current':
        raise ValueError('unknown offline policy')
    harness += off
    harness += '\n'.join(extract(core_bytes.decode(), 'int ' + name) for name in (
        'phy_power_on', 'phy_power_off'))
    harness += r'''
static const struct phy_ops ops={s5l_hostphy_power_on,s5l_hostphy_power_off};
static void reset(struct phy *phy, struct s5l_usbphy *p, uint32_t *complex) {
 memset(p,0,sizeof(*p)); memset(phy,0,sizeof(*phy)); *complex=0;
 p->usbcplx=complex; phy->data=p; phy->ops=&ops;
 memset(disable_errors,0,sizeof(disable_errors));
 memset(enables,0,sizeof(enables)); memset(disables,0,sizeof(disables));
 memset(references,0,sizeof(references));
 gates=writes=runtime_refs=0;
}
'''
    return harness


@unittest.skipUnless(shutil.which('cc'), 'requires a C compiler')
@unittest.skipUnless(CORE.is_file(), 'requires the local pinned Linux source tree')
class CoreLifecycle(unittest.TestCase):
    def run_fixture(self, body, off_policy='current'):
        source = source_harness(off_policy) + '\nint main(void) {\n' + body + '\nreturn 0;\n}\n'
        with tempfile.TemporaryDirectory(prefix='cascadia-phy-core-') as td:
            path, binary = Path(td) / 'count.c', Path(td) / 'count'
            path.write_text(source)
            subprocess.run(['cc', '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra',
                            '-Werror', '-fsanitize=undefined', str(path), '-o', str(binary)],
                           check=True, capture_output=True, text=True)
            subprocess.run([str(binary)], check=True, capture_output=True, text=True)

    def test_balanced_multiple_consumers_share_callbacks(self):
        self.run_fixture(r'''
 struct s5l_usbphy p; struct phy phy; uint32_t complex;
 reset(&phy,&p,&complex);
 assert(!phy_power_on(&phy) && !phy_power_on(&phy));
 assert(phy.power_count==2 && runtime_refs==2);
 assert(enables[0]==1 && enables[1]==1 && gates==2 && writes==1);
 assert(!phy_power_off(&phy) && phy.power_count==1);
 assert(!disables[0] && !disables[1] && p.hsic_supply_on && p.lpo_supply_on);
 assert(!phy_power_off(&phy) && !phy.power_count && !runtime_refs);
 assert(disables[0]==1 && disables[1]==1 && !p.hsic_supply_on && !p.lpo_supply_on);
''')

    def test_partial_disable_then_power_on_does_not_restore_supply(self):
        # Both early provider-error permutations expose the same generic-core
        # contract. The gap is the asserted observed behavior, not a fix.
        self.run_fixture(r'''
 for(unsigned failed=0; failed<2; failed++) {
  struct s5l_usbphy p; struct phy phy; uint32_t complex;
  reset(&phy,&p,&complex); assert(!phy_power_on(&phy));
  disable_errors[failed]=-EIO;
  assert(phy_power_off(&phy)==-EIO && phy.power_count==1 && runtime_refs==1);
  assert(p.lpo_supply_on==(failed==0) && p.hsic_supply_on==(failed==1));
  assert(references[failed]==1 && references[!failed]==0);
  unsigned before_writes=writes;
  assert(!phy_power_on(&phy) && phy.power_count==2 && runtime_refs==2);
  assert(enables[0]==1 && enables[1]==1 && writes==before_writes);
  assert(p.lpo_supply_on==(failed==0) && p.hsic_supply_on==(failed==1));
  assert(!phy_power_off(&phy) && phy.power_count==1);
  disable_errors[failed]=0;
  assert(!phy_power_off(&phy) && !phy.power_count && !runtime_refs);
  assert(!p.lpo_supply_on && !p.hsic_supply_on);
  assert(!references[0] && !references[1]);
 }
''')

    def test_historical_false_success_resumes_but_leaks_failed_reference(self):
        self.run_fixture(r'''
 struct s5l_usbphy p; struct phy phy; uint32_t complex;
 reset(&phy,&p,&complex); assert(!phy_power_on(&phy));
 disable_errors[0]=-EIO;
 assert(!phy_power_off(&phy) && !phy.power_count && !runtime_refs);
 assert(!p.lpo_supply_on && !p.hsic_supply_on);
 assert(references[0]==1 && references[1]==0);
 assert(!phy_power_on(&phy) && phy.power_count==1);
 assert(p.lpo_supply_on && p.hsic_supply_on);
 assert(references[0]==2 && references[1]==1);
 disable_errors[0]=0;
 assert(!phy_power_off(&phy) && !phy.power_count);
 assert(references[0]==1 && references[1]==0);
''', 'historical')

    def test_hidden_error_control_drops_core_count_but_is_false_success(self):
        self.run_fixture(r'''
 struct s5l_usbphy p; struct phy phy; uint32_t complex;
 reset(&phy,&p,&complex); assert(!phy_power_on(&phy));
 disable_errors[0]=-EIO;
 assert(!phy_power_off(&phy) && !phy.power_count && !runtime_refs);
 assert(p.lpo_supply_on && !p.hsic_supply_on && references[0]==1);
 assert(!phy_power_on(&phy) && phy.power_count==1);
 assert(p.lpo_supply_on && p.hsic_supply_on);
 assert(enables[0]==1 && enables[1]==2 && references[0]==1 && references[1]==1);
 disable_errors[0]=0;
 assert(!phy_power_off(&phy) && !phy.power_count && !references[0] && !references[1]);
''', 'hidden_error_control')

    def test_explicit_retry_can_release_retained_reference(self):
        self.run_fixture(r'''
 struct s5l_usbphy p; struct phy phy; uint32_t complex;
 reset(&phy,&p,&complex); assert(!phy_power_on(&phy));
 disable_errors[0]=-ETIMEDOUT;
 assert(phy_power_off(&phy)==-ETIMEDOUT && phy.power_count==1);
 disable_errors[0]=0;
 assert(!phy_power_off(&phy) && !phy.power_count && !runtime_refs);
 assert(disables[0]==2 && disables[1]==1);
 assert(!phy_power_on(&phy));
 assert(enables[0]==2 && enables[1]==2 && p.hsic_supply_on && p.lpo_supply_on);
 assert(!phy_power_off(&phy) && !phy.power_count);
''')


if __name__ == '__main__':
    unittest.main()
