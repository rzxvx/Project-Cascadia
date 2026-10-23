from pathlib import Path
import re,shutil,subprocess,tempfile,unittest
ROOT=Path(__file__).resolve().parents[1]/'experiments/p105-current'

def function(s,n):
 pos=re.search(r'\b'+re.escape(n)+r'\(',s).start()
 a=s.rfind('\n',0,pos)+1;b=s.index('{',pos);depth=1;e=b+1
 while depth:depth+=(s[e]=='{')-(s[e]=='}');e+=1
 return s[a:e]

class Lifecycle(unittest.TestCase):
 @classmethod
 def setUpClass(cls):
  if not shutil.which('gcc'):raise unittest.SkipTest('gcc required')
  src=(ROOT/'apple-d1946-current-only.c').read_text()
  c=r'''
#define main core_original_main
#include "core-fixture.c"
#undef main
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <sched.h>
#define READ_ONCE(v) __atomic_load_n(&(v),__ATOMIC_RELAXED)
#define WRITE_ONCE(v,x) __atomic_store_n(&(v),(x),__ATOMIC_RELAXED)
#define IS_REACHABLE(x) 1
#define EPROBE_DEFER 517
#define ERR_PTR(v) ((void *)(intptr_t)(v))
#define IS_ERR_OR_NULL(v) (!(v)||(uintptr_t)(v)>(uintptr_t)-4096)
#define IS_ERR(v) ((uintptr_t)(v)>(uintptr_t)-4096)
#define PTR_ERR(v) ((int)(intptr_t)(v))
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#define dev_err(...) ((void)0)
struct mutex {pthread_mutex_t lock;};
typedef pthread_mutex_t spinlock_t;
static void mutex_lock(struct mutex *m){assert(!pthread_mutex_lock(&m->lock));}
static void mutex_unlock(struct mutex *m){assert(!pthread_mutex_unlock(&m->lock));}
#define spin_lock_irqsave(l,f) ((f)=0,(void)pthread_mutex_lock(l))
#define spin_unlock_irqrestore(l,f) ((void)(f),(void)pthread_mutex_unlock(l))
static void atomic_inc(unsigned int *v){__atomic_add_fetch(v,1,__ATOMIC_RELAXED);}
struct device_node {unsigned int refs;};
struct device {struct device_node *of_node;void *platform_data;unsigned int refs;};
struct platform_device {struct device dev;};
struct power_supply {unsigned int refs;void *data;};
static void *power_supply_get_drvdata(struct power_supply *p){return p->data;}
enum power_supply_property {POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT,POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT,POWER_SUPPLY_PROP_OTHER,POWER_SUPPLY_PROP_STATUS,POWER_SUPPLY_PROP_HEALTH};
#define POWER_SUPPLY_TYPE_USB 1
union power_supply_propval;
struct power_supply_desc {
 const char *name;int type;const enum power_supply_property *properties;unsigned int num_properties;
 int(*get_property)(struct power_supply *,enum power_supply_property,union power_supply_propval *);
 int(*set_property)(struct power_supply *,enum power_supply_property,const union power_supply_propval *);
 int(*property_is_writeable)(struct power_supply *,enum power_supply_property);
 void(*external_power_changed)(struct power_supply *);
};
union power_supply_propval {int intval;};
struct d1946_current_provider {int(*snapshot)(void);bool(*valid)(void);};
struct notifier_block {int(*notifier_call)(struct notifier_block *,unsigned long,void *);};
#define NOTIFY_OK 1
#define container_of(ptr,type,member) ((type*)((char*)(ptr)-__builtin_offsetof(type,member)))
struct p105_combined_provider {const struct d1946_current_provider *ops;struct notifier_block *subscription;};
#define D_NBANKS 9
static const unsigned int source_events[]={3,4,52};
struct d1946_current {
 struct mutex lifecycle_lock,io_lock,bus_lock;spinlock_t queue_lock;
 struct device *dev;struct d1946_io io;
 const struct d1946_current_provider *provider;struct p105_combined_provider *owned_provider;struct notifier_block owned_source_notifier;
 struct platform_device *source_pdev;struct power_supply *input,*battery,*psy;
 bool ready,stopping,events_enabled,fault_latched,parent_enabled,parent_failed,applied_valid,status_valid;
 int parent_irq,stop_error,work,virq[3];unsigned int epoch,requested_input,requested_charge;struct power_supply_desc desc;void *domain;u8 ceiling,owned[9],boot_masks[9];
};
static bool owned_usb2_source=true;
static struct device dev;
static struct platform_device source;
static struct device_node node;
static struct power_supply battery,input;
static int no_snapshot(void){return -EOPNOTSUPP;}
static bool no_valid(void){return false;}
static const struct d1946_current_provider ops={no_snapshot,no_valid};
static struct p105_combined_provider combined={.ops=&ops};
static bool missing_battery,missing_node,missing_source,null_battery,null_input;
static int factory_error;static unsigned int input_lookups,battery_lookups,factory_calls,closes;
static unsigned int synchronizes,cancels,disables,freed_children,freed_parent,domain_removes,unregisters;
static unsigned int sequence,cancel_seq,irq_seq,close_seq;
static bool queue_block,queue_entered,queue_release;
static unsigned int scheduled,pending;
static pthread_mutex_t gate=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cond=PTHREAD_COND_INITIALIZER;
static atomic_bool stop_started,stop_done;
static void *system_wq;
static void mod_delayed_work(void *wq,int *w,unsigned long delay){
 (void)wq;(void)delay;assert(*w==123);pthread_mutex_lock(&gate);
 scheduled++;pending=1;queue_entered=true;pthread_cond_broadcast(&cond);
 while(queue_block&&!queue_release)pthread_cond_wait(&cond,&gate);
 pthread_mutex_unlock(&gate);
}
/* Preserve pending work without delaying it, as queue_delayed_work does. */
static bool queue_delayed_work(void *wq,int *w,unsigned long delay){
 (void)wq;(void)delay;assert(*w==123);pthread_mutex_lock(&gate);
 bool queued=!pending;if(queued){scheduled++;pending=1;}
 pthread_mutex_unlock(&gate);return queued;
}
static void cancel_delayed_work_sync(int *w){
 assert(*w==123);pthread_mutex_lock(&gate);pending=0;cancels++;cancel_seq=++sequence;pthread_mutex_unlock(&gate);
}
static unsigned int enables;
static void enable_irq(int irq){(void)irq;enables++;}
static void disable_irq(int irq){assert(irq==17);disables++;}
static void synchronize_irq(int irq){assert(irq==17);synchronizes++;}
static void free_irq(int irq,void *d){(void)d;assert(!pending);if(irq==17){assert(synchronizes);freed_parent++;}else{assert(freed_parent);freed_children++;}irq_seq=++sequence;}
static void irq_dispose_mapping(int irq){assert(irq!=17);}
static void irq_domain_remove(void *domain){assert(domain);domain_removes++;}
static void power_supply_unregister(struct power_supply *p){assert(p&& !pending);unregisters++;}
static void *dev_get_platdata(struct device *d){return d->platform_data;}
static struct power_supply *power_supply_get_by_phandle(struct device_node *n,const char *name){
 assert(n==&node);if(!strcmp(name,"power-supplies")){input_lookups++;if(null_input)return NULL;input.refs++;return &input;}
 assert(!strcmp(name,"apple,battery-supply"));battery_lookups++;if(missing_battery)return ERR_PTR(-ENODEV);if(null_battery)return NULL;battery.refs++;return &battery;
}
static void power_supply_put(struct power_supply *p){assert(p->refs);p->refs--;}
static struct device_node *of_parse_phandle(struct device_node *n,const char *name,int index){assert(n==&node&&!strcmp(name,"apple,usb-source-controller")&&index==0);if(missing_node)return NULL;node.refs++;return &node;}
static struct platform_device *of_find_device_by_node(struct device_node *n){assert(n==&node);if(missing_source)return NULL;source.dev.refs++;return &source;}
static void of_node_put(struct device_node *n){assert(n->refs);n->refs--;}
static void put_device(struct device *d){assert(d->refs);d->refs--;}
/* Explicit factory ownership mock only. No positive budget/permission exists;
 * actual source/gauge factory and grant chain are tested in the separate group. */
static struct p105_combined_provider *p105_combined_provider_open(struct device *c,struct device *s,struct power_supply *b){assert(c==&dev&&s==&source.dev&&b==&battery);factory_calls++;return factory_error?ERR_PTR(factory_error):&combined;}
static int p105_combined_provider_register(struct p105_combined_provider *p,struct notifier_block *nb){assert(p==&combined&&!p->subscription&&nb->notifier_call);p->subscription=nb;return 0;}
static void p105_combined_provider_unregister(struct p105_combined_provider *p){assert(p==&combined);p->subscription=NULL;}
static const struct d1946_current_provider *p105_combined_provider_ops(struct p105_combined_provider *p){assert(p==&combined);return p->ops;}
static void p105_combined_provider_close(struct p105_combined_provider *p){assert(p==&combined&&!pending&&!p->subscription);assert(!READ_ONCE(((struct d1946_current *)dev.platform_data)->ready));closes++;close_seq=++sequence;}
'''
  for name in ['d_queue','d_invalidate','d_owned_source_changed','d_stop','d_release','d_providers_acquire','d_set']:c+=function(src,name)+'\n'
  c+=r'''
static const enum power_supply_property d_properties[]={POWER_SUPPLY_PROP_STATUS,POWER_SUPPLY_PROP_HEALTH,POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT,POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT};
static const char *dev_name(struct device *d){assert(d==&dev);return "experiment";}
static int d_get(struct power_supply *p,enum power_supply_property v,union power_supply_propval *o){(void)p;(void)v;(void)o;return -EOPNOTSUPP;}
static int d_writeable(struct power_supply *p,enum power_supply_property v){(void)p;return v==POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT||v==POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT;}
'''+function(src,'d_prepare_desc')+'\n'
  c+=r'''
static void init(struct d1946_current *d,struct fake *f){
 assert(!battery.refs&&!input.refs&&!source.dev.refs&&!node.refs);
 memset(d,0,sizeof(*d));assert(!pthread_mutex_init(&d->lifecycle_lock.lock,NULL));assert(!pthread_mutex_init(&d->io_lock.lock,NULL));assert(!pthread_mutex_init(&d->queue_lock,NULL));assert(!pthread_mutex_init(&d->bus_lock.lock,NULL));
 dev=(struct device){.of_node=&node,.platform_data=d};d->dev=&dev;d->parent_irq=-1;d->work=123;d->ceiling=63;setup(f,0xbf);d->io=(struct d1946_io){f,read_fake,write_fake};
 missing_battery=missing_node=missing_source=null_battery=null_input=false;factory_error=0;owned_usb2_source=true;combined.ops=&ops;
 input_lookups=battery_lookups=factory_calls=closes=cancels=disables=0;freed_children=freed_parent=domain_removes=unregisters=0;
 sequence=cancel_seq=irq_seq=close_seq=synchronizes=enables=0;scheduled=pending=0;queue_block=queue_entered=queue_release=false;
 atomic_store(&stop_started,false);atomic_store(&stop_done,false);
}
static void done(struct d1946_current *d){assert(!battery.refs&&!input.refs&&!source.dev.refs&&!node.refs&&!pending);pthread_mutex_destroy(&d->lifecycle_lock.lock);pthread_mutex_destroy(&d->io_lock.lock);pthread_mutex_destroy(&d->queue_lock);pthread_mutex_destroy(&d->bus_lock.lock);}
static void acquire_tests(void){
 struct d1946_current d;struct fake f;
 init(&d,&f);assert(!d_providers_acquire(&d)&&d.provider==&ops&&d.owned_provider==&combined&&d.input==NULL&&input_lookups==0&&battery.refs==1&&source.dev.refs==1);
 d_release(&d);assert(closes==1&&cancel_seq<close_seq&&f.calls==0);done(&d);
 init(&d,&f);missing_battery=true;assert(d_providers_acquire(&d)==-ENODEV&&!d.battery&&factory_calls==0);d_release(&d);assert(!closes&&f.calls==0);done(&d);
 init(&d,&f);null_battery=true;assert(d_providers_acquire(&d)==-EPROBE_DEFER&&!d.battery);d_release(&d);done(&d);
 init(&d,&f);missing_node=true;assert(d_providers_acquire(&d)==-EINVAL&&!d.source_pdev);d_release(&d);done(&d);
 init(&d,&f);missing_source=true;assert(d_providers_acquire(&d)==-EPROBE_DEFER&&!d.source_pdev&&node.refs==0);d_release(&d);done(&d);
 init(&d,&f);factory_error=-EPROBE_DEFER;assert(d_providers_acquire(&d)==-EPROBE_DEFER&&!d.owned_provider);d_release(&d);assert(!closes);done(&d);
 init(&d,&f);combined.ops=NULL;assert(d_providers_acquire(&d)==-ENODEV&&d.owned_provider==&combined);d_release(&d);assert(closes==1);done(&d);
 init(&d,&f);owned_usb2_source=false;dev.platform_data=(void*)&ops;assert(!d_providers_acquire(&d)&&d.provider==&ops&&d.input==&input&&input_lookups==1&&!factory_calls);
 d_release(&d);assert(!closes);done(&d);
 puts("actual acquisition/error unwind/no dummy supply fixtures PASS");
}
static void release_tests(void){
 struct d1946_current d;struct fake f;
 init(&d,&f);assert(!d_providers_acquire(&d));d.ready=d.events_enabled=d.parent_enabled=d.applied_valid=d.status_valid=true;d.parent_irq=17;d.virq[0]=31;d.virq[1]=32;d.virq[2]=33;d.domain=&d;d.psy=&input;
 d_invalidate(&d);assert(scheduled==1&&pending==1);d_release(&d);
 assert(d.stopping&&!d.ready&&!d.applied_valid&&!d.status_valid&&!d.fault_latched);
 assert(synchronizes==1&&disables==1&&cancels==1&&freed_children==3&&freed_parent==1&&unregisters==1&&domain_removes==1&&closes==1);
 assert(cancel_seq<irq_seq&&irq_seq<close_seq&&d.owned_provider==NULL&&d.provider==NULL);
 assert(f.regs[0x3f]==0xff&&f.regs[0x3d]==0);
 for(unsigned int i=0;i<100;i++)d_invalidate(&d);assert(scheduled==1&&pending==0);
 done(&d);
 init(&d,&f);assert(!d_providers_acquire(&d));d.ready=d.events_enabled=d.parent_enabled=true;d.parent_irq=17;f.fail_at=1;
 d_release(&d);assert(d.stop_error==-EREMOTEIO&&d.fault_latched&&closes==1&&cancels==1&&f.calls>1);done(&d);
 init(&d,&f);assert(!d_providers_acquire(&d));d.ready=d.events_enabled=d.parent_enabled=d.parent_failed=true;d.parent_irq=17;
 d_release(&d);assert(!disables&&synchronizes==1&&freed_parent==1&&closes==1);done(&d);
 puts("actual stop/IRQ drain/close/error physical unknown fixtures PASS");
}
static void *invalidate_thread(void *p){d_invalidate(p);return NULL;}
static void *stop_thread(void *p){atomic_store(&stop_started,true);assert(!d_stop(p));atomic_store(&stop_done,true);return NULL;}
static void race_tests(void){
 struct d1946_current d;struct fake f;pthread_t queue,stop;
 init(&d,&f);assert(!d_providers_acquire(&d));d.ready=d.events_enabled=d.parent_enabled=true;d.parent_irq=17;queue_block=true;
 assert(!pthread_create(&queue,NULL,invalidate_thread,&d));
 pthread_mutex_lock(&gate);while(!queue_entered)pthread_cond_wait(&cond,&gate);pthread_mutex_unlock(&gate);
 assert(!pthread_create(&stop,NULL,stop_thread,&d));while(!atomic_load(&stop_started))sched_yield();
 /* Queue call is held under the actual queue_lock: stop cannot pass its fence. */
 assert(!atomic_load(&stop_done));pthread_mutex_lock(&gate);queue_release=true;pthread_cond_broadcast(&cond);pthread_mutex_unlock(&gate);
 assert(!pthread_join(queue,NULL)&&!pthread_join(stop,NULL));assert(atomic_load(&stop_done)&&pending==0&&scheduled==1&&cancels==1);
 d_invalidate(&d);d_queue(&d,100,false);assert(scheduled==1&&pending==0);
 d_release(&d);assert(closes==1&&cancels==1&&cancel_seq<close_seq);done(&d);
 puts("actual queue/stop interleaving/no requeue after drain fixtures PASS");
}

static void preprobe_tests(void){
 struct d1946_current d;struct fake f;union power_supply_propval v={1000000};
 init(&d,&f);struct power_supply p={.data=&d};
 assert(d_set(&p,POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT,&v)==-EAGAIN&&enables==0&&scheduled==0&&f.calls==0&&!d.events_enabled&&!d.requested_input);
 d.ready=true;v.intval=39000;assert(d_set(&p,POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT,&v)==-EINVAL&&f.calls==0&&enables==0);
 d.stopping=true;v.intval=500000;assert(d_set(&p,POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT,&v)==-EIO&&enables==0);
 done(&d);puts("actual early publication/write guard fixtures PASS");
}
static void descriptor_tests(void){
 struct d1946_current d;struct fake f;init(&d,&f);
 for(unsigned int owned=0;owned<=1;owned++){
  owned_usb2_source=owned;d_prepare_desc(&d);
  assert(d.desc.external_power_changed==NULL&&d.desc.set_property==d_set&&d.desc.get_property==d_get&&d.desc.num_properties==4);
 }
 done(&d);puts("actual owned and legacy descriptors/no foreign work callback fixtures PASS");
}
static void unavailable_tests(void){
 struct d1946_current d;struct fake f;init(&d,&f);
 assert(d_providers_acquire(&d)==-EOPNOTSUPP&&!d.owned_provider&&!d.source_pdev&&factory_calls==0&&input_lookups==0);
 d_release(&d);assert(f.calls==0&&closes==0);done(&d);puts("actual unavailable adapter default denial fixtures PASS");
}
int main(int argc,char **argv){assert(argc==2);if(!strcmp(argv[1],"acquire"))acquire_tests();else if(!strcmp(argv[1],"release"))release_tests();else if(!strcmp(argv[1],"preprobe"))preprobe_tests();else if(!strcmp(argv[1],"unavailable"))unavailable_tests();else if(!strcmp(argv[1],"descriptor"))descriptor_tests();else race_tests();}

'''
  cls.tmp=tempfile.TemporaryDirectory();p=Path(cls.tmp.name);(p/'test.c').write_text(c);cls.bin=p/'test';cls.disabled=p/'test-disabled';(p/'test-disabled.c').write_text(c.replace('#define IS_REACHABLE(x) 1','#define IS_REACHABLE(x) 0'))
  subprocess.run(['gcc','-pthread','-DCASCADIA_OFFLINE','-std=gnu11','-Wall','-Wextra','-Werror','-Wno-unused-function','-Wno-misleading-indentation','-fsanitize=address,undefined','-fno-sanitize-recover=all','-I',str(ROOT),str(p/'test.c'),str(ROOT/'d1946-current-core.c'),'-o',str(cls.bin)],check=True)
  subprocess.run(['gcc','-pthread','-DCASCADIA_OFFLINE','-std=gnu11','-Wall','-Wextra','-Werror','-Wno-unused-function','-Wno-misleading-indentation','-fsanitize=address,undefined','-fno-sanitize-recover=all','-I',str(ROOT),str(p/'test-disabled.c'),str(ROOT/'d1946-current-core.c'),'-o',str(cls.disabled)],check=True)
 @classmethod
 def tearDownClass(cls):cls.tmp.cleanup()
 def run_fixture(self,n):self.assertIn('fixtures PASS',subprocess.check_output([str(self.bin),n],text=True))
 def test_actual_owned_source_acquisition(self):self.run_fixture('acquire')
 def test_actual_release_and_fault_latch(self):self.run_fixture('release')
 def test_actual_concurrent_queue_stop_fence(self):self.run_fixture('race')
 def test_actual_preprobe_property_write_guard(self):self.run_fixture('preprobe')
 def test_actual_both_route_descriptor_callbacks(self):self.run_fixture('descriptor')
 def test_actual_disabled_adapter(self):self.assertIn('fixtures PASS',subprocess.check_output([str(self.disabled),'unavailable'],text=True))
if __name__=='__main__':unittest.main()
