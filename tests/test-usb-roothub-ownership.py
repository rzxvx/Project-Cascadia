"""Compile actual candidate USB PHY consumers with real core functions offline."""
from pathlib import Path
import importlib.util
import subprocess
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('core_fixture',ROOT/'tests/test-usb-phy-fail-closed.py')
core=importlib.util.module_from_spec(spec);spec.loader.exec_module(core)


def harness(legacy_core=False, historical_mode=False):
    src=core.harness(legacy=legacy_core)
    src=src.replace('int power_count; int power_off_error;',
                    'int power_count; int power_off_error; int init_count;')
    src=src.replace('int (*power_off)(struct phy *); };',
                    'int (*power_off)(struct phy *); int (*init)(struct phy *); int (*exit)(struct phy *); };')
    original_ops='static const struct phy_ops ops={s5l_hostphy_power_on,s5l_hostphy_power_off};'
    insertion='''
static int init_errors[3], exit_errors[3], on_errors[3], mode_errors[3];
static unsigned init_calls[3], exit_calls[3], mode_calls;
static int fixture_init(struct phy *phy) { unsigned id=phy->dev.unused; init_calls[id]++; return init_errors[id]; }
static int fixture_exit(struct phy *phy) { unsigned id=phy->dev.unused; exit_calls[id]++; return exit_errors[id]; }
static int fixture_on(struct phy *phy) {
 unsigned id=phy->dev.unused;
 return on_errors[id] ?: s5l_hostphy_power_on(phy);
}
static const struct phy_ops ops={.power_on=fixture_on,.power_off=s5l_hostphy_power_off,
 .init=fixture_init,.exit=fixture_exit};
'''
    assert original_ops in src
    src=src.replace(original_ops,insertion,1)
    baseline=core.base.baseline_source('drivers/phy/phy-core.c', core.base.PINNED_CORE_SHA256).decode()
    funcs='\n'.join(core.base.extract(baseline,'int '+name) for name in ('phy_init','phy_exit'))
    src+='\n'+funcs
    src+='''
#include <stddef.h>
struct list_head { struct list_head *next,*prev; };
#define container_of(p,t,m) ((t *)((char *)(p)-offsetof(t,m)))
#define list_entry(p,t,m) container_of(p,t,m)
#define list_for_each_entry(v,h,m) for(v=list_entry((h)->next,__typeof__(*v),m); &(v)->m!=(h); v=list_entry((v)->m.next,__typeof__(*v),m))
#define list_for_each_entry_reverse(v,h,m) for(v=list_entry((h)->prev,__typeof__(*v),m); &(v)->m!=(h); v=list_entry((v)->m.prev,__typeof__(*v),m))
#define list_for_each_entry_continue_reverse(v,h,m) for(v=list_entry((v)->m.prev,__typeof__(*v),m); &(v)->m!=(h); v=list_entry((v)->m.prev,__typeof__(*v),m))
static void INIT_LIST_HEAD(struct list_head *h) { h->next=h->prev=h; }
static void list_add_tail(struct list_head *v,struct list_head *h) {
 v->next=h; v->prev=h->prev; h->prev->next=v; h->prev=v;
}
#define EXPORT_SYMBOL_GPL(...) /* empty */
enum phy_mode { PHY_MODE_USB_HOST };
static int phy_set_mode(struct phy *p,enum phy_mode mode) {
 (void)mode; mode_calls++; return mode_errors[p->dev.unused];
}
static bool device_may_wakeup(struct device *d) { return d->unused; }
'''
    candidate=core.usb_phy_candidate.sources()['drivers/usb/core/phy.c']
    start=candidate.index('struct usb_phy_roothub {');end=candidate.index('};',start)+2
    src+=candidate[start:end]
    for declaration in ('int usb_phy_roothub_init','int usb_phy_roothub_exit',
                        'int usb_phy_roothub_set_mode','static int usb_phy_roothub_release',
                        'static int usb_phy_roothub_recover','bool usb_phy_roothub_needs_resume',
                        'int usb_phy_roothub_power_on','int usb_phy_roothub_power_off',
                        'int usb_phy_roothub_suspend','int usb_phy_roothub_resume'):
        selected = core.base.baseline_source('drivers/usb/core/phy.c', core.usb_phy_candidate.HASHES['drivers/usb/core/phy.c']).decode() if historical_mode and declaration == 'int usb_phy_roothub_set_mode' else candidate
        src+='\n'+core.base.extract(selected,declaration)
    src+='''
static void fixture_reset(struct phy *phys,struct s5l_usbphy *data,uint32_t *complex,unsigned n) {
 for(unsigned i=0;i<n;i++) {
  setup(&phys[i],&data[i],&complex[i],false);
  phys[i].dev.unused=i; data[i].dev=&phys[i].dev;
 }
 memset(init_errors,0,sizeof(init_errors)); memset(exit_errors,0,sizeof(exit_errors));
 memset(on_errors,0,sizeof(on_errors)); memset(mode_errors,0,sizeof(mode_errors));
 memset(init_calls,0,sizeof(init_calls)); memset(exit_calls,0,sizeof(exit_calls)); mode_calls=0;
}
static void root_setup(struct usb_phy_roothub *root,struct usb_phy_roothub *entries,
                       struct phy **phys,unsigned n) {
 memset(root,0,sizeof(*root)); INIT_LIST_HEAD(&root->list);
 for(unsigned i=0;i<n;i++) {
  memset(&entries[i],0,sizeof(entries[i])); entries[i].phy=phys[i];
  list_add_tail(&entries[i].list,&root->list);
 }
}
'''
    return src


