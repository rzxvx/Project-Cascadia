#!/usr/bin/env python3
"""Evaluate a temporary supply-release policy through real core/roothub code.

The transactional callback exists only in generated fake-provider C. This is
diagnostic research, not a runtime fix or proof of WLAN recovery.
"""
import hashlib
import importlib.util
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location('lifecycle_fixture',
    ROOT / 'tests/test-usb-phy-core-lifecycle.py')
LIFECYCLE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(LIFECYCLE)
ROOT_HUB = LIFECYCLE.LINUX / 'drivers/usb/core/phy.c'
ROOT_HUB_SHA256 = '700f01a45cbfd4fa08778c1507ad352cc88ae11655437e0ffdad05a876efb7c6'

# This is intentionally not the driver callback. Restore only the reference
# actually released by this call; preserve the original release error even if
# compensation fails. Delay accounts for the existing driver's HSIC-on delay,
# not verified USB/WLAN recovery. A fake fatal counter records rollback failure.
POLICY = r'''
static unsigned fatal_rollback, restored_delays;
static int s5l_hostphy_power_off(struct phy *phy)
{
 struct s5l_usbphy *p=phy_get_drvdata(phy);
 bool had_hsic=p->hsic_supply_on;
 int ret=s5l_supply_disable(p->hsic_supply,&p->hsic_supply_on);
 if(ret) return ret;
 ret=s5l_supply_disable(p->lpo_supply,&p->lpo_supply_on);
 if(ret && had_hsic && !p->hsic_supply_on) {
  int restore=regulator_enable(p->hsic_supply);
  if(restore) fatal_rollback++;
  else { p->hsic_supply_on=true; msleep(10); restored_delays++; }
 }
 return ret;
}
'''


def harness():
    source = LIFECYCLE.source_harness()
    off = LIFECYCLE.extract(source, 'static int s5l_hostphy_power_off')
    source = source.replace(off, POLICY)
    source = source.replace('static int disable_errors[2];',
                            'static int disable_errors[2], enable_errors[2];')
    enable = ('static int regulator_enable(struct regulator *r) { '
              'enables[r->id]++; references[r->id]++; return 0; }')
    if enable not in source:
        raise ValueError('review fake enable provider before policy experiment')
    source = source.replace(enable, '''static int regulator_enable(struct regulator *r) {
 enables[r->id]++;
 if(enable_errors[r->id]) return enable_errors[r->id];
 references[r->id]++; return 0;
}''')
    # One-entry fake linked-list, sufficient for P105's EHCI one-PHY consumer.
    # Actual generic core and actual root-hub power/suspend/resume functions run.
    source += r'''
struct usb_phy_roothub;
struct list_head { struct usb_phy_roothub *entry; };
struct usb_phy_roothub { struct phy *phy; struct list_head list; };
#define list_for_each_entry(v,h,m) for((v)=(h)->entry;(v);(v)=NULL)
#define list_for_each_entry_reverse(v,h,m) list_for_each_entry(v,h,m)
#define list_for_each_entry_continue_reverse(v,h,m) for((v)=NULL;(v);(v)=NULL)
#define EXPORT_SYMBOL_GPL(...) /* empty */
static bool device_may_wakeup(struct device *d) { return d->unused; }
static unsigned exits, inits;
static int usb_phy_roothub_init(struct usb_phy_roothub *p) { (void)p; inits++; return 0; }
static int usb_phy_roothub_exit(struct usb_phy_roothub *p) { (void)p; exits++; return 0; }
'''
    root_bytes = LIFECYCLE.baseline_source('drivers/usb/core/phy.c', ROOT_HUB_SHA256)
    if hashlib.sha256(root_bytes).hexdigest() != ROOT_HUB_SHA256:
        raise ValueError('requires the pinned unmodified Linux USB PHY consumer')
    root_source = root_bytes.decode()
    for declaration in ('int usb_phy_roothub_power_on',
                        'void usb_phy_roothub_power_off',
                        'int usb_phy_roothub_suspend',
                        'int usb_phy_roothub_resume'):
        source += LIFECYCLE.extract(root_source, declaration)
    return source


@unittest.skipUnless(shutil.which('cc'), 'requires a C compiler')
@unittest.skipUnless(LIFECYCLE.CORE.is_file() and ROOT_HUB.is_file(),
                     'requires the pinned local Linux source tree')
