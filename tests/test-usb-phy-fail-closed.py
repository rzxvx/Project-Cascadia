"""Compile actual candidate PHY functions and exact historical controls offline."""
from pathlib import Path
import importlib.util
import subprocess
import tempfile
import unittest

import usb_phy_candidate

REPO=Path(__file__).resolve().parents[1]
base=usb_phy_candidate.BASELINE


def harness(legacy=False):
    src=base.source_harness()
    src=src.replace('struct device { int unused; };','struct device { int unused; bool pm_enabled; };')
    src=src.replace('unsigned power_count;', 'int power_count; int power_off_error;')
    src=src.replace('supplies[2]={{0},{1}}','supplies[3]={{0},{1},{2}}')
    src=src.replace('static int disable_errors[2];','static int disable_errors[3], enable_errors[3];')
    src=src.replace('enables[2], disables[2], references[2]', 'enables[3], disables[3], references[3]')
    src=src.replace('static int regulator_enable(struct regulator *r) { enables[r->id]++; references[r->id]++; return 0; }',
    '''static int regulator_enable(struct regulator *r) {
 enables[r->id]++; if(enable_errors[r->id]) return enable_errors[r->id];
 references[r->id]++; return 0;
}''')
    src=src.replace('#define mutex_lock(p) ((void)(p))\n#define mutex_unlock(p) ((void)(p))',
    '''static void mutex_lock(int *p) { assert(!*p); *p=1; }
static void mutex_unlock(int *p) { assert(*p); *p=0; }''')
    for name in ('phy_pm_runtime_get_sync','phy_pm_runtime_put_sync','phy_pm_runtime_put'):
        declaration=('static int ' if name=='phy_pm_runtime_get_sync' else 'static void ')+name
        src=src.replace(base.extract(src,declaration),'')
    for name in ('phy_power_on','phy_power_off'):
        actual=base.extract(src,'int '+name)
        src=src.replace(actual,'')
    runtime_src=base.baseline_source('drivers/phy/phy-core.c', base.PINNED_CORE_SHA256).decode()
    candidate=usb_phy_candidate.sources()['drivers/phy/phy-core.c']
    mocks='''
static int phy_power_off(struct phy *phy);
static bool inject_off_race;
static struct phy *race_phy;
static int runtime_get_error;
static bool pm_runtime_enabled(struct device *dev) { return dev->pm_enabled; }
static int pm_runtime_get_sync(struct device *dev) {
 (void)dev; runtime_refs++;
 if(inject_off_race) { inject_off_race=false; assert(phy_power_off(race_phy)<0); }
 return runtime_get_error;
}
static int pm_runtime_put_sync(struct device *dev) { (void)dev; assert(runtime_refs); runtime_refs--; return 0; }
static int pm_runtime_put(struct device *dev) { return pm_runtime_put_sync(dev); }
'''
    runtime='\n'.join(base.extract(runtime_src,'int '+name) for name in (
        'phy_pm_runtime_get_sync','phy_pm_runtime_put_sync','phy_pm_runtime_put'))
    functions='\n'.join(base.extract(runtime_src if legacy else candidate,'int '+name) for name in (
        'phy_power_on','phy_power_off'))
    functions=functions.replace('int phy_power_off(', 'static int phy_power_off(',1)
    position=src.index('static const struct phy_ops ops=')
    src=src[:position]+mocks+runtime+functions+src[position:]
    src+='''
static void setup(struct phy *phy,struct s5l_usbphy *p,uint32_t *complex,bool parent) {
 reset(phy,p,complex); phy->dev.pm_enabled=true;
 memset(enable_errors,0,sizeof(enable_errors));
 inject_off_race=false; runtime_get_error=0;
 if(parent) phy->pwr=&supplies[2];
}
'''
    return src