class Consumer(unittest.TestCase):
    def run_c(self,body,legacy_core=False,historical_mode=False):
        src=harness(legacy_core,historical_mode)+'\nint main(void) {\n'+body+'\nreturn 0;\n}\n'
        with tempfile.TemporaryDirectory(prefix='usb-phy-consumer-') as td:
            c,binary=Path(td)/'consumer.c',Path(td)/'consumer'
            c.write_text(src)
            result=subprocess.run(['cc','-std=gnu11','-O1','-g','-Wall','-Wextra','-Werror',
                     '-fsanitize=undefined',str(c),'-o',str(binary)],text=True,capture_output=True)
            self.assertEqual(result.returncode,0,result.stderr)
            result=subprocess.run([str(binary)],text=True,capture_output=True)
            self.assertEqual(result.returncode,0,result.stderr)

    def test_partial_off_persistent_error_then_one_cleanup_and_balanced_resume(self):
        self.run_c('''
 struct phy phy; struct s5l_usbphy p; uint32_t complex;
 fixture_reset(&phy,&p,&complex,1);
 struct usb_phy_roothub root,entry; struct phy *refs[]={&phy};
 root_setup(&root,&entry,refs,1); struct device controller={.unused=1};
 assert(!usb_phy_roothub_init(&root) && !usb_phy_roothub_power_on(&root));
 disable_errors[0]=-EIO;
 assert(usb_phy_roothub_suspend(&controller,&root)==-EIO);
 assert(phy.power_count==1 && phy.init_count==1 && entry.powered && entry.initialized);
 unsigned before=disables[0];
 assert(usb_phy_roothub_resume(&controller,&root)==-EIO);
 assert(disables[0]==before+1 && phy.power_count==1 && phy.init_count==1);
 assert(usb_phy_roothub_needs_resume(&root) && !p.hsic_supply_on);
 disable_errors[0]=0;
 assert(!usb_phy_roothub_resume(&controller,&root));
 assert(phy.power_count==1 && phy.init_count==1 && runtime_refs==1);
 assert(p.hsic_supply_on && p.lpo_supply_on && !usb_phy_roothub_needs_resume(&root));
 assert(!usb_phy_roothub_power_off(&root) && !usb_phy_roothub_exit(&root));
 assert(!phy.power_count && !phy.init_count && !runtime_refs && !references[0] && !references[1]);
''')

    def test_nonwakeup_failure_retains_init_then_normal_suspend_releases_it(self):
        self.run_c('''
 struct phy phy; struct s5l_usbphy p; uint32_t complex; fixture_reset(&phy,&p,&complex,1);
 struct usb_phy_roothub root,entry; struct phy *refs[]={&phy}; root_setup(&root,&entry,refs,1);
 struct device controller={0};
 assert(!usb_phy_roothub_init(&root) && !usb_phy_roothub_power_on(&root));
 disable_errors[1]=-EIO; assert(usb_phy_roothub_suspend(&controller,&root)==-EIO);
 assert(phy.init_count==1 && !exit_calls[0]);
 disable_errors[1]=0; assert(!usb_phy_roothub_resume(&controller,&root));
 assert(phy.init_count==1 && phy.power_count==1);
 assert(!usb_phy_roothub_suspend(&controller,&root));
 assert(!phy.init_count && !phy.power_count && exit_calls[0]==1);
 assert(!usb_phy_roothub_resume(&controller,&root));
 assert(phy.init_count==1 && phy.power_count==1 && init_calls[0]==2);
''')

    def test_duplicate_entries_same_phy_recover_before_new_acquire(self):
        self.run_c('''
 struct phy phy; struct s5l_usbphy p; uint32_t complex; fixture_reset(&phy,&p,&complex,1);
 struct usb_phy_roothub root,entries[2]; struct phy *refs[]={&phy,&phy};
 root_setup(&root,entries,refs,2); struct device controller={.unused=1};
 assert(!usb_phy_roothub_init(&root) && !usb_phy_roothub_power_on(&root));
 assert(phy.init_count==2 && phy.power_count==2 && references[0]==1 && references[1]==1);
 disable_errors[0]=-EIO; assert(usb_phy_roothub_suspend(&controller,&root)==-EIO);
 assert(phy.power_count==1 && entries[0].powered && !entries[1].powered);
 assert(usb_phy_roothub_resume(&controller,&root)==-EIO && phy.power_count==1);
 disable_errors[0]=0; assert(!usb_phy_roothub_resume(&controller,&root));
 assert(phy.power_count==2 && phy.init_count==2 && references[0]==1 && references[1]==1);
 assert(!usb_phy_roothub_power_off(&root) && !usb_phy_roothub_exit(&root));
 assert(!phy.power_count && !phy.init_count);
''')

    def test_independent_root_consumer_cannot_bypass_shared_fault_latch(self):
        self.run_c('''
 struct phy phy; struct s5l_usbphy p; uint32_t complex; fixture_reset(&phy,&p,&complex,1);
 struct usb_phy_roothub roots[2],entries[2]; struct phy *refs[]={&phy};
 root_setup(&roots[0],&entries[0],refs,1); root_setup(&roots[1],&entries[1],refs,1);
 struct device controller={.unused=1};
 for(unsigned i=0;i<2;i++) assert(!usb_phy_roothub_init(&roots[i]) && !usb_phy_roothub_power_on(&roots[i]));
 assert(!usb_phy_roothub_suspend(&controller,&roots[0]));
 disable_errors[0]=-EIO; assert(usb_phy_roothub_suspend(&controller,&roots[1])==-EIO);
 assert(usb_phy_roothub_resume(&controller,&roots[0])==-EIO && phy.power_count==1);
 assert(!entries[0].powered && entries[1].powered && phy.init_count==2);
 disable_errors[0]=0;
 assert(!usb_phy_roothub_resume(&controller,&roots[1]));
 assert(!usb_phy_roothub_resume(&controller,&roots[0]) && phy.power_count==2);
 for(unsigned i=0;i<2;i++) assert(!usb_phy_roothub_power_off(&roots[i]) && !usb_phy_roothub_exit(&roots[i]));
 assert(!phy.power_count && !phy.init_count && !runtime_refs);
''')

    def test_multi_phy_power_on_unwind_preserves_original_error_and_failed_owner(self):
        self.run_c('''
 struct phy phys[2]; struct s5l_usbphy data[2]; uint32_t complex[2]; fixture_reset(phys,data,complex,2);
 struct usb_phy_roothub root,entries[2]; struct phy *refs[]={&phys[0],&phys[1]}; root_setup(&root,entries,refs,2);
 assert(!usb_phy_roothub_init(&root));
 on_errors[1]=-EACCES; disable_errors[0]=-EIO;
 assert(usb_phy_roothub_power_on(&root)==-EACCES);
 assert(entries[0].powered && entries[0].power_off_error==-EIO && !entries[1].powered);
 assert(phys[0].power_count==1 && !phys[1].power_count);
 assert(usb_phy_roothub_exit(&root)==-EIO);
 assert(phys[0].init_count==1 && !phys[1].init_count);
 disable_errors[0]=0; on_errors[1]=0; struct device controller={0};
 assert(!usb_phy_roothub_resume(&controller,&root));
 assert(phys[0].power_count==1 && phys[1].power_count==1 && phys[0].init_count==1 && phys[1].init_count==1);
 assert(!usb_phy_roothub_power_off(&root) && !usb_phy_roothub_exit(&root));
 assert(!runtime_refs && !references[0] && !references[1]);
''')

    def test_remove_failure_retains_ownership_and_unowned_repeated_off_never_puts(self):
        self.run_c('''
 struct phy phy; struct s5l_usbphy p; uint32_t complex; fixture_reset(&phy,&p,&complex,1);
 struct usb_phy_roothub root,entry; struct phy *refs[]={&phy}; root_setup(&root,&entry,refs,1);
 assert(!usb_phy_roothub_init(&root) && !usb_phy_roothub_power_on(&root));
 disable_errors[0]=-EIO;
 assert(usb_phy_roothub_power_off(&root)==-EIO && usb_phy_roothub_exit(&root)==-EIO);
 assert(phy.power_count==1 && phy.init_count==1 && entry.powered && entry.initialized);
 disable_errors[0]=0;
 assert(!usb_phy_roothub_power_off(&root) && !usb_phy_roothub_exit(&root));
 unsigned before=disables[0]+disables[1];
 assert(!usb_phy_roothub_power_off(&root) && !usb_phy_roothub_exit(&root));
 assert(disables[0]+disables[1]==before && !phy.power_count && !phy.init_count && !runtime_refs);
''')

    def test_set_mode_error_has_no_unowned_power_release(self):
        self.run_c('''
 struct phy phys[2]; struct s5l_usbphy data[2]; uint32_t complex[2]; fixture_reset(phys,data,complex,2);
 struct usb_phy_roothub root,entries[2]; struct phy *refs[]={&phys[0],&phys[1]}; root_setup(&root,entries,refs,2);
 assert(!usb_phy_roothub_init(&root)); mode_errors[1]=-EIO;
 assert(usb_phy_roothub_set_mode(&root,PHY_MODE_USB_HOST)==-EIO && mode_calls==2);
 assert(!phys[0].power_count && !phys[1].power_count && !runtime_refs && !disables[0] && !disables[1]);
 assert(!usb_phy_roothub_exit(&root) && !phys[0].init_count && !phys[1].init_count);
''')

    def test_failed_init_unwind_retains_error_until_explicit_exit(self):
        self.run_c('''
 struct phy phys[2]; struct s5l_usbphy data[2]; uint32_t complex[2]; fixture_reset(phys,data,complex,2);
 struct usb_phy_roothub root,entries[2]; struct phy *refs[]={&phys[0],&phys[1]}; root_setup(&root,entries,refs,2);
 init_errors[1]=-EACCES; exit_errors[0]=-EIO;
 assert(usb_phy_roothub_init(&root)==-EACCES);
 assert(entries[0].initialized && entries[0].init_error==-EIO && phys[0].init_count==1);
 assert(usb_phy_roothub_init(&root)==-EIO && phys[0].init_count==1);
 exit_errors[0]=0; init_errors[1]=0;
 assert(!usb_phy_roothub_exit(&root)); assert(!usb_phy_roothub_init(&root));
 assert(phys[0].init_count==1 && phys[1].init_count==1);
''')


    def test_exact_historical_mode_control_underflows_unowned_reference(self):
        self.run_c('''
 struct phy phys[2]; struct s5l_usbphy data[2]; uint32_t complex[2]; fixture_reset(phys,data,complex,2);
 struct usb_phy_roothub root,entries[2]; struct phy *refs[]={&phys[0],&phys[1]}; root_setup(&root,entries,refs,2);
 assert(!usb_phy_roothub_init(&root)); mode_errors[1]=-EIO;
 /* No runtime-PM usage refs exist; disabling PM lets the exact original
  * put helper return ENOTSUPP while exposing the original count decrement. */
 phys[0].dev.pm_enabled=phys[1].dev.pm_enabled=false;
 assert(usb_phy_roothub_set_mode(&root,PHY_MODE_USB_HOST)==-EIO);
 assert(phys[0].power_count==-1 && phys[1].power_count==0);
 assert(!runtime_refs && !disables[0] && !disables[1]);
''',legacy_core=True,historical_mode=True)

    def test_mode_caller_fix_is_independent_of_new_core_zero_count_guard(self):
        self.run_c('''
 struct phy phys[2]; struct s5l_usbphy data[2]; uint32_t complex[2]; fixture_reset(phys,data,complex,2);
 struct usb_phy_roothub root,entries[2]; struct phy *refs[]={&phys[0],&phys[1]}; root_setup(&root,entries,refs,2);
 assert(!usb_phy_roothub_init(&root)); mode_errors[1]=-EIO;
 assert(usb_phy_roothub_set_mode(&root,PHY_MODE_USB_HOST)==-EIO);
 assert(!phys[0].power_count && !phys[1].power_count && !runtime_refs);
''',legacy_core=True)


    def test_generic_init_failure_is_not_globally_latched_by_this_power_fix(self):
        self.run_c('''
 struct phy phy; struct s5l_usbphy p; uint32_t complex; fixture_reset(&phy,&p,&complex,1);
 struct usb_phy_roothub root,entry; struct phy *refs[]={&phy}; root_setup(&root,&entry,refs,1);
 assert(!usb_phy_roothub_init(&root)); exit_errors[0]=-EIO;
 assert(usb_phy_roothub_exit(&root)==-EIO && entry.init_error==-EIO && phy.init_count==1);
 assert(usb_phy_roothub_init(&root)==-EIO && phy.init_count==1);
 /* A different direct consumer can still increment generic init_count;
  * the existing core skips provider init. This is an explicit scope limit. */
 assert(!phy_init(&phy) && phy.init_count==2 && init_calls[0]==1);
 exit_errors[0]=0; assert(!phy_exit(&phy) && phy.init_count==1);
 assert(!usb_phy_roothub_exit(&root) && !phy.init_count && !entry.init_error);
''')


if __name__=='__main__': unittest.main()
