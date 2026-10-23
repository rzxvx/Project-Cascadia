from pathlib import Path
import hashlib,importlib.util,shutil,subprocess,tempfile,unittest,re,os,resource
ROOT = Path(__file__).resolve().parents[1] / 'experiments/p105-source'
CURRENT = ROOT.parent / 'p105-current'
LINUX = Path(os.environ.get('CASCADIA_TEST_LINUX', str(Path(__file__).resolve().parents[1] / 'build/linux')))

class Notifications(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not shutil.which('gcc') or not (LINUX / 'kernel/notifier.c').exists():
            raise unittest.SkipTest('gcc and original Linux notifier source required')
        assert hashlib.sha256((LINUX/'kernel/notifier.c').read_bytes()).hexdigest() == '1f4a1b2384a8751d780d39a748ef5beda21149f6e715af54df131ff9add2c44e'
        spec = importlib.util.spec_from_file_location('fixtures', Path(__file__).resolve().parent / 'test-experimental-source-provider.py')
        mod = importlib.util.module_from_spec(spec); spec.loader.exec_module(mod)
        mod.Combined.setUpClass()
        cls.base = mod
        c = (Path(mod.Combined.tmp.name) / 'test.c').read_text()
        c = c[:c.index('int main(int argc,char **argv)')]
        c = c.replace('#include <assert.h>', '#include <assert.h>\n#include <pthread.h>\n#include <stdatomic.h>')
        a = c.index('struct notifier_block {'); b = c.index('enum dl_dev_state')
        c = c[:a] + r'''
static _Thread_local unsigned int spin_depth;
typedef struct {pthread_mutex_t mutex;int locked;} spinlock_t;
struct mutex {pthread_mutex_t mutex;int locked;};
static void spin_lock_init(spinlock_t *l){assert(!pthread_mutex_init(&l->mutex,NULL));l->locked=0;}
static void lock(spinlock_t *l){assert(!pthread_mutex_lock(&l->mutex));l->locked=1;spin_depth++;}
static void unlock(spinlock_t *l){assert(l->locked&&spin_depth);l->locked=0;spin_depth--;assert(!pthread_mutex_unlock(&l->mutex));}
static int trylock(spinlock_t *l){if(pthread_mutex_trylock(&l->mutex))return 0;l->locked=1;spin_depth++;return 1;}
#define spin_lock_irqsave(l,f) ((f)=0,lock(l))
#define spin_unlock_irqrestore(l,f) ((void)(f),unlock(l))
#define spin_trylock_irqsave(l,f) ((f)=0,trylock(l))
static void mutex_init(struct mutex *l){assert(!pthread_mutex_init(&l->mutex,NULL));l->locked=0;}
static void mutex_lock(struct mutex *l){assert(!spin_depth);assert(!pthread_mutex_lock(&l->mutex));l->locked=1;}
static void mutex_unlock(struct mutex *l){assert(l->locked);l->locked=0;assert(!pthread_mutex_unlock(&l->mutex));}
struct notifier_block {int (*notifier_call)(struct notifier_block *,unsigned long,void *);struct notifier_block *next;int priority;};
struct atomic_notifier_head {spinlock_t lock;struct notifier_block *head;};
#define NOTIFY_OK 1
#define NOTIFY_DONE 0
#define NOTIFY_STOP_MASK 0x8000
#define container_of(ptr,type,member) ((type*)((char*)(ptr)-__builtin_offsetof(type,member)))
#define ATOMIC_INIT_NOTIFIER_HEAD(h) (spin_lock_init(&(h)->lock),(h)->head=NULL)
#define unlikely(x) (x)
#define WARN(...) ((void)0)
#define rcu_assign_pointer(p,v) __atomic_store_n(&(p),(v),__ATOMIC_RELEASE)
#define rcu_dereference_raw(p) __atomic_load_n(&(p),__ATOMIC_ACQUIRE)
#define trace_notifier_register(...) ((void)0)
#define trace_notifier_unregister(...) ((void)0)
#define trace_notifier_run(...) ((void)0)
/* Explicit userspace model of RCU grace periods, not kernel scheduling proof.
 * Actual notifier.c registration/call/unregister bodies are compiled below. */
static pthread_mutex_t rcu_mutex=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t rcu_cond=PTHREAD_COND_INITIALIZER;
static unsigned int rcu_readers,rcu_waiters;
static void rcu_read_lock(void){pthread_mutex_lock(&rcu_mutex);rcu_readers++;pthread_mutex_unlock(&rcu_mutex);}
static void rcu_read_unlock(void){pthread_mutex_lock(&rcu_mutex);assert(rcu_readers);rcu_readers--;pthread_cond_broadcast(&rcu_cond);pthread_mutex_unlock(&rcu_mutex);}
static void synchronize_rcu(void){assert(!spin_depth);pthread_mutex_lock(&rcu_mutex);rcu_waiters++;pthread_cond_broadcast(&rcu_cond);while(rcu_readers)pthread_cond_wait(&rcu_cond,&rcu_mutex);rcu_waiters--;pthread_mutex_unlock(&rcu_mutex);}
''' + '\n'.join(mod.function(re.sub(r'/\*.*?\*/', '', (LINUX / 'kernel/notifier.c').read_text(), flags=re.S), n) for n in [
            'notifier_chain_register', 'notifier_chain_unregister', 'notifier_call_chain',
            'atomic_notifier_chain_register', 'atomic_notifier_chain_unregister', 'atomic_notifier_call_chain']) + '\n' + c[b:]
        front = (CURRENT / 'apple-d1946-current-only.c').read_text()
        c += r'''
#define WRITE_ONCE(v,x) ((v)=(x))
#define IS_REACHABLE(x) 1
#define CONFIG_P105_COMBINED_CURRENT_PROVIDER 1
typedef _Atomic unsigned int atomic_t;
#define atomic_inc(p) atomic_fetch_add((p),1)
struct delayed_work {unsigned int queued;};
static struct d1946_current *active_front;
struct d1946_current {spinlock_t queue_lock;struct mutex lifecycle_lock,io_lock;bool ready,events_enabled,stopping,fault_latched,applied_valid,status_valid,parent_enabled,parent_failed;atomic_t epoch;struct delayed_work work;int parent_irq,stop_error,ceiling,io;struct device *dev;struct p105_combined_provider *owned_provider;struct notifier_block owned_source_notifier;};
static unsigned int mod_calls,cancel_calls,inhibit_calls,disable_calls,sync_calls;
static int io_error;
static void *system_wq;
static void mod_delayed_work(void *wq,struct delayed_work *w,unsigned long delay){(void)wq;(void)delay;assert(spin_depth);w->queued=1;mod_calls++;}
static bool queue_delayed_work(void *wq,struct delayed_work *w,unsigned long delay){if(w->queued)return false;mod_delayed_work(wq,w,delay);return true;}
static void cancel_delayed_work_sync(struct delayed_work *w){assert(!spin_depth);assert(active_front->stopping&&!active_front->ready&&!active_front->owned_provider->subscription);w->queued=0;cancel_calls++;}
static void disable_irq(int irq){assert(irq==19&&!spin_depth);disable_calls++;}
static void synchronize_irq(int irq){assert(irq==19&&!spin_depth);sync_calls++;}
static int d1946_current_limit(int *io,unsigned int current,unsigned int ceiling){(void)io;(void)ceiling;assert(!spin_depth&&current==0);inhibit_calls++;return io_error;}
static int d1946_input_limit(int *io,unsigned int input){(void)io;assert(!spin_depth&&input==0);inhibit_calls++;return io_error;}
#define dev_err(...) ((void)0)
'''
        c += '\n'.join(mod.function(front,n) for n in ['d_queue','d_invalidate','d_owned_source_changed','d_stop'])
        c += r'''
static pthread_mutex_t gate=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gate_cond=PTHREAD_COND_INITIALIZER;
static bool pause_callback,entered,released;
static unsigned int callback_calls;
static int test_callback(struct notifier_block *nb,unsigned long reason,void *v){
 assert(spin_depth);pthread_mutex_lock(&gate);callback_calls++;if(pause_callback){entered=true;pthread_cond_broadcast(&gate_cond);while(!released)pthread_cond_wait(&gate_cond,&gate);}pthread_mutex_unlock(&gate);
 return d_owned_source_changed(nb,reason,v);
}
static void front_init(struct d1946_current *d,struct p105_combined_provider *p){
 *d=(struct d1946_current){.ready=true,.events_enabled=true,.parent_irq=19,.parent_enabled=true,.owned_provider=p};
 spin_lock_init(&d->queue_lock);mutex_init(&d->lifecycle_lock);mutex_init(&d->io_lock);
 active_front=d;mod_calls=cancel_calls=inhibit_calls=disable_calls=sync_calls=callback_calls=0;io_error=0;
 pause_callback=entered=released=false;d->owned_source_notifier.notifier_call=test_callback;
 assert(!p105_combined_provider_register(p,&d->owned_source_notifier));
}
static void *event_thread(void *unused){(void)unused;p105_udc_invalidate(&controller.p105_power_observer,P105_UDC_DISCONNECT);return NULL;}
static void *stop_thread(void *d){assert(!d_stop(d));return NULL;}
static void *close_thread(void *p){p105_combined_provider_close(p);return NULL;}
static int paused_source_callback(struct notifier_block *nb,unsigned long why,void *v){
 pthread_mutex_lock(&gate);entered=true;pthread_cond_broadcast(&gate_cond);while(!released)pthread_cond_wait(&gate_cond,&gate);pthread_mutex_unlock(&gate);
 return p105_source_changed(nb,why,v);
}
static void source_close_tests(void){
 init();struct p105_combined_provider *p=p105_combined_provider_open(&consumer,&source_device,&psy);assert(!IS_ERR(p));
 struct d1946_current d;front_init(&d,p);assert(!d_stop(&d));
 p->source_notifier.notifier_call=paused_source_callback;
 pthread_t event,close;assert(!pthread_create(&event,NULL,event_thread,NULL));
 pthread_mutex_lock(&gate);while(!entered)pthread_cond_wait(&gate_cond,&gate);pthread_mutex_unlock(&gate);
 assert(!pthread_create(&close,NULL,close_thread,p));
 pthread_mutex_lock(&rcu_mutex);while(!rcu_waiters)pthread_cond_wait(&rcu_cond,&rcu_mutex);assert(rcu_readers==1);pthread_mutex_unlock(&rcu_mutex);
 assert(allocs==3&&module_refs==3&&consumer.refs==2);
 pthread_mutex_lock(&gate);released=true;pthread_cond_broadcast(&gate_cond);pthread_mutex_unlock(&gate);
 assert(!pthread_join(event,NULL)&&!pthread_join(close,NULL));
 assert(!allocs&&!controller.p105_power_observer.changes.head);
 puts("actual supplier callback drain before combined/gauge/source free fixtures PASS");
}
static void notification_tests(void){
 init();struct p105_combined_provider *p=p105_combined_provider_open(&consumer,&source_device,&psy);assert(!IS_ERR(p));
 struct d1946_current d;front_init(&d,p);
 assert(p105_combined_provider_register(p,&d.owned_source_notifier)==-EBUSY);
 assert(p105_dwc2_source_register(p->source,&p->source_notifier)==-EBUSY);
 for(unsigned int reason=P105_UDC_RESET;reason<=P105_UDC_REMOVE;reason++){
  p->valid=true;u64 serial=p->serial;unsigned int epoch=atomic_load(&d.epoch);
  p105_udc_invalidate(&controller.p105_power_observer,reason);
  assert(!p->valid&&p->serial==serial+1&&atomic_load(&d.epoch)==epoch+1&&callback_calls==reason&&mod_calls==reason&&inhibit_calls==0&&byte_reads==0);
 }
 /* Config start revokes; setup outcome alone does not complete permission. */
 unsigned int calls=callback_calls;
 u64 cookie=p105_udc_setup_begin(&controller.p105_power_observer,true,1);
 assert(callback_calls==calls+1);
 p105_udc_observe_draw(&controller.p105_power_observer,500);
 p105_udc_setup_result(&controller.p105_power_observer,cookie,0,false);assert(callback_calls==calls+1);
 p105_udc_status_complete(&controller.p105_power_observer,cookie,0);assert(callback_calls==calls+2);
 assert(!inhibit_calls&&!byte_reads);
 assert(!d_stop(&d)&&inhibit_calls==2&&cancel_calls==1&&sync_calls==1);
 calls=callback_calls;p105_udc_invalidate(&controller.p105_power_observer,P105_UDC_REMOVE);assert(callback_calls==calls);
 p105_combined_provider_close(p);assert(!controller.p105_power_observer.changes.head);
 puts("actual atomic event chain/config completion/stop fixtures PASS");
}
static void drain_tests(void){
 init();struct p105_combined_provider *p=p105_combined_provider_open(&consumer,&source_device,&psy);assert(!IS_ERR(p));
 struct d1946_current d;front_init(&d,p);pause_callback=true;
 pthread_t event,stop;assert(!pthread_create(&event,NULL,event_thread,NULL));
 pthread_mutex_lock(&gate);while(!entered)pthread_cond_wait(&gate_cond,&gate);pthread_mutex_unlock(&gate);
 assert(!pthread_create(&stop,NULL,stop_thread,&d));
 /* Deterministic grace-period barrier, no timing/sleep assumption. */
 pthread_mutex_lock(&rcu_mutex);while(!rcu_waiters)pthread_cond_wait(&rcu_cond,&rcu_mutex);assert(rcu_readers==2);pthread_mutex_unlock(&rcu_mutex);
 assert(d.stopping&&!d.ready&&mod_calls==0&&cancel_calls==0&&inhibit_calls==0);
 pthread_mutex_lock(&gate);released=true;pthread_cond_broadcast(&gate_cond);pthread_mutex_unlock(&gate);
 assert(!pthread_join(event,NULL)&&!pthread_join(stop,NULL));
 assert(mod_calls==0&&cancel_calls==1&&inhibit_calls==2&&atomic_load(&d.epoch)==2&&!p->subscription);
 p105_combined_provider_close(p);assert(!controller.p105_power_observer.changes.head);
 puts("actual paused callback/RCU drain/queue stop fence fixtures PASS");
}
static void unbind_tests(void){
 init();struct p105_combined_provider *p=p105_combined_provider_open(&consumer,&source_device,&psy);assert(!IS_ERR(p));
 struct d1946_current d;front_init(&d,p);p105_udc_invalidate(&controller.p105_power_observer,P105_UDC_ROLE);assert(mod_calls==1);
 consumer.links.status=DL_DEV_UNBINDING;source_link.status=gauge_link.status=DL_STATE_SUPPLIER_UNBIND;
 source_device.links.status=DL_DEV_UNBINDING;
 /* Registration denies new users, unregister must still reach retained context. */
 assert(p105_dwc2_source_register(p->source,&p->source_notifier)==-ENODEV);
 io_error=-EIO;assert(d_stop(&d)==-EIO&&d.fault_latched&&inhibit_calls==2&&!p->subscription);
 unsigned int calls=callback_calls;p105_udc_invalidate(&controller.p105_power_observer,P105_UDC_REMOVE);assert(callback_calls==calls);
 assert(d_stop(&d)==-EIO&&inhibit_calls==2);
 p105_combined_provider_close(p);assert(!controller.p105_power_observer.changes.head);
 puts("actual supplier unbind/failure/idempotent shutdown fixtures PASS");
}
int main(int argc,char **argv){assert(argc==2);
 if(!strcmp(argv[1],"events"))notification_tests();else if(!strcmp(argv[1],"drain"))drain_tests();else if(!strcmp(argv[1],"source-close"))source_close_tests();else unbind_tests();
 assert(!allocs&&!module_refs&&!supply_refs&&!consumer.refs&&!source_device.refs&&!gauge_device.refs&&!rcu_readers&&!rcu_waiters);
}
'''
        cls.tmp = tempfile.TemporaryDirectory(); directory=Path(cls.tmp.name)
        (directory/'notify.c').write_text(c);cls.bin=directory/'notify'
        subprocess.run(['gcc','-std=gnu11','-Wall','-Wextra','-Werror','-Wno-unused-function','-Wno-unused-parameter','-fsanitize=address,undefined','-fno-sanitize-recover=all','-O1','-g','-pthread',str(directory/'notify.c'),'-o',str(cls.bin)],check=True)
        cls.negative=directory/'historical'
        historical=(CURRENT/'historical-v4-d-stop.c').read_text()
        negative=c.replace(mod.function(front,'d_stop'),historical)
        assert negative!=c
        (directory/'historical.c').write_text(negative)
        subprocess.run(['gcc','-std=gnu11','-Wall','-Wextra','-Werror','-Wno-unused-function','-Wno-unused-parameter','-fsanitize=address,undefined','-fno-sanitize-recover=all','-O1','-g','-pthread',str(directory/'historical.c'),'-o',str(cls.negative)],check=True)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup();cls.base.Combined.tearDownClass()

    def run_fixture(self,name):
        self.assertIn('fixtures PASS',subprocess.check_output([str(self.bin),name],text=True,timeout=10))
    def test_actual_atomic_notification_chain(self):self.run_fixture('events')
    def test_actual_inflight_callback_drain_and_queue_stop(self):self.run_fixture('drain')
    def test_actual_unbind_and_failed_stop_lifetime(self):self.run_fixture('unbind')
    def test_actual_source_callback_drain_before_close(self):self.run_fixture('source-close')
    def test_historical_stop_without_notifier_drain_rejected(self):
        run=subprocess.run([str(self.negative),'events'],text=True,capture_output=True,timeout=10,preexec_fn=lambda:resource.setrlimit(resource.RLIMIT_CORE,(0,0)))
        self.assertNotEqual(run.returncode,0)
        self.assertIn('cancel_delayed_work_sync',run.stderr)
if __name__=='__main__':unittest.main()
