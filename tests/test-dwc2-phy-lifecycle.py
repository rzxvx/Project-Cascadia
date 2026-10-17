#!/usr/bin/env python3
"""Actual DWC2 PHY/clock ownership diagnostic with no hardware backend.

Compile original DWC2 platform resource functions unchanged, current patched
USB/PHY consumer functions and pinned historical controls. DWC2's unrelated
HCD/IRQ/bus work is injected, not a simulated complete controller probe.
No source download, production edit or kernel build occurs.
"""
from pathlib import Path
import importlib.util,sys,subprocess,tempfile,unittest
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tests'))
spec=importlib.util.spec_from_file_location('consumer',ROOT/'tests/test-usb-roothub-ownership.py')
consumer=importlib.util.module_from_spec(spec);spec.loader.exec_module(consumer)
base=consumer.core.base
PLATFORM_SHA='f1f89b559a774b10f02e1a726f0d4de8056e69b05786736e9d67b67500cd45a6'

def harness(legacy=False,historical_mode=False):
 source=base.baseline_source('drivers/usb/dwc2/platform.c',PLATFORM_SHA).decode()
 s=consumer.harness(legacy_core=legacy,historical_mode=historical_mode)
 s=s.replace("enum phy_mode { PHY_MODE_USB_HOST };","enum phy_mode { PHY_MODE_USB_HOST, PHY_MODE_USB_HOST_SS };")
 s=s.replace("static int phy_set_mode(","static unsigned mode_fail_on_call;\nstatic int phy_set_mode(")
 s=s.replace("mode_calls++; return mode_errors[p->dev.unused];", "mode_calls++; return (!mode_fail_on_call||mode_calls==mode_fail_on_call)?mode_errors[p->dev.unused]:0;")
 s+=r'''
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
struct platform_device { struct device dev; };
static struct platform_device platform;
#define to_platform_device(d) (&platform)
struct usb_phy { unsigned unused; };
struct s3c_hsotg_plat { int (*phy_init)(struct platform_device *,int);
 int (*phy_exit)(struct platform_device *,int); int phy_type; };
struct clk { unsigned index; };
struct regulator_bulk_data { unsigned index; };
struct dwc2_hsotg {
 struct device *dev; struct phy *phy; struct usb_phy *uphy;
 struct s3c_hsotg_plat *plat; struct clk *utmi_clk,*clk;
 struct regulator_bulk_data supplies[2]; bool ll_hw_enabled;
};
static struct clk clocks[2]={{0},{1}};
static unsigned clock_refs[2],clock_enable_calls[2],clock_disable_calls[2],bulk_refs;
static int clock_errors[2],bulk_enable_error,bulk_disable_error;
static int clk_prepare_enable(struct clk *c) {
 clock_enable_calls[c->index]++;if(clock_errors[c->index])return clock_errors[c->index];
 clock_refs[c->index]++;return 0;
}
static void clk_disable_unprepare(struct clk *c) {
 assert(clock_refs[c->index]);clock_refs[c->index]--;clock_disable_calls[c->index]++;
}
static int regulator_bulk_enable(unsigned n,struct regulator_bulk_data *supplies) {
 (void)supplies;assert(n==2);if(bulk_enable_error)return bulk_enable_error;bulk_refs++;return 0;
}
static int regulator_bulk_disable(unsigned n,struct regulator_bulk_data *supplies) {
 (void)supplies;assert(n==2);if(bulk_disable_error)return bulk_disable_error;
 assert(bulk_refs);bulk_refs--;return 0;
}
static int usb_phy_init(struct usb_phy *phy) { (void)phy;assert(0);return -ENOTSUPP; }
static void usb_phy_shutdown(struct usb_phy *phy) { (void)phy;assert(0); }
'''
 for name in ('__dwc2_lowlevel_hw_enable','dwc2_lowlevel_hw_enable','__dwc2_lowlevel_hw_disable','dwc2_lowlevel_hw_disable'):
  s+='\n'+base.extract(source,('static int ' if name.startswith('__') else 'int ')+name)
 # Exact actual S5L OTG exit callback (a success-only stub), with provider init
 # replaced by a fault-injectable no-MMIO fake. No host/HSIC power callbacks.
 s+='\n'+base.extract((ROOT/'patches/files/drivers/phy/phy-apple-s5l-usb.c').read_text(),'static int s5l_usbphy_exit')
 s+=r'''
static const struct phy_ops p105_otg_ops={.init=fixture_init,.exit=s5l_usbphy_exit};
static void setup_dwc2(struct dwc2_hsotg *h,struct phy *phy,struct s5l_usbphy *p,uint32_t *complex,bool with_clocks) {
 fixture_reset(phy,p,complex,1);phy->ops=&p105_otg_ops;phy->dev.pm_enabled=false;
 memset(h,0,sizeof(*h));h->phy=phy;h->dev=&platform.dev;
 if(with_clocks){h->utmi_clk=&clocks[0];h->clk=&clocks[1];}
 memset(clock_refs,0,sizeof(clock_refs));memset(clock_enable_calls,0,sizeof(clock_enable_calls));
 memset(clock_disable_calls,0,sizeof(clock_disable_calls));memset(clock_errors,0,sizeof(clock_errors));
 bulk_refs=0;bulk_enable_error=bulk_disable_error=0;
}
'''
 hcd_source=consumer.core.usb_phy_candidate.sources()['drivers/usb/core/hcd.c']
 add=base.extract(hcd_source,'int usb_add_hcd')
 initial_start=add.index('\tif (!hcd->skip_phy_initialization)')
 initial_end=add.index('\n\tdev_info(',initial_start)
 initial=add[initial_start:initial_end]
 cleanup_start=add.index('err_create_buf:')
 cleanup_end=add.index('\n\treturn retval;',cleanup_start)
 cleanup=add[cleanup_start:cleanup_end]
 s+=r'''
struct usb_hcd { bool skip_phy_initialization,primary;
 struct { struct device *sysdev; } self;struct usb_phy_roothub *phy_roothub; };
static struct usb_phy_roothub *allocated;
static bool usb_hcd_is_primary_hcd(struct usb_hcd *h) { return h->primary; }
static struct usb_phy_roothub *usb_phy_roothub_alloc(struct device *d) { (void)d;return allocated; }
static struct usb_phy_roothub *usb_phy_roothub_alloc_usb3_phy(struct device *d) { (void)d;return allocated; }
/* Exact production USB add PHY block/error tail. Other HCD stages injected. */
static int add_phy_excerpt(struct usb_hcd *hcd,int downstream_error) {
 int retval,phy_status;
'''+initial+'''
 if(!downstream_error)return 0;
 retval=downstream_error;goto err_create_buf;
'''+cleanup+'\n return retval;\n}\n'
 remove=base.extract(hcd_source,'void usb_remove_hcd')
 start=remove.index('\tphy_status = usb_phy_roothub_power_off(')
 end=remove.index('\n\tusb_put_invalidate_rhdev(',start)
 s+='\nstatic void remove_phy_excerpt(struct usb_hcd *hcd) {\nint phy_status;\n'+remove[start:end]+'\n}\n'
 s+='''
static void setup_hcd(struct usb_hcd *hcd,struct usb_phy_roothub *root) {
 memset(hcd,0,sizeof(*hcd));hcd->self.sysdev=&platform.dev;hcd->primary=true;allocated=root;
}
'''

 return s