class TemporaryPolicy(unittest.TestCase):
    def run_case(self, body):
        setup = r'''
 struct s5l_usbphy p; struct phy phy; uint32_t complex;
 reset(&phy,&p,&complex); assert(!phy_power_on(&phy));
 struct usb_phy_roothub root={.phy=&phy}; root.list.entry=&root;
 struct device controller={.unused=1};
 (void)root; (void)controller;
'''
        source = harness() + '\nint main(void) {\n' + setup + body + '\nreturn 0;\n}\n'
        with tempfile.TemporaryDirectory(prefix='cascadia-phy-policy-') as td:
            c, executable = Path(td)/'policy.c', Path(td)/'policy'
            c.write_text(source)
            subprocess.run(['cc','-std=gnu11','-O1','-g','-Wall','-Wextra','-Werror',
                            '-fsanitize=undefined',str(c),'-o',str(executable)],
                           check=True,capture_output=True,text=True)
            subprocess.run([str(executable)],check=True,capture_output=True,text=True)

    def test_success_releases_both_and_balanced_resume_reacquires(self):
        self.run_case(r'''
 assert(!usb_phy_roothub_suspend(&controller,&root));
 assert(!phy.power_count && !runtime_refs && !references[0] && !references[1]);
 assert(!usb_phy_roothub_resume(&controller,&root));
 assert(phy.power_count==1 && references[0]==1 && references[1]==1);
 assert(!fatal_rollback && !restored_delays);
''')

    def test_hsic_failure_stops_before_lpo_but_consumer_still_increments(self):
        self.run_case(r'''
 disable_errors[1]=-EIO;
 assert(phy_power_off(&phy)==-EIO && phy.power_count==1);
 assert(disables[1]==1 && !disables[0]);
 assert(p.lpo_supply_on && p.hsic_supply_on && !fatal_rollback);
 assert(!usb_phy_roothub_resume(&controller,&root) && phy.power_count==2);
 assert(enables[0]==1 && enables[1]==1);
''')

    def test_two_injected_release_errors_stop_at_first_without_compensation(self):
        self.run_case(r'''
 disable_errors[1]=-EACCES; disable_errors[0]=-ETIMEDOUT;
 assert(phy_power_off(&phy)==-EACCES && phy.power_count==1);
 assert(disables[1]==1 && !disables[0] && !fatal_rollback && !restored_delays);
 assert(p.lpo_supply_on && p.hsic_supply_on && references[0]==1 && references[1]==1);
''')

    def test_lpo_failure_restores_reference_but_count_leaks_across_resume(self):
        self.run_case(r'''
 disable_errors[0]=-ETIMEDOUT;
 assert(!usb_phy_roothub_suspend(&controller,&root));
 assert(phy.power_count==1 && runtime_refs==1);
 assert(p.lpo_supply_on && p.hsic_supply_on && references[0]==1 && references[1]==1);
 assert(enables[0]==1 && enables[1]==2 && restored_delays==1 && !fatal_rollback);
 assert(!usb_phy_roothub_resume(&controller,&root) && phy.power_count==2);
 unsigned before_disables=disables[0]+disables[1];
 disable_errors[0]=0;
 assert(!usb_phy_roothub_suspend(&controller,&root) && phy.power_count==1);
 assert(disables[0]+disables[1]==before_disables);
 assert(!usb_phy_roothub_resume(&controller,&root) && phy.power_count==2);
 assert(references[0]==1 && references[1]==1);
''')

    def test_compensation_failure_preserves_cause_and_records_fatal_state(self):
        self.run_case(r'''
 disable_errors[0]=-ETIMEDOUT; enable_errors[1]=-EACCES;
 assert(phy_power_off(&phy)==-ETIMEDOUT && fatal_rollback==1);
 assert(phy.power_count==1 && p.lpo_supply_on && !p.hsic_supply_on);
 assert(references[0]==1 && !references[1] && !restored_delays);
 assert(!usb_phy_roothub_resume(&controller,&root) && phy.power_count==2);
 assert(!p.hsic_supply_on && !references[1]);
''')

    def test_nonwakeup_suspend_exit_does_not_repair_failed_power_count(self):
        self.run_case(r'''
 controller.unused=0; disable_errors[0]=-EIO; enable_errors[1]=-EACCES;
 assert(!usb_phy_roothub_suspend(&controller,&root));
 assert(exits==1 && phy.power_count==1 && fatal_rollback==1);
 assert(!usb_phy_roothub_resume(&controller,&root));
 assert(inits==1 && phy.power_count==2 && !p.hsic_supply_on);
''')


if __name__ == '__main__':
    unittest.main()
