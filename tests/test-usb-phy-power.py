#!/usr/bin/env python3
"""Run actual S5L PHY callbacks/probe with fake supplies and MMIO, offline."""
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'patches/files/drivers/phy/phy-apple-s5l-usb.c'


def function(text, name, return_type='int'):
    start = text.index('static ' + return_type + ' ' + name + '(')
    brace = text.index('{', start)
    depth, end = 1, brace + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]


class HostPower(unittest.TestCase):
    @unittest.skipUnless(shutil.which('cc'), 'requires a C compiler')
    def test_real_callbacks_failures_cleanup_and_retry(self):
        text = SOURCE.read_text()
        macros = '\n'.join(line for line in text.splitlines() if re.match(
            r'#define\s+(PMGR_USB_EHCI|PMGR_USB_OHCI0|USBCPLX_HSIC_EN)\b', line))
        self.assertEqual(len(macros.splitlines()), 3)
        harness = r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define EPROBE_DEFER 517
#define BIT(n) (1u << (n))
#define IS_ERR(p) ((intptr_t)(p) < 0)
#define PTR_ERR(p) ((int)(intptr_t)(p))
#define ERR_PTR(e) ((void *)(intptr_t)(e))
#define dev_warn(...) ((void)0)
#define dev_info(...) ((void)0)
/* SOURCE_MACROS */
struct regulator { int id; };
struct device { int unused; };
struct s5l_usbphy {
 void *pmgr, *usbcplx;
 struct regulator *hsic_supply, *lpo_supply;
 bool hsic_supply_on, lpo_supply_on;
 struct device *dev;
};
struct phy { struct s5l_usbphy *data; };
static struct regulator supplies[2]={{0},{1}};
static int get_errors[2], enable_errors[2], disable_errors[2], gate_errors[2];
static unsigned gets[2], enables[2], disables[2], gates[2], sleeps, writes;
static struct s5l_usbphy *phy_get_drvdata(struct phy *phy) { return phy->data; }
static struct regulator *devm_regulator_get_optional(struct device *dev, const char *name) {
 (void)dev; int i=!strcmp(name,"hsic"); assert(i || !strcmp(name,"lpo"));
 gets[i]++; return get_errors[i] ? ERR_PTR(get_errors[i]) : &supplies[i];
}
static int regulator_enable(struct regulator *r) { enables[r->id]++; return enable_errors[r->id]; }
static int regulator_disable(struct regulator *r) { assert(r); disables[r->id]++; return disable_errors[r->id]; }
static void msleep(unsigned n) { assert(n==10); sleeps++; }
static int s5l_pmgr_on(struct s5l_usbphy *p, unsigned gate) {
 (void)p; int i=gate==PMGR_USB_OHCI0; assert(i || gate==PMGR_USB_EHCI);
 gates[i]++; return gate_errors[i];
}
static uint32_t readl(void *p) { return *(uint32_t *)p; }
static void writel(uint32_t value, void *p) { writes++; *(uint32_t *)p=value; }
''' .replace('/* SOURCE_MACROS */', macros)
        harness += '\n'.join(function(text, name) for name in (
            's5l_supply_disable', 's5l_hostphy_power_on', 's5l_hostphy_power_off'))
        harness += r'''