class DWC2(unittest.TestCase):
 def run_c(self,body,legacy=False,historical_mode=False):
  prefix='''struct dwc2_hsotg hsotg;struct phy phy;struct s5l_usbphy p;uint32_t complex;
 (void)root_setup;(void)usb_phy_roothub_init;(void)usb_phy_roothub_exit;
 (void)usb_phy_roothub_set_mode;(void)usb_phy_roothub_power_on;(void)usb_phy_roothub_power_off;
 (void)usb_phy_roothub_suspend;(void)usb_phy_roothub_resume;(void)usb_phy_roothub_needs_resume;
 (void)s5l_hostphy_power_on;(void)s5l_hostphy_power_off;(void)setup;
 (void)add_phy_excerpt;(void)remove_phy_excerpt;(void)setup_hcd;
'''
  text=harness(legacy,historical_mode)+'\nint main(void){\n'+prefix+body+'\nreturn 0;\n}\n'
  with tempfile.TemporaryDirectory(prefix='dwc2-ownership-') as directory:
   source=Path(directory)/'test.c';binary=Path(directory)/'test';source.write_text(text)
   result=subprocess.run(['cc','-std=gnu11','-O1','-g','-Wall','-Wextra','-Werror','-Wno-unused-parameter','-fsanitize=undefined',str(source),'-o',str(binary)],capture_output=True,text=True,timeout=30)
   self.assertEqual(result.returncode,0,result.stderr)
   result=subprocess.run([str(binary)],capture_output=True,text=True,timeout=30)
   self.assertEqual(result.returncode,0,result.stderr)

 def test_p105_dual_platform_hcd_acquire_then_remove_balances_one_provider_init(self):
  for legacy in (False,True):
   self.run_c('''
 setup_dwc2(&hsotg,&phy,&p,&complex,false);
 struct usb_phy_roothub root,entry;struct phy *refs[]={&phy};root_setup(&root,&entry,refs,1);
 assert(!dwc2_lowlevel_hw_enable(&hsotg));
 assert(hsotg.ll_hw_enabled && phy.init_count==1 && phy.power_count==1 && bulk_refs==1);
 struct usb_hcd hcd;setup_hcd(&hcd,&root);assert(!add_phy_excerpt(&hcd,0));
 assert(phy.init_count==2 && phy.power_count==2);
 assert(init_calls[0]==1 && !runtime_refs);
 remove_phy_excerpt(&hcd);
 assert(phy.init_count==1 && phy.power_count==1 && hsotg.ll_hw_enabled);
 assert(!dwc2_lowlevel_hw_disable(&hsotg));
 assert(!phy.init_count && !phy.power_count && !bulk_refs && !hsotg.ll_hw_enabled);
''',legacy)

 def test_p105_phy_init_gate_failure_unwinds_bulk_before_probe_returns(self):
  for legacy in (False,True):
   self.run_c('''
 setup_dwc2(&hsotg,&phy,&p,&complex,false);init_errors[0]=-ETIMEDOUT;
 assert(dwc2_lowlevel_hw_enable(&hsotg)==-ETIMEDOUT);
 assert(!phy.init_count && !phy.power_count && !bulk_refs && !hsotg.ll_hw_enabled);
 assert(init_calls[0]==1 && !runtime_refs);
''',legacy)

 def test_downstream_hcd_failure_releases_only_roothub_then_platform(self):
  self.run_c('''
 setup_dwc2(&hsotg,&phy,&p,&complex,false);
 struct usb_phy_roothub root,entry;struct phy *refs[]={&phy};root_setup(&root,&entry,refs,1);
 assert(!dwc2_lowlevel_hw_enable(&hsotg));
 struct usb_hcd hcd;setup_hcd(&hcd,&root);
 assert(add_phy_excerpt(&hcd,-ENOMEM)==-ENOMEM);
 assert(!entry.powered && !entry.initialized && phy.init_count==1 && phy.power_count==1);
 assert(!dwc2_lowlevel_hw_disable(&hsotg));
 assert(!phy.init_count && !phy.power_count && !bulk_refs);
''')

 def test_hcd_mode_failure_cannot_release_platform_power_reference(self):
  self.run_c('''
 setup_dwc2(&hsotg,&phy,&p,&complex,false);
 struct usb_phy_roothub root,entry;struct phy *refs[]={&phy};root_setup(&root,&entry,refs,1);
 assert(!dwc2_lowlevel_hw_enable(&hsotg));struct usb_hcd hcd;setup_hcd(&hcd,&root);
 mode_errors[0]=-EACCES;assert(add_phy_excerpt(&hcd,0)==-EACCES);
 assert(phy.power_count==1 && phy.init_count==1 && !entry.initialized);
 assert(!dwc2_lowlevel_hw_disable(&hsotg) && !phy.init_count && !phy.power_count && !bulk_refs);
''')

 def test_generic_optional_clocks_enable_failures_unwind_only_acquired_refs(self):
  for legacy in (False,True):
   self.run_c('''
 setup_dwc2(&hsotg,&phy,&p,&complex,true);clock_errors[0]=-EIO;
 assert(dwc2_lowlevel_hw_enable(&hsotg)==-EIO && !bulk_refs && !clock_refs[0] && !clock_refs[1]);
 assert(!clock_disable_calls[0] && !clock_disable_calls[1] && !phy.init_count);
 setup_dwc2(&hsotg,&phy,&p,&complex,true);clock_errors[1]=-ETIMEDOUT;
 assert(dwc2_lowlevel_hw_enable(&hsotg)==-ETIMEDOUT && !bulk_refs && !clock_refs[0] && !clock_refs[1]);
 assert(clock_disable_calls[0]==1 && !clock_disable_calls[1] && !phy.init_count);
 setup_dwc2(&hsotg,&phy,&p,&complex,true);init_errors[0]=-EACCES;
 assert(dwc2_lowlevel_hw_enable(&hsotg)==-EACCES && !bulk_refs && !clock_refs[0] && !clock_refs[1]);
 assert(clock_disable_calls[0]==1 && clock_disable_calls[1]==1 && !phy.init_count);
''',legacy)

 def test_generic_bulk_disable_failure_is_not_atomic_or_retryable_without_state(self):
  self.run_c('''
 setup_dwc2(&hsotg,&phy,&p,&complex,true);assert(!dwc2_lowlevel_hw_enable(&hsotg));
 bulk_disable_error=-EIO;
 assert(dwc2_lowlevel_hw_disable(&hsotg)==-EIO && hsotg.ll_hw_enabled);
 assert(!phy.power_count && !phy.init_count && !clock_refs[0] && !clock_refs[1] && bulk_refs==1);
 bulk_disable_error=0;
 assert(dwc2_lowlevel_hw_disable(&hsotg)==-EINVAL && hsotg.ll_hw_enabled && bulk_refs==1);
''')


 def test_exact_old_set_mode_control_consumes_other_platform_reference(self):
  self.run_c('''
 setup_dwc2(&hsotg,&phy,&p,&complex,false);
 struct usb_phy_roothub root,entries[2];struct phy *refs[]={&phy,&phy};root_setup(&root,entries,refs,2);
 assert(!dwc2_lowlevel_hw_enable(&hsotg));assert(!usb_phy_roothub_init(&root));
 assert(phy.init_count==3 && phy.power_count==1 && !entries[0].powered && !entries[1].powered);
 mode_errors[0]=-EACCES;mode_fail_on_call=2;
 assert(usb_phy_roothub_set_mode(&root,PHY_MODE_USB_HOST)==-EACCES);
 assert(!phy.power_count && hsotg.ll_hw_enabled && bulk_refs==1);
 assert(!usb_phy_roothub_exit(&root) && phy.init_count==1);
 /* Exact historical set-mode function, current core/wrapper around it.
  * No claim this is the complete original kernel or current P105 mode ops. */
''',historical_mode=True)


 def test_current_two_entry_mode_failure_preserves_other_platform_reference(self):
  self.run_c('''
 setup_dwc2(&hsotg,&phy,&p,&complex,false);
 struct usb_phy_roothub root,entries[2];struct phy *refs[]={&phy,&phy};root_setup(&root,entries,refs,2);
 assert(!dwc2_lowlevel_hw_enable(&hsotg));assert(!usb_phy_roothub_init(&root));
 assert(phy.init_count==3 && phy.power_count==1);
 mode_errors[0]=-EACCES;mode_fail_on_call=2;
 assert(usb_phy_roothub_set_mode(&root,PHY_MODE_USB_HOST)==-EACCES);
 assert(phy.power_count==1 && hsotg.ll_hw_enabled && bulk_refs==1);
 assert(!usb_phy_roothub_exit(&root) && phy.init_count==1);
 assert(!dwc2_lowlevel_hw_disable(&hsotg) && !phy.power_count && !phy.init_count && !bulk_refs);
''')

if __name__=='__main__':unittest.main()
