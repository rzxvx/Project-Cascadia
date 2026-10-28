from pathlib import Path
import hashlib,json,os,re,shutil,subprocess,tempfile,unittest
ROOT = Path(__file__).resolve().parents[1] / 'experiments/p105-source'
HDQ = Path(os.environ.get('CASCADIA_TEST_HDQ_READER', str(ROOT / 'apple-hdq-battery.c')))
LINUX = Path(os.environ.get('CASCADIA_TEST_LINUX', str(Path(__file__).resolve().parents[1] / 'build/linux')))


def clean(path):
    return '\n'.join(line for line in path.read_text().splitlines() if not line.startswith('#include'))


def function(source, name):
    pos = re.search(r'\b' + re.escape(name) + r'\(', source).start()
    start = source.rfind('\n', 0, pos) + 1
    if source[start:pos].strip() == '':
        start = source.rfind('\n', 0, start - 1) + 1
    body = source.index('{', pos)
    depth, end = 1, body + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


class Combined(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not shutil.which('gcc') or not HDQ.exists() or not (LINUX / 'drivers/usb/gadget/composite.c').exists():
            raise unittest.SkipTest('gcc, reviewed HDQ-v3 and Linux source are required')
        expected = json.loads((ROOT / 'source-provenance.json').read_text())
        assert hashlib.sha256((LINUX / 'drivers/usb/gadget/composite.c').read_bytes()).hexdigest() == expected['normal_composite_c_sha256']
        src = HDQ.read_text()
        c = r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
typedef uint8_t u8; typedef uint16_t u16; typedef int16_t s16; typedef uint64_t u64;
#define CONFIG_P105_DWC2_POWER_OBSERVER 1
#define EXPORT_SYMBOL_GPL(x)
#define MODULE_LICENSE(x)
#define MODULE_DESCRIPTION(x)
#define EPROBE_DEFER 517
#define ERR_PTR(v) ((void *)(intptr_t)(v))
#define IS_ERR(v) ((uintptr_t)(v)>(uintptr_t)-4096)
#define PTR_ERR(v) ((int)(intptr_t)(v))
#define READ_ONCE(v) (v)
#define THIS_MODULE ((void*)1)
#define GFP_KERNEL 0
#define DL_FLAG_AUTOREMOVE_CONSUMER 4
#define NSEC_PER_SEC 1000000000ULL
#define NSEC_PER_MSEC 1000000ULL
#define HZ 100
#define __iomem
#define BIT(n) (1U<<(n))
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#define min(a,b) ((a)<(b)?(a):(b))
#define time_before(a,b) ((long)((b)-(a))>0)
#define dev_warn_ratelimited(...) ((void)0)
struct notifier_block {int (*notifier_call)(struct notifier_block *,unsigned long,void *);struct notifier_block *next;};
struct atomic_notifier_head {struct notifier_block *head;};
#define NOTIFY_OK 1
#define container_of(ptr,type,member) ((type*)((char*)(ptr)-__builtin_offsetof(type,member)))
#define ATOMIC_INIT_NOTIFIER_HEAD(h) ((h)->head=NULL)
static int atomic_notifier_chain_register(struct atomic_notifier_head *h,struct notifier_block *n){n->next=h->head;h->head=n;return 0;}
static int atomic_notifier_chain_unregister(struct atomic_notifier_head *h,struct notifier_block *n){struct notifier_block **it=&h->head;while(*it&&*it!=n)it=&(*it)->next;assert(*it==n);*it=n->next;n->next=NULL;return 0;}
static int atomic_notifier_call_chain(struct atomic_notifier_head *h,unsigned long reason,void *v){for(struct notifier_block *n=h->head;n;n=n->next)assert(n->notifier_call(n,reason,v)==NOTIFY_OK);return NOTIFY_OK;}
typedef struct {int locked;} spinlock_t;
struct mutex {int locked;};
static void spin_lock_init(spinlock_t *l){l->locked=0;}
static void lock(spinlock_t *l){assert(!l->locked);l->locked=1;}
static void unlock(spinlock_t *l){assert(l->locked);l->locked=0;}
#define spin_lock_irqsave(l,f) ((f)=0,lock(l))
#define spin_unlock_irqrestore(l,f) ((void)(f),unlock(l))
#define spin_trylock_irqsave(l,f) ((f)=0,(l)->locked?0:((l)->locked=1))
static void mutex_lock(struct mutex *l){assert(!l->locked);l->locked=1;}
static void mutex_unlock(struct mutex *l){assert(l->locked);l->locked=0;}
enum dl_dev_state {DL_DEV_NO_DRIVER,DL_DEV_PROBING,DL_DEV_DRIVER_BOUND,DL_DEV_UNBINDING};
enum device_link_state {DL_STATE_DORMANT,DL_STATE_CONSUMER_PROBE,DL_STATE_ACTIVE,DL_STATE_SUPPLIER_UNBIND};
struct device_driver {int id;};
struct platform_driver {struct device_driver driver;};
struct device {bool platform;struct {enum dl_dev_state status;}links;struct device_driver *driver;void *data;unsigned int refs;};
struct device_link {enum device_link_state status;struct device *supplier;};
#define dev_is_platform(d) ((d)->platform)
struct power_supply_desc {const char *name;};
struct power_supply {struct {struct device *parent;}dev;const struct power_supply_desc *desc;void *data;};
union power_supply_propval { int intval; };
enum power_supply_property {
 POWER_SUPPLY_PROP_TECHNOLOGY, POWER_SUPPLY_PROP_PRESENT,
 POWER_SUPPLY_PROP_STATUS, POWER_SUPPLY_PROP_CAPACITY,
 POWER_SUPPLY_PROP_VOLTAGE_NOW, POWER_SUPPLY_PROP_CURRENT_NOW,
 POWER_SUPPLY_PROP_TEMP, POWER_SUPPLY_PROP_CHARGE_NOW,
 POWER_SUPPLY_PROP_CHARGE_FULL, POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN,
 POWER_SUPPLY_PROP_CYCLE_COUNT
};
enum { POWER_SUPPLY_TECHNOLOGY_LIPO, POWER_SUPPLY_STATUS_FULL,
 POWER_SUPPLY_STATUS_CHARGING, POWER_SUPPLY_STATUS_DISCHARGING,
 POWER_SUPPLY_STATUS_NOT_CHARGING };

static struct device consumer,source_device,gauge_device;
static struct device_link source_link,gauge_link;
static struct platform_driver hdq_driver,dwc2_platform_driver;
static const struct power_supply_desc hdq_desc={"battery"};
static struct power_supply *lookup;
static unsigned int module_refs,supply_refs,allocs,alloc_calls,context_reads;
static unsigned int allocation_fail_at,link_fail_at,link_calls;
static bool module_live=true;
static struct device_link *device_link_add(struct device *c,struct device *s,unsigned int flags){
 assert(c==&consumer&&flags==DL_FLAG_AUTOREMOVE_CONSUMER);
 if(++link_calls==link_fail_at)return NULL;
 return s==&source_device?&source_link:&gauge_link;
}
static void *dev_get_drvdata(struct device *d){context_reads++;return d->data;}
static void *power_supply_get_drvdata(struct power_supply *p){context_reads++;return p->data;}
static struct power_supply *power_supply_get_by_name(const char *n){assert(!strcmp(n,"battery"));if(lookup)supply_refs++;return lookup;}
static void power_supply_put(struct power_supply *p){assert(p&&supply_refs);supply_refs--;}
static struct device *get_device(struct device *d){d->refs++;return d;}
static void put_device(struct device *d){assert(d->refs);d->refs--;}
static bool try_module_get(void *m){(void)m;if(!module_live)return false;module_refs++;return true;}
static void module_put(void *m){(void)m;assert(module_refs);module_refs--;}
static void *kzalloc(unsigned int size,int flags){(void)flags;if(++alloc_calls==allocation_fail_at)return NULL;void *p=calloc(1,size);if(p)allocs++;return p;}
static void kfree(void *p){assert(allocs);allocs--;free(p);}
static u64 now_ns;static unsigned long jiffies;
static u64 ktime_get_ns(void){return now_ns;}
'''
        c += clean(ROOT / 'p105-udc-observer.h') + '\n' + clean(ROOT / 'p105-udc-observer.c')
        c += '\n' + clean(ROOT / 'p105-dwc2-source.h')
        c += '\n' + clean(ROOT / 'apple-hdq-snapshot.h')
        c += '\n' + clean(ROOT / 'd1946-current-provider.h')
        c += '\n' + clean(ROOT / 'p105-combined-provider.h')
        c += r'''
#define USB_DR_MODE_PERIPHERAL 2
#define USB_DR_MODE_HOST 1
#define DWC2_L0 0
#define USB_SPEED_UNKNOWN 0
#define USB_SPEED_FULL 2
#define USB_SPEED_HIGH 3
#define USB_SPEED_SUPER 5
#define USB_CONFIG_ATT_SELFPOWER 0x40
#define USB_CONFIG_ATT_WAKEUP 0x20
#define USB_SELF_POWER_VBUS_MAX_DRAW 100
#define USB_GADGET_DELAYED_STATUS 32767
#define CONFIG_USB_GADGET_VBUS_DRAW 2
#define MAX_CONFIG_INTERFACES 1
#define USB_DT_ENDPOINT 5
#define USB_STATE_CONFIGURED 5
#define DBG(...) ((void)0)
struct usb_ctrlrequest {int unused;};
struct usb_gadget {int speed,state;};
struct usb_gadget_driver {int(*setup)(struct usb_gadget *,const struct usb_ctrlrequest *);};
struct dwc2_hsotg {struct device *dev;spinlock_t lock;int dr_mode,lx_state;bool gadget_enabled,ll_hw_enabled,connected,test_mode,bus_suspended;struct usb_gadget_driver *driver;struct usb_gadget gadget;struct p105_udc_observer p105_power_observer;};
static int composite_setup(struct usb_gadget *g,const struct usb_ctrlrequest *r){(void)g;(void)r;return 0;}
static struct usb_gadget_driver composite_driver={composite_setup};
static struct dwc2_hsotg controller;
/* Same identity check as private platform.c, no source-budget mock. */
bool p105_dwc2_matches(struct device *d){return READ_ONCE(d->driver)==&dwc2_platform_driver.driver;}
struct usb_descriptor_header {unsigned char bDescriptorType;};
struct usb_endpoint_descriptor {unsigned char bDescriptorType,bEndpointAddress;};
struct usb_function {int(*set_alt)(struct usb_function *,unsigned int,unsigned int);unsigned long endpoints[1];};
struct list_head {void *entry;};
struct usb_configuration {unsigned int bConfigurationValue,MaxPower,bmAttributes;struct usb_function *interface[1];};
struct usb_composite_dev {struct usb_gadget *gadget;struct usb_configuration *config;struct list_head configs;int delayed_status;};
#define list_for_each_entry(p,h,m) for((p)=(h)->entry;(p);(p)=NULL)
static bool gadget_is_otg(struct usb_gadget *g){(void)g;return false;}
static void reset_config(struct usb_composite_dev *c){c->config=NULL;c->delayed_status=0;}
static void usb_gadget_set_state(struct usb_gadget *g,int state){g->state=state;}
static struct usb_descriptor_header *empty_desc[]={NULL};
static struct usb_descriptor_header **function_descriptors(struct usb_function *f,int speed){(void)f;(void)speed;return empty_desc;}
static void set_bit(unsigned int bit,unsigned long *bits){*bits|=1UL<<bit;}
static void usb_gadget_set_remote_wakeup(struct usb_gadget *g,int on){(void)g;(void)on;}
static void usb_gadget_set_selfpowered(struct usb_gadget *g){(void)g;}
static void usb_gadget_clear_selfpowered(struct usb_gadget *g){(void)g;}
static unsigned int requested_draw;
static int usb_gadget_vbus_draw(struct usb_gadget *g,unsigned int ma){(void)g;requested_draw=ma;p105_udc_observe_draw(&controller.p105_power_observer,ma);return -EOPNOTSUPP;}
static int alt_result;
static int set_alt(struct usb_function *f,unsigned int a,unsigned int b){(void)f;(void)a;(void)b;return alt_result;}
'''
        c += function((LINUX / 'drivers/usb/gadget/composite.c').read_text(), 'set_config') + '\n'
        c += clean(ROOT / 'p105-dwc2-source.c') + '\n'
        c += src[src.index('#define ULCON'):src.index('static void hdq_drain')]
        c += r'''
static unsigned int byte_reads,fail_byte,source_change_byte;
static unsigned int fixture_soc,fixture_rm,fixture_fcc;
static int fake_read8(struct hdq_battery *b,u8 reg){
 assert(b->lock.locked);byte_reads++;now_ns+=5000000;jiffies=now_ns/(NSEC_PER_SEC/HZ);
 if(byte_reads==source_change_byte)p105_udc_invalidate(&controller.p105_power_observer,P105_UDC_DISCONNECT);
 if(byte_reads==fail_byte)return -ETIMEDOUT;
 switch(reg&~1U){
 case BQ_SOC:return reg&1?fixture_soc>>8:fixture_soc&255;
 case BQ_VOLT:return reg&1?3900>>8:3900&255;
 case BQ_AI:return 255;
 case BQ_TEMP:return reg&1?2981>>8:2981&255;
 case BQ_RM:return reg&1?fixture_rm>>8:fixture_rm&255;
 case BQ_FCC:return reg&1?fixture_fcc>>8:fixture_fcc&255;
 case BQ_FLAGS:return 0;
 case BQ_CYCT:return reg&1?0:25;
 case BQ_DCAP:return reg&1?4400>>8:4400&255;
 default:assert(0);return -EIO;
 }
}
#define hdq_read8 fake_read8
'''
        for name in ['hdq_read16', 'hdq_invalidate_locked', 'hdq_refresh_locked', 'hdq_refresh', 'hdq_get_property', 'hdq_get_snapshot', 'hdq_snapshot_valid']:
            c += function(src, name) + '\n'
        c += '#undef hdq_read8\n'
        a = src.index('struct apple_hdq_reader {')
        c += src[a:src.index('};', a) + 2] + '\n'
        for name in ['hdq_reader_usable', 'apple_hdq_reader_open', 'apple_hdq_reader_close', 'apple_hdq_reader_snapshot', 'apple_hdq_reader_valid']:
            c += function(src, name) + '\n'
        c += clean(ROOT / 'p105-combined-provider.c') + '\n'
        c += r'''
static struct hdq_battery gauge;
static struct power_supply psy;
static struct usb_function f={.set_alt=set_alt};
static struct usb_configuration cfg;
static struct usb_composite_dev cdev;
static void init(void){
 assert(!module_refs&&!supply_refs&&!allocs&&!consumer.refs&&!source_device.refs&&!gauge_device.refs);
 consumer=(struct device){.platform=true,.links={DL_DEV_PROBING}};
 source_device=(struct device){.platform=true,.links={DL_DEV_DRIVER_BOUND},.driver=&dwc2_platform_driver.driver,.data=&controller};
 gauge_device=(struct device){.platform=true,.links={DL_DEV_DRIVER_BOUND},.driver=&hdq_driver.driver,.data=&gauge};
 source_link=(struct device_link){DL_STATE_CONSUMER_PROBE,&source_device};gauge_link=(struct device_link){DL_STATE_CONSUMER_PROBE,&gauge_device};
 controller=(struct dwc2_hsotg){.dev=&source_device,.dr_mode=USB_DR_MODE_PERIPHERAL,.lx_state=DWC2_L0,.gadget_enabled=true,.ll_hw_enabled=true,.connected=true,.driver=&composite_driver,.gadget={.speed=USB_SPEED_HIGH}};
 p105_udc_observer_init(&controller.p105_power_observer);
 psy=(struct power_supply){.dev={&gauge_device},.desc=&hdq_desc,.data=&gauge};
 gauge=(struct hdq_battery){.dev=&gauge_device,.psy=&psy};lookup=&psy;
 cfg=(struct usb_configuration){.bConfigurationValue=1,.MaxPower=500,.interface={&f}};
 cdev=(struct usb_composite_dev){.gadget=&controller.gadget,.configs={&cfg}};
 now_ns=NSEC_PER_SEC;jiffies=HZ;byte_reads=fail_byte=source_change_byte=0;alt_result=0;
 fixture_soc=64;fixture_rm=2500;fixture_fcc=4000;
 requested_draw=0;allocation_fail_at=alloc_calls=link_fail_at=link_calls=context_reads=0;module_live=true;
}
static u64 configure(unsigned int ma,int result,bool complete){
 struct usb_ctrlrequest ctrl={};
 u64 cookie=p105_udc_setup_begin(&controller.p105_power_observer,true,1);
 cfg.MaxPower=ma;cfg.bmAttributes=ma?0:USB_CONFIG_ATT_SELFPOWER;alt_result=result;
 int ret=set_config(&cdev,&ctrl,1);assert(ret==result);
 p105_udc_setup_result(&controller.p105_power_observer,cookie,ret,ret==USB_GADGET_DELAYED_STATUS);
 if(complete)p105_udc_status_complete(&controller.p105_power_observer,cookie,0);
 return cookie;
}
static void source_tests(void){
 struct p105_usb_source_sample s,old;struct p105_dwc2_source_reader *r;
 init();r=p105_dwc2_source_open(&consumer,&source_device);assert(!IS_ERR(r));
 memset(&s,0xa5,sizeof(s));old=s;assert(p105_dwc2_source_snapshot(r,&s)==-EAGAIN&&!memcmp(&s,&old,sizeof(s)));
 u64 cookie=configure(500,0,false);assert(requested_draw==500&&p105_dwc2_source_snapshot(r,&s)==-EAGAIN);
 p105_udc_status_complete(&controller.p105_power_observer,cookie,0);
 assert(!p105_dwc2_source_snapshot(r,&s)&&s.budget_ua==500000&&s.online&&p105_dwc2_source_valid(r,&s));
 old=s;configure(0,0,true);assert(!p105_dwc2_source_valid(r,&old));
 assert(!p105_dwc2_source_snapshot(r,&s)&&s.budget_ua==0&&s.online&&p105_dwc2_source_valid(r,&s));
 configure(2100,0,true);assert(requested_draw==500&&!p105_dwc2_source_snapshot(r,&s)&&s.budget_ua==500000);
 configure(100,USB_GADGET_DELAYED_STATUS,false);assert(p105_dwc2_source_snapshot(r,&s)==-EAGAIN);
 p105_udc_status_complete(&controller.p105_power_observer,p105_udc_pending_cookie(&controller.p105_power_observer),0);
 assert(!p105_dwc2_source_snapshot(r,&s)&&s.budget_ua==100000);
 configure(500,-EINVAL,true);assert(p105_dwc2_source_snapshot(r,&s)==-EAGAIN);
 p105_dwc2_source_close(r);puts("actual composite/EP0 budgets fixtures PASS");
}
static void invalidation_tests(void){
 struct p105_usb_source_sample s;init();struct p105_dwc2_source_reader *r=p105_dwc2_source_open(&consumer,&source_device);assert(!IS_ERR(r));
 for(unsigned int why=P105_UDC_RESET;why<=P105_UDC_REMOVE;why++){
  configure(500,0,true);assert(!p105_dwc2_source_snapshot(r,&s));p105_udc_invalidate(&controller.p105_power_observer,why);
  assert(!p105_dwc2_source_valid(r,&s)&&p105_dwc2_source_snapshot(r,&s)==-EAGAIN);
 }
 configure(500,0,true);assert(!p105_dwc2_source_snapshot(r,&s));
 controller.lock.locked=1;assert(!p105_dwc2_source_valid(r,&s));controller.lock.locked=0;
 controller.p105_power_observer.lock.locked=1;assert(!p105_dwc2_source_valid(r,&s));controller.p105_power_observer.lock.locked=0;
 controller.bus_suspended=true;assert(!p105_dwc2_source_valid(r,&s));controller.bus_suspended=false;
 controller.lx_state=2;assert(!p105_dwc2_source_valid(r,&s));controller.lx_state=0;
 controller.dr_mode=USB_DR_MODE_HOST;assert(!p105_dwc2_source_valid(r,&s));controller.dr_mode=USB_DR_MODE_PERIPHERAL;
 controller.driver=NULL;assert(!p105_dwc2_source_valid(r,&s));controller.driver=&composite_driver;
 controller.gadget.speed=USB_SPEED_UNKNOWN;assert(!p105_dwc2_source_valid(r,&s));controller.gadget.speed=USB_SPEED_HIGH;
 controller.connected=false;assert(!p105_dwc2_source_valid(r,&s));controller.connected=true;
 source_link.status=DL_STATE_SUPPLIER_UNBIND;assert(!p105_dwc2_source_valid(r,&s)&&p105_dwc2_source_snapshot(r,&s)==-ENODEV);
 source_link.status=DL_STATE_ACTIVE;assert(p105_dwc2_source_valid(r,&s));now_ns=s.valid_until_ns;assert(!p105_dwc2_source_valid(r,&s));
 p105_dwc2_source_close(r);puts("disconnect/role/suspend/unbind/lease/busy fixtures PASS");
}
static void lifecycle_tests(void){
 for(unsigned int fail=1;fail<=3;fail++){
  init();allocation_fail_at=fail;struct p105_combined_provider *p=p105_combined_provider_open(&consumer,&source_device,&psy);
  assert(IS_ERR(p)&&PTR_ERR(p)==-ENOMEM&&!module_refs&&!supply_refs&&!allocs&&!consumer.refs&&!source_device.refs&&!gauge_device.refs);
 }
 for(unsigned int fail=1;fail<=2;fail++){
  init();link_fail_at=fail;struct p105_combined_provider *p=p105_combined_provider_open(&consumer,&source_device,&psy);
  assert(IS_ERR(p)&&PTR_ERR(p)==-EPROBE_DEFER&&!module_refs&&!supply_refs&&!allocs&&!consumer.refs&&!source_device.refs&&!gauge_device.refs);
 }
 init();consumer.links.status=DL_DEV_DRIVER_BOUND;struct p105_combined_provider *p=p105_combined_provider_open(&consumer,&source_device,&psy);assert(IS_ERR(p)&&context_reads==0&&!allocs&&!module_refs);
 init();source_device.driver=NULL;p=p105_combined_provider_open(&consumer,&source_device,&psy);assert(IS_ERR(p)&&context_reads==0&&!allocs&&!module_refs);
 init();gauge_device.driver=NULL;p=p105_combined_provider_open(&consumer,&source_device,&psy);assert(IS_ERR(p)&&!allocs&&!module_refs&&!source_device.refs);
 init();p=p105_combined_provider_open(&consumer,&source_device,&psy);assert(!IS_ERR(p)&&module_refs==3&&allocs==3&&supply_refs==1&&consumer.refs==2);
 configure(500,0,true);struct d1946_snapshot s;const struct d1946_current_provider *ops=p105_combined_provider_ops(p);
 assert(!ops->snapshot(ops->context,NULL,&psy,&s)&&ops->valid(ops->context,&s));
 consumer.links.status=DL_DEV_UNBINDING;source_link.status=gauge_link.status=DL_STATE_SUPPLIER_UNBIND;
 source_device.data=gauge_device.data=psy.data=NULL;assert(!ops->valid(ops->context,&s));
 p105_combined_provider_close(p);assert(!allocs&&!module_refs&&!supply_refs&&!consumer.refs&&!source_device.refs&&!gauge_device.refs);
 puts("managed reader/error unwind/drain-close fixtures PASS");
}
static void combined_tests(void){
 init();configure(500,0,true);struct p105_combined_provider *p=p105_combined_provider_open(&consumer,&source_device,&psy);assert(!IS_ERR(p));
 const struct d1946_current_provider *ops=p105_combined_provider_ops(p);struct d1946_snapshot s,old;
 assert(!ops->snapshot(ops->context,NULL,&psy,&s)&&s.budget_ua==500000&&s.voltage_uv==3900000&&s.temp_decic==250&&s.present);
 assert(byte_reads==27&&s.sampled_ns==NSEC_PER_SEC&&s.valid_until_ns==now_ns+250*NSEC_PER_MSEC&&ops->valid(ops->context,&s));
 old=s;assert(!ops->snapshot(ops->context,NULL,&psy,&s)&&byte_reads==27&&!ops->valid(ops->context,&old));
 assert(ops->valid(ops->context,&s));old=s;s.budget_ua++;assert(!ops->valid(ops->context,&s));s=old;
 p->lock.locked=1;assert(!ops->valid(ops->context,&s));p->lock.locked=0;
 gauge.sample_lock.locked=1;assert(!ops->valid(ops->context,&s));gauge.sample_lock.locked=0;
 now_ns=s.valid_until_ns;assert(!ops->valid(ops->context,&s));
 assert(!ops->snapshot(ops->context,NULL,&psy,&s)&&byte_reads==27);
 hdq_invalidate_locked(&gauge);assert(!ops->valid(ops->context,&s));
 p105_combined_provider_close(p);puts("real gauge/source tuple/units/generation/age fixtures PASS");
}
static void transport_tests(void){
 init();configure(500,0,true);struct p105_combined_provider *p=p105_combined_provider_open(&consumer,&source_device,&psy);assert(!IS_ERR(p));
 const struct d1946_current_provider *ops=p105_combined_provider_ops(p);struct d1946_snapshot s,old;
 assert(!ops->snapshot(ops->context,NULL,&psy,&s));old=s;
 now_ns=gauge.sample_begin_ns+NSEC_PER_SEC;fail_byte=byte_reads+5;
 assert(ops->snapshot(ops->context,NULL,&psy,&s)==-ETIMEDOUT&&!memcmp(&old,&s,sizeof(s))&&!ops->valid(ops->context,&old));
 fail_byte=0;source_change_byte=byte_reads+4;memset(&s,0xa5,sizeof(s));old=s;
 assert(ops->snapshot(ops->context,NULL,&psy,&s)==-EAGAIN&&!memcmp(&old,&s,sizeof(s))&&!p->valid);
 source_change_byte=0;configure(500,0,true);assert(!ops->snapshot(ops->context,NULL,&psy,&s));old=s;
 gauge.suspended=true;hdq_invalidate_locked(&gauge);assert(!ops->valid(ops->context,&old));
 assert(ops->snapshot(ops->context,NULL,&psy,&s)==-EHOSTDOWN&&!memcmp(&old,&s,sizeof(s)));
 p105_combined_provider_close(p);puts("transport/partial burst/source changed/output retention fixtures PASS");
}

static void present_tests(void){
 init(); union power_supply_propval value={.intval=0x123456};
 fail_byte=1;
 assert(hdq_get_property(&psy,POWER_SUPPLY_PROP_PRESENT,&value)==-ETIMEDOUT);
 assert(value.intval==0x123456&&!gauge.lock.locked&&!gauge.valid);
 fail_byte=0;
 assert(!hdq_get_property(&psy,POWER_SUPPLY_PROP_PRESENT,&value));
 assert(value.intval==1&&gauge.valid&&!gauge.lock.locked);
 u64 generation=gauge.generation;
 now_ns+=6*NSEC_PER_SEC;jiffies=now_ns/(NSEC_PER_SEC/HZ);
 fail_byte=byte_reads+2;value.intval=0x654321;
 assert(hdq_get_property(&psy,POWER_SUPPLY_PROP_PRESENT,&value)==-ETIMEDOUT);
 assert(value.intval==0x654321&&!gauge.lock.locked&&!gauge.valid&&gauge.generation>generation);
 puts("PRESENT errno/output retention/invalidation fixtures PASS");
}

static void charge_trace_tests(void){
 init();struct apple_hdq_sample s,old;
 struct apple_hdq_reader *r=apple_hdq_reader_open(&consumer,&psy);assert(!IS_ERR(r));
 assert(!apple_hdq_reader_snapshot(r,NSEC_PER_SEC,&s));
 assert(s.capacity_percent==64&&s.charge_now_uah==2500000&&s.charge_full_uah==4000000);
 assert(s.current_ua==-1000&&s.gauge_present==1&&byte_reads==27);
 old=s;fixture_soc=65;fixture_rm=2510;fixture_fcc=4010;
 assert(!apple_hdq_reader_snapshot(r,NSEC_PER_SEC,&s)&&byte_reads==27);
 assert(s.generation==old.generation&&s.charge_now_uah==old.charge_now_uah);
 now_ns=s.valid_until_ns;
 assert(!apple_hdq_reader_valid(r,&s));
 assert(!apple_hdq_reader_snapshot(r,NSEC_PER_SEC,&s)&&byte_reads==54);
 assert(s.generation>old.generation&&!apple_hdq_reader_valid(r,&old));
 assert(s.capacity_percent==65&&s.charge_now_uah==2510000&&s.charge_full_uah==4010000);
 old=s;now_ns=s.valid_until_ns;fixture_rm=2520;fail_byte=byte_reads+17;
 assert(apple_hdq_reader_snapshot(r,NSEC_PER_SEC,&s)==-ETIMEDOUT);
 assert(!memcmp(&old,&s,sizeof(s))&&!apple_hdq_reader_valid(r,&old));
 fail_byte=0;fixture_soc=101;
 assert(apple_hdq_reader_snapshot(r,NSEC_PER_SEC,&s)==-ERANGE&&!memcmp(&old,&s,sizeof(s)));
 apple_hdq_reader_close(r);
 puts("charge trace complete burst/cache/expiry/partial failure/invalid SOC fixtures PASS");
}

int main(int argc,char **argv){assert(argc==2);
 if(!strcmp(argv[1],"charge-trace")){charge_trace_tests();assert(!allocs&&!module_refs&&!supply_refs&&!consumer.refs&&!source_device.refs&&!gauge_device.refs);return 0;}
 if(!strcmp(argv[1],"present"))present_tests();else if(!strcmp(argv[1],"source"))source_tests();else if(!strcmp(argv[1],"invalidate"))invalidation_tests();else if(!strcmp(argv[1],"lifetime"))lifecycle_tests();else if(!strcmp(argv[1],"combined"))combined_tests();else transport_tests();
 assert(!allocs&&!module_refs&&!supply_refs&&!consumer.refs&&!source_device.refs&&!gauge_device.refs);
}
'''
        cls.tmp = tempfile.TemporaryDirectory()
        path = Path(cls.tmp.name)
        (path / 'test.c').write_text(c)
        cls.bin = path / 'test'
        subprocess.run(['gcc', '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function', '-Wno-unused-parameter', '-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-O1', '-g', str(path / 'test.c'), '-o', str(cls.bin)], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def run_fixture(self, name):
        self.assertIn('fixtures PASS', subprocess.check_output([str(self.bin), name], text=True))

    def test_actual_completed_configuration_budget(self): self.run_fixture('source')
    def test_actual_source_invalidation_and_lease(self): self.run_fixture('invalidate')
    def test_actual_managed_handle_unwind_and_fence(self): self.run_fixture('lifetime')
    def test_actual_combined_source_and_hdq_tuple(self): self.run_fixture('combined')
    def test_actual_transport_errors_and_source_change(self): self.run_fixture('transport')
    def test_present_preserves_transport_errno_and_caller_output(self): self.run_fixture('present')
    def test_charge_trace_fields_share_complete_burst_and_expiry(self): self.run_fixture('charge-trace')

if __name__ == '__main__': unittest.main()