static void reset(struct s5l_usbphy *p, uint32_t *complex) {
 memset(p,0,sizeof(*p)); *complex=0x100;
 p->usbcplx=complex;
 memset(get_errors,0,sizeof(get_errors)); memset(enable_errors,0,sizeof(enable_errors));
 memset(disable_errors,0,sizeof(disable_errors)); memset(gate_errors,0,sizeof(gate_errors));
 memset(gets,0,sizeof(gets)); memset(enables,0,sizeof(enables));
 memset(disables,0,sizeof(disables)); memset(gates,0,sizeof(gates)); sleeps=writes=0;
}
int main(void) {
 struct s5l_usbphy p; struct phy phy={&p}; uint32_t complex;
 unsigned cases=0;
 reset(&p,&complex);
 assert(!s5l_hostphy_power_on(&phy));
 assert(p.lpo_supply_on && p.hsic_supply_on && enables[0]==1 && enables[1]==1);
 assert(gates[0]==1 && gates[1]==1 && writes==1 && complex==(0x100|USBCPLX_HSIC_EN));
 assert(sleeps==2 && !disables[0] && !disables[1]); cases++;
 assert(!s5l_hostphy_power_on(&phy));
 assert(enables[0]==1 && enables[1]==1 && sleeps==2); cases++;
 assert(!s5l_hostphy_power_off(&phy) && !p.lpo_supply_on && !p.hsic_supply_on);
 assert(disables[0]==1 && disables[1]==1);
 assert(!s5l_hostphy_power_off(&phy) && disables[0]==1 && disables[1]==1); cases++;
 /* Acquisition/enable failures and both PMGR failures must stop before
  * usb-complex writes, undo only acquired supplies, and retain the cause. */
 for (int kind=0; kind<7; kind++) {
  reset(&p,&complex);
  const int errors[]={-EPROBE_DEFER,-ENOMEM,-EIO,-EPROBE_DEFER,-EACCES,-ETIMEDOUT,-EIO};
  int error=errors[kind];
  if (kind<2) get_errors[0]=error;
  if (kind==2) enable_errors[0]=error;
  if (kind==3) get_errors[1]=error;
  if (kind==4) enable_errors[1]=error;
  if (kind>=5) gate_errors[kind-5]=error;
  assert(s5l_hostphy_power_on(&phy)==error && !writes && complex==0x100);
  assert(!p.lpo_supply_on && !p.hsic_supply_on);
  assert(disables[0]==(kind>=3) && disables[1]==(kind>=5));
  if (kind==5) assert(!gates[1]);
  cases++;
 }
 reset(&p,&complex); get_errors[0]=get_errors[1]=-ENODEV;
 assert(!s5l_hostphy_power_on(&phy) && !p.lpo_supply_on && !p.hsic_supply_on);
 assert(!enables[0] && !enables[1] && gates[0]==1 && gates[1]==1); cases++;
 reset(&p,&complex); p.lpo_supply=&supplies[0]; p.hsic_supply=&supplies[1];
 p.lpo_supply_on=p.hsic_supply_on=true; gate_errors[0]=-ETIMEDOUT;
 assert(s5l_hostphy_power_on(&phy)==-ETIMEDOUT);
 assert(p.lpo_supply_on && p.hsic_supply_on && !disables[0] && !disables[1]); cases++;
 reset(&p,&complex); p.lpo_supply=&supplies[0]; p.lpo_supply_on=true;
 gate_errors[0]=-ETIMEDOUT;
 assert(s5l_hostphy_power_on(&phy)==-ETIMEDOUT);
 assert(p.lpo_supply_on && !p.hsic_supply_on && !disables[0] && disables[1]==1); cases++;
 reset(&p,&complex); gate_errors[1]=-ETIMEDOUT; disable_errors[1]=-EIO;
 assert(s5l_hostphy_power_on(&phy)==-ETIMEDOUT);
 assert(!p.lpo_supply_on && p.hsic_supply_on && disables[0]==1 && disables[1]==1); cases++;
 disable_errors[1]=0;
 assert(!s5l_hostphy_power_off(&phy) && !p.hsic_supply_on && disables[1]==2); cases++;
 reset(&p,&complex); assert(!s5l_hostphy_power_on(&phy));
 disable_errors[0]=-ETIMEDOUT; disable_errors[1]=-EIO;
 assert(s5l_hostphy_power_off(&phy)==-EIO && p.lpo_supply_on && p.hsic_supply_on);
 assert(disables[0]==1 && disables[1]==1); cases++;
 disable_errors[1]=0;
 assert(s5l_hostphy_power_off(&phy)==-ETIMEDOUT && p.lpo_supply_on && !p.hsic_supply_on); cases++;
 disable_errors[0]=0;
 assert(!s5l_hostphy_power_off(&phy) && !p.lpo_supply_on); cases++;
 printf("USB PHY power: %u actual callback cases passed\n",cases);
 return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix='cascadia-phy-power-') as td:
            source, binary = Path(td) / 'power.c', Path(td) / 'power'
            source.write_text(harness)
            subprocess.run(['cc', '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra',
                            '-Werror', '-fsanitize=undefined', str(source), '-o', str(binary)],
                           check=True)
            subprocess.run([str(binary)], check=True)


