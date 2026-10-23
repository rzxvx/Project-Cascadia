"""Compile full candidate HCD suspend/resume and actual add/remove PHY excerpts."""
from pathlib import Path
import importlib.util
import subprocess
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('consumer_fixture',ROOT/'tests/test-usb-roothub-ownership.py')
consumer=importlib.util.module_from_spec(spec);spec.loader.exec_module(consumer)
extract=consumer.core.base.extract


def harness():
    s=consumer.harness().replace('enum phy_mode { PHY_MODE_USB_HOST };',
                                 'enum phy_mode { PHY_MODE_USB_HOST, PHY_MODE_USB_HOST_SS };')
    s+=r'''
#undef dev_err
static unsigned errors;
static void error_log(struct device *dev,const char *fmt,...) { (void)dev; (void)fmt; errors++; }
#define dev_err(...) error_log(__VA_ARGS__)
#define dev_dbg(...) ((void)0)
#define PMSG_IS_AUTO(msg) ((msg)&1)
#define PMSG_AUTO_RESUME 1
#define PMSG_RESUME 0
typedef int pm_message_t;
enum { HC_STATE_RUNNING=1, HC_STATE_QUIESCING, HC_STATE_SUSPENDED, HC_STATE_RESUMING };
enum { USB_STATE_SUSPENDED=1, USB_STATE_CONFIGURED, USB_STATE_ADDRESS, USB_STATE_NOTATTACHED };
enum { HCD_FLAG_RH_RUNNING=1, HCD_FLAG_WAKEUP_PENDING=2 };
struct usb_hcd;
struct usb_bus { struct usb_hcd *owner; };
struct usb_device {
 struct device dev; struct usb_bus *bus; int state;
 bool do_remote_wakeup,port_is_suspended; void *actconfig;
};
struct hc_driver { int (*bus_suspend)(struct usb_hcd *); int (*bus_resume)(struct usb_hcd *);
 int (*hub_status_data)(struct usb_hcd *,char *); };
struct usb_hcd {
 struct { struct device *sysdev,*controller; } self;
 struct usb_phy_roothub *phy_roothub;
 const struct hc_driver *driver; int state;
 unsigned long flags; bool dead,skip_phy_initialization,primary;
 int suspend_error,resume_error; unsigned suspend_calls,resume_calls;
};
static int hcd_root_hub_lock;
#define spin_lock_irq(p) ((void)(p))
#define spin_unlock_irq(p) ((void)(p))
static void clear_bit(unsigned n,unsigned long *flags) { *flags&=~(1ul<<n); }
static void set_bit(unsigned n,unsigned long *flags) { *flags|=1ul<<n; }
#define HCD_DEAD(hcd) ((hcd)->dead)
#define HCD_RH_RUNNING(hcd) (((hcd)->flags&(1ul<<HCD_FLAG_RH_RUNNING))!=0)
static struct usb_hcd *bus_to_hcd(struct usb_bus *bus) { return bus->owner; }
static void usb_set_device_state(struct usb_device *d,int state) { d->state=state; }
static void usb_hc_died(struct usb_hcd *hcd) { hcd->dead=true; }
#define usb_hub_for_each_child(root,port,child) for((port)=0,(child)=NULL;(child);(port)++)
static void usleep_range(unsigned a,unsigned b) { assert(a==10000 && b==11000); }
static int usb_phy_roothub_calibrate(struct usb_phy_roothub *root) { (void)root; return 0; }
static int fixture_bus_suspend(struct usb_hcd *hcd) { hcd->suspend_calls++; return hcd->suspend_error; }
static int fixture_bus_resume(struct usb_hcd *hcd) { hcd->resume_calls++; return hcd->resume_error; }
static int fixture_hub_status(struct usb_hcd *hcd,char *buf) { (void)hcd; (void)buf; return 0; }
static const struct hc_driver hcd_ops={fixture_bus_suspend,fixture_bus_resume,fixture_hub_status};
int hcd_bus_resume(struct usb_device *,pm_message_t);
'''
    source=consumer.core.usb_phy_candidate.sources()['drivers/usb/core/hcd.c']
    s+='\n'+extract(source,'int hcd_bus_suspend')
    s+='\n'+extract(source,'int hcd_bus_resume')
    s+=r'''
static void hcd_setup(struct usb_hcd *hcd,struct usb_device *rhdev,struct usb_bus *bus,
                      struct device *controller,struct usb_phy_roothub *root) {
 memset(hcd,0,sizeof(*hcd)); memset(rhdev,0,sizeof(*rhdev));
 hcd->phy_roothub=root; hcd->driver=&hcd_ops; hcd->state=HC_STATE_RUNNING;
 hcd->self.sysdev=hcd->self.controller=controller; hcd->primary=true;
 set_bit(HCD_FLAG_RH_RUNNING,&hcd->flags); bus->owner=hcd; rhdev->bus=bus;
 rhdev->state=USB_STATE_CONFIGURED; rhdev->actconfig=bus; errors=0;
}
static struct usb_phy_roothub *allocated;
static bool usb_hcd_is_primary_hcd(struct usb_hcd *hcd) { return hcd->primary; }
static struct usb_phy_roothub *usb_phy_roothub_alloc(struct device *d) { (void)d; return allocated; }
static struct usb_phy_roothub *usb_phy_roothub_alloc_usb3_phy(struct device *d) { (void)d; return allocated; }
'''
    add=extract(source,'int usb_add_hcd')
    initial_start=add.index('\tif (!hcd->skip_phy_initialization)')
    initial_end=add.index('\n\tdev_info(',initial_start)
    initial=add[initial_start:initial_end]
    cleanup_start=add.index('err_create_buf:')
    cleanup_end=add.index('\n\treturn retval;',cleanup_start)
    cleanup=add[cleanup_start:cleanup_end]
    s+='''
/* The two PHY blocks below are verbatim production add/remove excerpts.
 * Unrelated bus/IRQ/DMA registration and teardown are not modeled. */
static int add_phy_excerpt(struct usb_hcd *hcd,int downstream_error)
{
 int retval,phy_status;
'''+initial+'''
 if(!downstream_error) return 0;
 retval=downstream_error; goto err_create_buf;
'''+cleanup+'\nreturn retval;\n}\n'
    remove=extract(source,'void usb_remove_hcd')
    begin=remove.index('\tphy_status = usb_phy_roothub_power_off(')
    end=remove.index('\n\tusb_put_invalidate_rhdev(',begin)
    s+='\nstatic void remove_phy_excerpt(struct usb_hcd *hcd) {\nint phy_status;\n'+remove[begin:end]+'\n}\n'
    return s