class CoreLatch(unittest.TestCase):
    def run_c(self,body,legacy=False):
        src=harness(legacy)+'\nint main(void) {\n'+body+'\nreturn 0;\n}\n'
        with tempfile.TemporaryDirectory(prefix='phy-latch-') as td:
            c,binary=Path(td)/'latch.c',Path(td)/'latch'
            c.write_text(src)
            result=subprocess.run(['cc','-std=gnu11','-O1','-g','-Wall','-Wextra','-Werror',
                    '-fsanitize=undefined',str(c),'-o',str(binary)],text=True,capture_output=True)
            self.assertEqual(result.returncode,0,result.stderr)
            result=subprocess.run([str(binary)],text=True,capture_output=True)
            self.assertEqual(result.returncode,0,result.stderr)

    def test_shared_multi_consumer_partial_fault_block_then_cleanup_and_reacquire(self):
        self.run_c('''
 struct s5l_usbphy p; struct phy phy; uint32_t complex;
 setup(&phy,&p,&complex,true);
 assert(!phy_power_on(&phy) && !phy_power_on(&phy));
 assert(phy.power_count==2 && runtime_refs==2 && references[2]==2);
 assert(!phy_power_off(&phy) && phy.power_count==1 && !disables[0] && !disables[1]);
 disable_errors[0]=-EIO;
 assert(phy_power_off(&phy)==-EIO && phy.power_off_error==-EIO && phy.power_count==1);
 assert(p.lpo_supply_on && !p.hsic_supply_on && runtime_refs==1 && references[2]==1);
 for(unsigned consumer=0;consumer<3;consumer++) {
  unsigned before_enables=enables[0]+enables[1]+enables[2], before_writes=writes;
  assert(phy_power_on(&phy)==-EIO && phy.power_count==1 && runtime_refs==1);
  assert(before_enables==enables[0]+enables[1]+enables[2] && writes==before_writes);
 }
 disable_errors[0]=-ETIMEDOUT;
 assert(phy_power_off(&phy)==-EIO && phy.power_off_error==-EIO && phy.power_count==1);
 disable_errors[0]=0;
 assert(!phy_power_off(&phy) && !phy.power_off_error && !phy.power_count && !runtime_refs);
 assert(!references[0] && !references[1] && !references[2]);
 assert(!phy_power_on(&phy) && phy.power_count==1 && p.lpo_supply_on && p.hsic_supply_on);
 assert(!phy_power_off(&phy) && !phy.power_count);
''')

    def test_late_latch_check_unwinds_only_new_parent_and_runtime_references(self):
        self.run_c('''
 struct s5l_usbphy p; struct phy phy; uint32_t complex;
 setup(&phy,&p,&complex,true); assert(!phy_power_on(&phy));
 disable_errors[0]=-EIO; inject_off_race=true; race_phy=&phy;
 assert(phy_power_on(&phy)==-EIO && phy.power_count==1 && phy.power_off_error==-EIO);
 assert(runtime_refs==1 && references[2]==1 && enables[2]==2 && disables[2]==1);
 assert(p.lpo_supply_on && !p.hsic_supply_on && enables[0]==1 && enables[1]==1);
 disable_errors[0]=0; assert(!phy_power_off(&phy) && !runtime_refs && !references[2]);
''')

    def test_busy_failure_without_partial_change_is_still_fail_closed(self):
        self.run_c('''
 struct s5l_usbphy p; struct phy phy; uint32_t complex;
 setup(&phy,&p,&complex,false); assert(!phy_power_on(&phy));
 disable_errors[0]=disable_errors[1]=-EBUSY;
 assert(phy_power_off(&phy)==-EBUSY && phy.power_count==1);
 assert(p.lpo_supply_on && p.hsic_supply_on);
 assert(phy_power_on(&phy)==-EBUSY && phy.power_count==1 && runtime_refs==1);
 disable_errors[0]=disable_errors[1]=0;
 assert(!phy_power_off(&phy) && !phy.power_off_error && !phy.power_count);
''')

    def test_parent_enable_and_runtime_get_failures_preserve_existing_accounting(self):
        self.run_c('''
 struct s5l_usbphy p; struct phy phy; uint32_t complex;
 setup(&phy,&p,&complex,true); enable_errors[2]=-EACCES;
 assert(phy_power_on(&phy)==-EACCES && !runtime_refs && !phy.power_count && !references[2]);
 setup(&phy,&p,&complex,true); runtime_get_error=-EIO;
 assert(phy_power_on(&phy)==-EIO && !runtime_refs && !phy.power_count && !references[2]);
 setup(&phy,&p,&complex,true); enable_errors[1]=-ETIMEDOUT;
 assert(phy_power_on(&phy)==-ETIMEDOUT && !runtime_refs && !phy.power_count && !references[2]);
''')

    def test_failed_parent_rollback_is_an_explicit_existing_accounting_limit(self):
        self.run_c('''
 struct s5l_usbphy p; struct phy phy; uint32_t complex;
 setup(&phy,&p,&complex,true); assert(!phy_power_on(&phy));
 disable_errors[0]=-EIO; disable_errors[2]=-EACCES;
 inject_off_race=true; race_phy=&phy;
 assert(phy_power_on(&phy)==-EIO && phy.power_count==1 && phy.power_off_error==-EIO);
 assert(runtime_refs==1 && references[2]==2);
 assert(enables[2]==2 && disables[2]==1);
 /* Generic core's existing unwind discards regulator_disable errors. This
  * is diagnosed, not repaired or represented as a released physical rail. */
''')

    def test_runtime_pm_disabled_has_no_usage_refs_and_recovers(self):
        self.run_c('''
 struct s5l_usbphy p; struct phy phy; uint32_t complex;
 setup(&phy,&p,&complex,true); phy.dev.pm_enabled=false;
 assert(!phy_power_on(&phy) && phy.power_count==1 && !runtime_refs);
 disable_errors[0]=-EIO; assert(phy_power_off(&phy)==-EIO);
 assert(phy_power_on(&phy)==-EIO && phy.power_count==1 && !runtime_refs);
 disable_errors[0]=0; assert(!phy_power_off(&phy) && !phy.power_count && !references[2]);
''')

    def test_zero_count_off_is_refused_without_provider_pm_or_regulator_put(self):
        self.run_c('''
 struct s5l_usbphy p; struct phy phy; uint32_t complex;
 setup(&phy,&p,&complex,true);
 assert(phy_power_off(&phy)==-EINVAL && !phy.power_count && !runtime_refs);
 assert(!disables[0] && !disables[1] && !disables[2]);
''')

    def test_historical_control_confirms_false_success_with_same_partial_provider_fault(self):
        self.run_c('''
 struct s5l_usbphy p; struct phy phy; uint32_t complex;
 setup(&phy,&p,&complex,true); assert(!phy_power_on(&phy));
 disable_errors[0]=-EIO; assert(phy_power_off(&phy)==-EIO && phy.power_count==1);
 assert(!phy_power_on(&phy) && phy.power_count==2 && runtime_refs==2);
 assert(!p.hsic_supply_on && p.lpo_supply_on && references[2]==2);
''',legacy=True)


    def test_baseline_control_reads_pinned_git_object_from_patched_tree(self):
        original_linux=base.LINUX
        with tempfile.TemporaryDirectory(prefix='phy-baseline-git-') as td:
            tree=Path(td); target=tree/'drivers/phy/phy-core.c'
            target.parent.mkdir(parents=True)
            target.write_text(usb_phy_candidate.sources()['drivers/phy/phy-core.c'])
            (tree/'.git').symlink_to(original_linux/'.git')
            try:
                base.LINUX=tree
                data=base.baseline_source('drivers/phy/phy-core.c',base.PINNED_CORE_SHA256)
                self.assertEqual(__import__('hashlib').sha256(data).hexdigest(),base.PINNED_CORE_SHA256)
                self.assertNotIn(b'power_off_error',data)
            finally:
                base.LINUX=original_linux

    def test_unverified_tree_without_pinned_git_object_is_refused(self):
        original_linux=base.LINUX
        with tempfile.TemporaryDirectory(prefix='phy-baseline-refuse-') as td:
            tree=Path(td); target=tree/'drivers/phy/phy-core.c'
            target.parent.mkdir(parents=True); target.write_text('unverified')
            try:
                base.LINUX=tree
                with self.assertRaisesRegex(ValueError,'exact pinned'):
                    base.baseline_source('drivers/phy/phy-core.c',base.PINNED_CORE_SHA256)
            finally:
                base.LINUX=original_linux


if __name__=='__main__':unittest.main()