@unittest.skipUnless(shutil.which('cc'), 'requires a C compiler')
class OTGLifecycle(unittest.TestCase):
    @staticmethod
    def run_c(source):
        with tempfile.TemporaryDirectory(prefix='cascadia-phy-lifecycle-') as td:
            path, binary = Path(td) / 'lifecycle.c', Path(td) / 'lifecycle'
            path.write_text(source)
            subprocess.run(['cc', '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra',
                            '-Werror', '-fsanitize=undefined', str(path), '-o', str(binary)],
                           check=True, capture_output=True, text=True)
            return subprocess.run([str(binary)], capture_output=True, text=True)

    @staticmethod
    def init_harness(text):
        # Keep the actual gate polling and register dump; no error-return oracle
        # is substituted for s5l_pmgr_on(). All mapped registers are C arrays.
        macros = '\n'.join(line for line in text.splitlines()
                           if line.startswith('#define '))
        harness = r'''
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#define BIT(n) (1u << (n))
typedef uint32_t u32;
struct device { int unused; };
struct s5l_usbphy { void *base, *pmgr; struct device *dev; };
struct phy { struct s5l_usbphy *data; };
static struct s5l_usbphy *phy_get_drvdata(struct phy *phy) { return phy->data; }
static void log_message(const char *fmt, ...) { (void)fmt; }
static void device_message(struct device *dev, const char *fmt, ...) { (void)dev; (void)fmt; }
#define dev_info(...) device_message(__VA_ARGS__)
#define dev_err(...) device_message(__VA_ARGS__)
#define pr_err(...) log_message(__VA_ARGS__)
/* SOURCE_MACROS */
static uint32_t phy_regs[0x1000/4], gate_regs[0x2000/4];
static unsigned phy_writes, gate_writes, delays[16], delay_count, gate_polls;
static uint32_t write_values[2], write_offsets[2];
static bool requested, acknowledge, force_readback;
static uint32_t forced_readback;
static unsigned acknowledge_after;
static uint32_t readl(void *address) {
 if (address==&gate_regs[PMGR_USB_OTG/4] && requested) {
  gate_polls++;
  if (force_readback) return forced_readback;
  if (acknowledge && gate_polls >= acknowledge_after) {
   uint32_t v=*(uint32_t *)address;
   *(uint32_t *)address=(v & ~0xf0u) | ((v & 0xfu)<<4);
  }
 }
 return *(uint32_t *)address;
}
static void writel(uint32_t value, void *address) {
 uintptr_t a=(uintptr_t)address, b=(uintptr_t)phy_regs;
 if (a>=b && a<b+sizeof(phy_regs)) {
  assert(phy_writes<2);
  write_offsets[phy_writes]=(uint32_t)(a-b);
  write_values[phy_writes++]=value;
 } else {
  assert(address==&gate_regs[PMGR_USB_OTG/4]);
  gate_writes++; requested=true;
 }
 *(uint32_t *)address=value;
}
static void udelay(unsigned us) {
 /* Gate poll delays are distinguished from the post-gate PHY sequence. */
 if (us==1) return;
 assert(delay_count<16); delays[delay_count++]=us;
}
''' .replace('/* SOURCE_MACROS */', macros)
        return harness + '\n'.join((function(text, 's5l_pmgr_on'),
                                     function(text, 'phy_dump_regs', 'void'),
                                     function(text, 's5l_usbphy_init')))

    def test_otg_gate_timeout_stops_before_phy_reset_and_can_retry(self):
        harness = self.init_harness(SOURCE.read_text()) + r'''
int main(void) {
 struct device dev={0}; struct s5l_usbphy p={phy_regs,gate_regs,&dev};
 struct phy phy={&p}; unsigned cases=0;
 phy_regs[ORSTCON/4]=0xa0;
 phy_regs[0x40/4]=0x549; phy_regs[0x44/4]=0x2ff3;
 gate_regs[PMGR_USB_OTG/4]=0xf1230120;
 acknowledge=false;
 assert(s5l_usbphy_init(&phy)==-ETIMEDOUT);
 assert(gate_writes==1 && gate_polls==1000);
 assert(gate_regs[PMGR_USB_OTG/4]==((0xf1230120u & ~0x10fu)|0xfu));
 assert(!phy_writes && !delay_count && phy_regs[ORSTCON/4]==0xa0);
 assert(phy_regs[0x40/4]==0x549 && phy_regs[0x44/4]==0x2ff3); cases++;
 acknowledge=true;
 assert(!s5l_usbphy_init(&phy));
 assert(gate_writes==2 && phy_writes==2);
 assert(write_offsets[0]==ORSTCON && write_offsets[1]==ORSTCON);
 assert(write_values[0]==0xa1 && write_values[1]==0xa0);
 assert(delay_count==4 && delays[0]==100 && delays[1]==10 && delays[2]==20 && delays[3]==1000);
 assert(phy_regs[0x40/4]==0x549 && phy_regs[0x44/4]==0x2ff3); cases++;
 printf("OTG PHY: %u actual initialization cases passed\n",cases);
 return 0;
}
'''
        result = self.run_c(harness)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('2 actual initialization cases passed', result.stdout)

    @staticmethod
    def gate_target_cases():
        return r'''
int main(void) {
 struct device dev={0}; struct s5l_usbphy p={phy_regs,gate_regs,&dev};
 struct phy phy={&p}; unsigned cases=0;
 /* Exhaust every requested/actual nibble, including identical OFF/intermediate
  * states. Equal nibbles are not sufficient: both must indicate ON. */
 for (unsigned status=0; status<256; status++) {
  memset(phy_regs,0,sizeof(phy_regs)); memset(gate_regs,0,sizeof(gate_regs));
  phy_writes=gate_writes=gate_polls=delay_count=0; requested=false;
  force_readback=true; forced_readback=0xa5000200u | status;
  phy_regs[ORSTCON/4]=0xa0; gate_regs[PMGR_USB_OTG/4]=0xa5000100;
  int ret=s5l_usbphy_init(&phy);
  if (status==0xff) {
   if (ret || phy_writes!=2 || delay_count!=4) return 41;
  } else if (ret!=-ETIMEDOUT || phy_writes || delay_count || gate_polls!=1000) {
   fprintf(stderr,"false gate success: status=0x%02x return=%d PHY writes=%u\n",
           status,ret,phy_writes);
   return 42;
  }
  assert(gate_writes==1 && gate_regs[PMGR_USB_OTG/4]==0xa500000f);
  cases++;
 }
 /* A late acknowledgement at the final allowed poll succeeds; a later one
  * times out, without extra writes or a speculative rollback. */
 for (unsigned at=1000; at<=1001; at++) {
  phy_writes=gate_writes=gate_polls=delay_count=0; requested=false;
  force_readback=false; acknowledge=true; acknowledge_after=at;
  gate_regs[PMGR_USB_OTG/4]=0; phy_regs[ORSTCON/4]=0xa0;
  int ret=s5l_usbphy_init(&phy);
  assert(ret==(at==1000 ? 0 : -ETIMEDOUT));
  assert(phy_writes==(at==1000 ? 2u : 0u) && gate_writes==1);
  cases++;
 }
 printf("PMGR target: %u actual initialization cases passed\n",cases);
 return 0;
}
'''

    def test_pmgr_requires_requested_on_state_before_reset(self):
        result = self.run_c(self.init_harness(SOURCE.read_text()) + self.gate_target_cases())
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('258 actual initialization cases passed', result.stdout)

    def test_legacy_gate_accepts_matching_off_state(self):
        text = SOURCE.read_text()
        checked = ('if ((v & 0xf) == PMGR_GATE_ON &&\n'
                   '\t\t    ((v >> 4) & 0xf) == PMGR_GATE_ON)')
        self.assertEqual(text.count(checked), 1)
        text = text.replace(checked, 'if (!((v ^ (v >> 4)) & 0xf))')
        result = self.run_c(self.init_harness(text) + self.gate_target_cases())
        self.assertEqual(result.returncode, 42, result.stderr)
        self.assertIn('status=0x00 return=0 PHY writes=2', result.stderr)

    def test_legacy_init_reports_success_and_resets_after_gate_timeout(self):
        text = SOURCE.read_text()
        checked = ('ret = s5l_pmgr_on(p, PMGR_USB_OTG);\n'
                   '\tif (ret)\n\t\treturn ret;')
        self.assertEqual(text.count(checked), 1)
        # Restore only the historical ignored-return statement in a temporary
        # extraction. The production source is never rewritten by this test.
        text = text.replace(checked, 's5l_pmgr_on(p, PMGR_USB_OTG);')
        extracted = function(text, 's5l_usbphy_init')
        self.assertEqual(extracted.count('\tint ret;'), 1)
        text = text.replace(extracted, extracted.replace('\tint ret;\n', ''))
        harness = self.init_harness(text) + r'''
int main(void) {
 struct device dev={0}; struct s5l_usbphy p={phy_regs,gate_regs,&dev};
 struct phy phy={&p}; phy_regs[ORSTCON/4]=0xa0;
 int ret=s5l_usbphy_init(&phy);
 /* This same timeout contract deliberately fails on the historical code. */
 if (ret!=-ETIMEDOUT || phy_writes || delay_count) {
  fprintf(stderr,"legacy false success: return=%d, PHY writes=%u, post-gate delays=%u\n",
          ret,phy_writes,delay_count);
  return 42;
 }
 return 0;
}
'''
        result = self.run_c(harness)
        self.assertEqual(result.returncode, 42, result.stderr)
        self.assertIn('return=0, PHY writes=2, post-gate delays=4', result.stderr)

    def test_probe_distinguishes_missing_optional_resource_and_mapping_error(self):
        text = SOURCE.read_text()
        harness = r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define GFP_KERNEL 0