class HCD(unittest.TestCase):
    def run_c(self,body):
        setup=r'''
 struct phy phy; struct s5l_usbphy p; uint32_t complex; fixture_reset(&phy,&p,&complex,1);
 struct usb_phy_roothub root,entry; struct phy *refs[]={&phy}; root_setup(&root,&entry,refs,1);
 struct device controller={.unused=1}; struct usb_hcd hcd; struct usb_device rhdev; struct usb_bus bus;
 hcd_setup(&hcd,&rhdev,&bus,&controller,&root); allocated=&root;
 (void)add_phy_excerpt; (void)remove_phy_excerpt;
'''
        src=harness()+'\nint main(void) {\n'+setup+body+'\nreturn 0;\n}\n'
        with tempfile.TemporaryDirectory(prefix='hcd-phy-caller-') as td:
            c,binary=Path(td)/'caller.c',Path(td)/'caller'
            c.write_text(src)
            result=subprocess.run(['cc','-std=gnu11','-O1','-g','-Wall','-Wextra','-Werror',
                        '-fsanitize=undefined',str(c),'-o',str(binary)],text=True,capture_output=True)
            self.assertEqual(result.returncode,0,result.stderr)
            result=subprocess.run([str(binary)],text=True,capture_output=True)
            self.assertEqual(result.returncode,0,result.stderr)

    def test_suspend_error_propagates_and_auto_resume_must_recover_before_bus(self):
        self.run_c('''
 assert(!add_phy_excerpt(&hcd,0)); disable_errors[0]=-EIO;
 assert(hcd_bus_suspend(&rhdev,0)==-EIO && errors==1);
 assert(hcd.state==HC_STATE_SUSPENDED && rhdev.state==USB_STATE_SUSPENDED);
 assert(phy.power_count==1 && phy.init_count==1);
 assert(hcd_bus_resume(&rhdev,PMSG_AUTO_RESUME)==-EIO && !hcd.resume_calls);
 assert(hcd.state==HC_STATE_SUSPENDED && phy.power_count==1 && !p.hsic_supply_on);
 disable_errors[0]=0;
 assert(!hcd_bus_resume(&rhdev,PMSG_AUTO_RESUME));
 assert(hcd.resume_calls==1 && hcd.state==HC_STATE_RUNNING && HCD_RH_RUNNING(&hcd));
 assert(phy.power_count==1 && phy.init_count==1 && p.hsic_supply_on);
 remove_phy_excerpt(&hcd); assert(!phy.power_count && !phy.init_count && !runtime_refs);
''')

    def test_repeated_nonauto_resume_of_running_hcd_does_not_duplicate_refs(self):
        self.run_c('''
 assert(!add_phy_excerpt(&hcd,0));
 assert(!hcd_bus_resume(&rhdev,PMSG_RESUME) && !hcd.resume_calls);
 assert(!hcd_bus_resume(&rhdev,PMSG_RESUME) && !hcd.resume_calls);
 assert(phy.power_count==1 && phy.init_count==1 && runtime_refs==1);
''')

    def test_normal_auto_suspend_resume_keeps_owned_phy_without_callbacks(self):
        self.run_c('''
 assert(!add_phy_excerpt(&hcd,0)); unsigned before=disables[0]+disables[1];
 assert(!hcd_bus_suspend(&rhdev,1) && phy.power_count==1);
 assert(!hcd_bus_resume(&rhdev,1) && hcd.resume_calls==1);
 assert(disables[0]+disables[1]==before && phy.power_count==1 && phy.init_count==1);
''')

    def test_controller_suspend_failure_keeps_original_state_and_phy(self):
        self.run_c('''
 assert(!add_phy_excerpt(&hcd,0)); hcd.suspend_error=-EACCES;
 assert(hcd_bus_suspend(&rhdev,0)==-EACCES);
 assert(hcd.state==HC_STATE_RUNNING && HCD_RH_RUNNING(&hcd));
 assert(phy.power_count==1 && entry.powered && !entry.power_off_error && !errors);
''')

    def test_resume_controller_error_preserves_primary_and_logs_failed_phy_cleanup(self):
        self.run_c('''
 assert(!add_phy_excerpt(&hcd,0)); assert(!hcd_bus_suspend(&rhdev,0));
 hcd.resume_error=-EACCES; disable_errors[0]=-EIO;
 assert(hcd_bus_resume(&rhdev,0)==-EACCES);
 assert(hcd.dead && errors==1 && phy.power_count==1 && entry.power_off_error==-EIO);
 assert(entry.initialized && phy.init_count==1);
''')

    def test_add_downstream_failure_reports_cleanup_failure_without_false_put(self):
        self.run_c('''
 disable_errors[0]=-EIO;
 assert(add_phy_excerpt(&hcd,-ENOMEM)==-ENOMEM);
 assert(errors==2 && phy.power_count==1 && phy.init_count==1 && entry.powered);
 disable_errors[0]=0; remove_phy_excerpt(&hcd);
 assert(!phy.power_count && !phy.init_count && !runtime_refs);
''')

    def test_mode_failure_add_exit_does_not_power_off_unowned_phy(self):
        self.run_c('''
 mode_errors[0]=-EIO;
 assert(add_phy_excerpt(&hcd,0)==-EIO);
 assert(!phy.power_count && !phy.init_count && !runtime_refs && !errors);
 assert(!disables[0] && !disables[1]);
''')

    def test_permanent_remove_fault_is_logged_and_reference_is_retained(self):
        self.run_c('''
 assert(!add_phy_excerpt(&hcd,0)); disable_errors[0]=-EIO;
 remove_phy_excerpt(&hcd);
 assert(errors==2 && phy.power_count==1 && phy.init_count==1);
 assert(entry.powered && entry.initialized && phy.power_off_error==-EIO);
''')


if __name__=='__main__':unittest.main()
