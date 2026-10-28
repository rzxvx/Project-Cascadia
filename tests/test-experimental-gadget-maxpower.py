from pathlib import Path
import hashlib,importlib.util,json,os,shutil,subprocess,tempfile,unittest,re
REPO=Path(__file__).resolve().parents[1]
HERE=REPO/'experiments/p105-gadget'
BASE_TEST=Path(os.environ.get('CASCADIA_TEST_SOURCE_PROVIDER',str(REPO/'tests/test-experimental-source-provider.py')))
LINUX=Path(os.environ.get('CASCADIA_TEST_LINUX',str(REPO/'build/linux')))
class Descriptor(unittest.TestCase):
 @classmethod
 def setUpClass(cls):
  if not shutil.which('gcc') or not BASE_TEST.exists() or not (LINUX/'drivers/usb/gadget/composite.c').exists():raise unittest.SkipTest('gcc, existing source-provider fixture and Linux6.12 source required')
  provenance=json.loads((HERE/'source-provenance.json').read_text())
  assert hashlib.sha256((LINUX/'drivers/usb/gadget/legacy/cdc2.c').read_bytes()).hexdigest()==provenance['original_cdc2_sha256']
  assert hashlib.sha256((LINUX/'drivers/usb/gadget/composite.c').read_bytes()).hexdigest()==provenance['original_composite_sha256']
  spec=importlib.util.spec_from_file_location('base',BASE_TEST);m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m);m.Combined.setUpClass();cls.base=m
  c=(Path(m.Combined.tmp.name)/'test.c').read_text();c=c[:c.index('int main(int argc,char **argv)')]
  c=c.replace('static void kfree(void *p){assert(allocs);allocs--;free(p);}', 'static void kfree(const void *p){if(!p)return;assert(allocs);allocs--;free((void*)p);}')
  c=c.replace('struct usb_gadget {int speed,state;};','struct usb_gadget {int speed,state,max_speed;bool is_selfpowered;struct device dev;const char *name;};')
  c=c.replace('#define MAX_CONFIG_INTERFACES 1','#define MAX_CONFIG_INTERFACES 2')
  c=c.replace('struct usb_function *interface[1];','struct usb_function *interface[2];const char *label;struct usb_composite_dev *cdev;const struct usb_descriptor_header **descriptors;unsigned int next_interface_id,iConfiguration;struct list_head functions;')
  old='static void usb_gadget_set_selfpowered(struct usb_gadget *g){(void)g;}\nstatic void usb_gadget_clear_selfpowered(struct usb_gadget *g){(void)g;}'
  gadget=(LINUX/'drivers/usb/dwc2/gadget.c').read_text()
  body=m.function(re.sub(r'/\*.*?\*/','',gadget,flags=re.S),'dwc2_hsotg_set_selfpowered')
  c=c.replace(old,'#define to_hsotg(g) container_of(g,struct dwc2_hsotg,gadget)\n'+body+'\nstatic void usb_gadget_set_selfpowered(struct usb_gadget *g){assert(!dwc2_hsotg_set_selfpowered(g,1));}\nstatic void usb_gadget_clear_selfpowered(struct usb_gadget *g){assert(!dwc2_hsotg_set_selfpowered(g,0));}')
  c+=r'''
#define __init
#define __exit
#define USB_CONFIG_ATT_ONE 0x80
#define USB_DT_CONFIG_SIZE 9
#define USB_COMP_EP0_BUFSIZ 256
#define USB_GADGET_MANUFACTURER_IDX 0
#define USB_GADGET_PRODUCT_IDX 1
#define DRIVER_DESC "CDC Composite Gadget"
#define DRIVER_VERSION "King Kamehameha Day2008"
#define cpu_to_le16(x) (x)
#define dev_info(...) ((void)0)
#define dev_err(...) ((void)0)
#define pr_info(...) ((void)0)
#define IS_ERR_OR_NULL(p) (!(p)||IS_ERR(p))
enum usb_device_speed {FIXTURE_UNKNOWN=0,FIXTURE_FULL=2,FIXTURE_HIGH=3,FIXTURE_SUPER=5};
struct usb_config_descriptor {u8 bLength,bDescriptorType;u16 wTotalLength;u8 bNumInterfaces,bConfigurationValue,iConfiguration,bmAttributes,bMaxPower;} __attribute__((packed));
static int usb_descriptor_fillbuf(void *next,int len,const struct usb_descriptor_header **desc){(void)next;(void)len;(void)desc;return 0;}
static struct usb_function *f_acm,*f_ecm;
struct usb_function_instance {int marker;};
struct f_ecm_opts {struct usb_function_instance func_inst;void *net;};
static struct usb_function_instance *fi_serial,*fi_ecm;
static struct f_ecm_opts ecm_instance;
static struct usb_function_instance serial_instance;
static struct usb_function ecm_function={.set_alt=set_alt},acm_function={.set_alt=set_alt};
static unsigned int qmult=5;
static const char *host_addr="02:00:00:00:00:01",*dev_addr="02:00:00:00:00:02";
struct usb_string {int id;};
static struct usb_string strings_dev[3];
static struct {int iManufacturer,iProduct;}device_desc;
static const struct usb_descriptor_header *otg_desc[2];
static int coverwrite;
static unsigned int usb2_maxpower;
static unsigned int probe_calls,instance_calls,function_calls,add_calls,put_instances,put_functions,ecm_checks,remove_calls;
static int instance_error;
static bool ecm_supported=true;
static bool can_support_ecm(struct usb_gadget *g){assert(g==&controller.gadget);ecm_checks++;return ecm_supported;}
static struct usb_function_instance *usb_get_function_instance(const char *name){instance_calls++;if(instance_calls==(unsigned int)instance_error)return ERR_PTR(-EIO);return !strcmp(name,"ecm")?&ecm_instance.func_inst:&serial_instance;}
static struct usb_function *usb_get_function(struct usb_function_instance *inst){function_calls++;return inst==&ecm_instance.func_inst?&ecm_function:&acm_function;}
static void usb_put_function(struct usb_function *f){assert(f);put_functions++;}
static void usb_put_function_instance(struct usb_function_instance *f){assert(f&&!IS_ERR(f));put_instances++;}
static int usb_add_function(struct usb_configuration *c,struct usb_function *f){assert(c->next_interface_id<2);c->interface[c->next_interface_id++]=f;add_calls++;return 0;}
static void usb_remove_function(struct usb_configuration *c,struct usb_function *f){(void)c;(void)f;remove_calls++;}
static void gether_set_qmult(void *net,unsigned int n){(void)net;assert(n==5);}
static int gether_set_host_addr(void *net,const char *addr){(void)net;assert(addr==host_addr);return 0;}
static int gether_set_dev_addr(void *net,const char *addr){(void)net;assert(addr==dev_addr);return 0;}
static int usb_string_ids_tab(struct usb_composite_dev *d,struct usb_string *s){assert(d==&cdev&&s==strings_dev);s[0].id=1;s[1].id=2;return 0;}
static struct usb_descriptor_header *usb_otg_descriptor_alloc(struct usb_gadget *g){(void)g;return NULL;}
static void usb_otg_descriptor_init(struct usb_gadget *g,struct usb_descriptor_header *desc){(void)g;(void)desc;}
static int usb_add_config(struct usb_composite_dev *d,struct usb_configuration *c,int(*bind)(struct usb_configuration *)){assert(d==&cdev);c->cdev=d;c->next_interface_id=0;memset(c->interface,0,sizeof(c->interface));d->configs.entry=c;return bind(c);}
static void usb_composite_overwrite_options(struct usb_composite_dev *d,void *opts){assert(d==&cdev&&opts==&coverwrite);}
struct usb_composite_driver {int(*bind)(struct usb_composite_dev *);int(*unbind)(struct usb_composite_dev *);};
static struct usb_composite_driver cdc_driver;
static int usb_composite_probe(struct usb_composite_driver *driver){assert(driver==&cdc_driver);probe_calls++;return driver->bind(&cdev);}
static void usb_composite_unregister(struct usb_composite_driver *driver){assert(driver==&cdc_driver);assert(!driver->unbind(&cdev));}
'''
  src=(HERE/'cdc2.c').read_text();a=src.index('static struct usb_configuration cdc_config_driver');b=src.index('};',a)+2;c+=src[a:b]+'\n'
  for name in ('cdc_validate_maxpower','cdc_do_config','cdc_bind','cdc_unbind','cdc_init','cdc_exit'):c+=m.function(src,name)+'\n'
  # Actual byte encoding and config descriptor construction, not a model thereof.
  composite=(LINUX/'drivers/usb/gadget/composite.c').read_text()
  for name in ('encode_bMaxPower','config_buf'):c+=m.function(composite,name)+'\n'
  c+=r'''
static void reset_cdc(void){
 init();controller.gadget.max_speed=USB_SPEED_HIGH;probe_calls=instance_calls=function_calls=add_calls=put_instances=put_functions=ecm_checks=remove_calls=0;instance_error=0;ecm_supported=true;
 cdc_driver=(struct usb_composite_driver){cdc_bind,cdc_unbind};cdc_config_driver=(struct usb_configuration){.label="CDC Composite (ECM+ACM)",.bConfigurationValue=1,.bmAttributes=USB_CONFIG_ATT_SELFPOWER};
}
static u64 configure_cdc(int result,bool complete,int status){
 struct usb_ctrlrequest ctrl={};u64 cookie=p105_udc_setup_begin(&controller.p105_power_observer,true,1);alt_result=result;
 int ret=set_config(&cdev,&ctrl,1);assert(ret==result);p105_udc_setup_result(&controller.p105_power_observer,cookie,ret,ret==USB_GADGET_DELAYED_STATUS);
 if(complete)p105_udc_status_complete(&controller.p105_power_observer,cookie,status);
 return cookie;
}
static void declaration_tests(void){
 unsigned int values[]={0,100,500};
 for(unsigned int i=0;i<3;i++)for(unsigned int speed=USB_SPEED_FULL;speed<=USB_SPEED_HIGH;speed++){
  reset_cdc();usb2_maxpower=values[i];controller.gadget.speed=speed;assert(!cdc_init()&&probe_calls==1&&instance_calls==2&&function_calls==2&&add_calls==2);
  assert(cdc_config_driver.MaxPower==values[i]&&cdc_config_driver.bmAttributes==USB_CONFIG_ATT_SELFPOWER);
  u8 bytes[256]={};assert(config_buf(&cdc_config_driver,speed,bytes,2)==9);
  assert(bytes[7]==(USB_CONFIG_ATT_ONE|USB_CONFIG_ATT_SELFPOWER)&&bytes[8]==values[i]/2);
  struct p105_dwc2_source_reader *r=p105_dwc2_source_open(&consumer,&source_device);assert(!IS_ERR(r));struct p105_usb_source_sample sample;
  assert(p105_dwc2_source_snapshot(r,&sample)==-EAGAIN);u64 cookie=configure_cdc(0,false,0);assert(requested_draw==values[i]&&p105_dwc2_source_snapshot(r,&sample)==-EAGAIN);
  p105_udc_status_complete(&controller.p105_power_observer,cookie,0);assert(!p105_dwc2_source_snapshot(r,&sample)&&sample.budget_ua==values[i]*1000&&p105_dwc2_source_valid(r,&sample));
  assert(controller.gadget.is_selfpowered==(values[i]<=100));
  p105_dwc2_source_close(r);cdc_exit();assert(put_instances==2&&put_functions==2);
 }
 puts("actual CDC bind/descriptors/libcomposite/EP0/defaultzero fixtures PASS");
}
static void invalid_tests(void){
 unsigned int values[]={1,99,101,499,501,512,1000,0xffffffff};
 for(unsigned int i=0;i<ARRAY_SIZE(values);i++){
  reset_cdc();usb2_maxpower=values[i];assert(cdc_init()==-EINVAL&&probe_calls==0&&ecm_checks==0&&instance_calls==0&&function_calls==0&&add_calls==0&&cdc_config_driver.MaxPower==0);
  assert(cdc_bind(&cdev)==-EINVAL&&instance_calls==0&&ecm_checks==0&&cdc_config_driver.MaxPower==0);
 }
 reset_cdc();usb2_maxpower=500;ecm_supported=false;assert(cdc_init()==-EINVAL&&probe_calls==1&&instance_calls==0);
 reset_cdc();usb2_maxpower=500;controller.gadget.max_speed=USB_SPEED_SUPER;assert(cdc_init()==-EOPNOTSUPP&&instance_calls==0&&function_calls==0&&cdc_config_driver.MaxPower==0);
 reset_cdc();usb2_maxpower=0;controller.gadget.max_speed=USB_SPEED_SUPER;assert(!cdc_init()&&cdc_config_driver.MaxPower==0);cdc_exit();
 reset_cdc();usb2_maxpower=500;instance_error=2;assert(cdc_init()==-EIO&&put_instances==1&&function_calls==0&&add_calls==0);
 puts("actual init prebind validation/early bind validation/unwind fixtures PASS");
}
static void incomplete_tests(void){
 reset_cdc();usb2_maxpower=500;assert(!cdc_init());struct p105_dwc2_source_reader *r=p105_dwc2_source_open(&consumer,&source_device);assert(!IS_ERR(r));struct p105_usb_source_sample sample;
 u64 cookie=configure_cdc(USB_GADGET_DELAYED_STATUS,false,0);assert(cdev.delayed_status==2&&p105_dwc2_source_snapshot(r,&sample)==-EAGAIN);
 p105_udc_status_complete(&controller.p105_power_observer,cookie,-EIO);assert(p105_dwc2_source_snapshot(r,&sample)==-EAGAIN);
 cookie=configure_cdc(-EIO,true,0);assert(p105_dwc2_source_snapshot(r,&sample)==-EAGAIN);
 cookie=configure_cdc(0,true,0);assert(!p105_dwc2_source_snapshot(r,&sample)&&sample.budget_ua==500000);
 p105_udc_invalidate(&controller.p105_power_observer,P105_UDC_SUSPEND);assert(!p105_dwc2_source_valid(r,&sample));
 /* No fresh config can be inferred from the ordinary resume draw fallback. */
 p105_udc_observe_draw(&controller.p105_power_observer,500);assert(p105_dwc2_source_snapshot(r,&sample)==-EAGAIN);
 configure_cdc(0,true,0);assert(!p105_dwc2_source_snapshot(r,&sample));controller.gadget.speed=USB_SPEED_SUPER;assert(!p105_dwc2_source_valid(r,&sample));
 p105_dwc2_source_close(r);cdc_exit();puts("actual delayed/error EP0/suspend/resume/superspeed denial fixtures PASS");
}
static void historical_zero_test(void){
 reset_cdc();usb2_maxpower=500;assert(!cdc_init());assert(cdc_config_driver.MaxPower==0);
 struct p105_dwc2_source_reader *r=p105_dwc2_source_open(&consumer,&source_device);assert(!IS_ERR(r));struct p105_usb_source_sample sample;
 u8 bytes[256]={};assert(config_buf(&cdc_config_driver,USB_SPEED_HIGH,bytes,2)==9&&bytes[8]==0);
 configure_cdc(0,true,0);assert(!p105_dwc2_source_snapshot(r,&sample)&&sample.budget_ua==0&&requested_draw==0);
 p105_dwc2_source_close(r);cdc_exit();puts("actual original CDC bind/config descriptor/completed zero budget fixtures PASS");
}
int main(int argc,char **argv){assert(argc==2);if(!strcmp(argv[1],"oldzero"))historical_zero_test();else if(!strcmp(argv[1],"declare"))declaration_tests();else if(!strcmp(argv[1],"invalid"))invalid_tests();else incomplete_tests();assert(!allocs&&!module_refs&&!supply_refs&&!consumer.refs&&!source_device.refs&&!gauge_device.refs);}
'''
  cls.tmp=tempfile.TemporaryDirectory();path=Path(cls.tmp.name);(path/'cdc.c').write_text(c);cls.bin=path/'cdc'
  subprocess.run(['gcc','-std=gnu11','-Wall','-Wextra','-Werror','-Wno-unused-function','-Wno-unused-parameter','-fsanitize=address,undefined','-fno-sanitize-recover=all','-O1','-g',str(path/'cdc.c'),'-o',str(cls.bin)],check=True)
  cls.old=path/'original-bind'
  historical=c.replace(m.function(src,'cdc_bind'),m.function((LINUX/'drivers/usb/gadget/legacy/cdc2.c').read_text(),'cdc_bind'))
  assert historical!=c
  (path/'original.c').write_text(historical)
  subprocess.run(['gcc','-std=gnu11','-Wall','-Wextra','-Werror','-Wno-unused-function','-Wno-unused-parameter','-fsanitize=address,undefined','-fno-sanitize-recover=all','-O1','-g',str(path/'original.c'),'-o',str(cls.old)],check=True)
 @classmethod
 def tearDownClass(cls):cls.tmp.cleanup();cls.base.Combined.tearDownClass()
 def run_case(self,name):self.assertIn('fixtures PASS',subprocess.check_output([str(self.bin),name],text=True,timeout=10))
 def test_actual_descriptor_and_completed_budget(self):self.run_case('declare')
 def test_actual_invalid_before_probe_and_bind(self):self.run_case('invalid')
 def test_actual_no_incomplete_or_suspended_budget(self):self.run_case('incomplete')
 def test_actual_original_bind_zero_budget_control(self):self.assertIn('fixtures PASS',subprocess.check_output([str(self.old),'oldzero'],text=True,timeout=10))
class Patch(unittest.TestCase):
 def test_strict_patch_and_exact_result(self):
  if not shutil.which('git') or not (LINUX/'drivers/usb/gadget/legacy/cdc2.c').exists():self.skipTest('git and original Linux CDC source needed')
  with tempfile.TemporaryDirectory()as directory:
   target=Path(directory)/'drivers/usb/gadget/legacy';target.mkdir(parents=True)
   shutil.copyfile(LINUX/'drivers/usb/gadget/legacy/cdc2.c',target/'cdc2.c')
   for args in (['--check'],[]):subprocess.run(['git','apply',*args,'--whitespace=error',str(HERE/'cdc2-maxpower.patch')],cwd=directory,check=True)
   self.assertEqual((target/'cdc2.c').read_bytes(),(HERE/'cdc2.c').read_bytes())
if __name__=='__main__':unittest.main()