#define IORESOURCE_MEM 0
#define PMGR_BASE 0x3f100000
#define IS_ERR(p) ((intptr_t)(p)<0)
#define PTR_ERR(p) ((int)(intptr_t)(p))
#define ERR_PTR(e) ((void *)(intptr_t)(e))
#define dev_info(...) ((void)0)
#define dev_err(...) ((void)0)
struct device { int unused; };
struct phy { int unused; };
struct phy_provider { int unused; };
struct s5l_usbphy { void *base, *pmgr, *usbcplx; struct device *dev; struct phy *phys[2]; };
struct platform_device { struct device dev; };
static struct s5l_usbphy object;
static struct phy fake_phys[2];
static struct phy_provider provider;
static int s5l_usbphy_ops, s5l_hostphy_ops, s5l_usbphy_xlate;
static unsigned map_calls[2], created, published, drvdata;
static int map_error;
static bool optional_present;
static void *devm_kzalloc(struct device *dev, size_t size, int flags) {
 (void)dev; assert(size==sizeof(object) && flags==GFP_KERNEL);
 memset(&object,0,sizeof(object)); return &object;
}
static void *devm_platform_ioremap_resource(struct platform_device *p, int index) {
 (void)p; assert(index==0 || index==1); map_calls[index]++;
 return index && map_error ? ERR_PTR(map_error) : (void *)(uintptr_t)(0x1000+index*0x1000);
}
static void *devm_ioremap(struct device *dev, unsigned base, size_t size) {
 (void)dev; assert(base==PMGR_BASE && size==0x2000); return (void *)(uintptr_t)0x3000;
}
static void *platform_get_resource(struct platform_device *p, int type, int index) {
 (void)p; assert(type==IORESOURCE_MEM && index==1);
 return optional_present ? (void *)(uintptr_t)1 : NULL;
}
static void platform_set_drvdata(struct platform_device *p, struct s5l_usbphy *data) {
 (void)p; assert(data==&object); drvdata++;
}
static struct phy *devm_phy_create(struct device *dev, void *node, int *ops) {
 (void)dev; assert(!node && created<2);
 assert(ops==(created ? &s5l_hostphy_ops : &s5l_usbphy_ops)); return &fake_phys[created++];
}
static void phy_set_drvdata(struct phy *phy, struct s5l_usbphy *data) {
 assert((phy==&fake_phys[0] || phy==&fake_phys[1]) && data==&object);
}
static struct phy_provider *devm_of_phy_provider_register(struct device *dev, int xlate) {
 (void)dev; assert(xlate==s5l_usbphy_xlate); published++; return &provider;
}
''' + function(text, 's5l_usbphy_probe') + r'''
int main(void) {
 struct platform_device p={0}; unsigned cases=0;
 optional_present=false;
 assert(!s5l_usbphy_probe(&p));
 assert(!object.usbcplx && !map_calls[1] && created==2 && published==1); cases++;
 map_calls[0]=map_calls[1]=created=published=drvdata=0;
 optional_present=true; map_error=-ENOMEM;
 assert(s5l_usbphy_probe(&p)==-ENOMEM);
 assert(map_calls[1]==1 && !created && !published && !drvdata); cases++;
 map_calls[0]=map_calls[1]=created=published=drvdata=0;
 map_error=-EBUSY;
 assert(s5l_usbphy_probe(&p)==-EBUSY);
 assert(map_calls[1]==1 && !created && !published && !drvdata); cases++;
 map_calls[0]=map_calls[1]=created=published=drvdata=0;
 map_error=0;
 assert(!s5l_usbphy_probe(&p));
 assert(object.usbcplx==(void *)(uintptr_t)0x2000 && created==2 && published==1); cases++;
 printf("USB PHY probe: %u actual resource cases passed\n",cases);
 return 0;
}
'''
        result = self.run_c(harness)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('4 actual resource cases passed', result.stdout)


if __name__ == '__main__':
    unittest.main()
