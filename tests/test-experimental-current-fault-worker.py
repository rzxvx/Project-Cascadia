from pathlib import Path
import re,resource,shutil,subprocess,tempfile,unittest,hashlib
ROOT=Path(__file__).resolve().parents[1]/'experiments/p105-current'
SOURCE=ROOT/'apple-d1946-current-only.c'

def function(s,n):
 pos=re.search(r'\b'+re.escape(n)+r'\(',s).start();a=s.rfind('\n',0,pos)+1;b=s.index('{',pos);depth=1;e=b+1
 while depth:depth+=(s[e]=='{')-(s[e]=='}');e+=1
 return s[a:e]

class Worker(unittest.TestCase):
 @classmethod
 def setUpClass(cls):
  if not shutil.which('gcc'):raise unittest.SkipTest('gcc required')
  src=SOURCE.read_text()
  c=r'''
#define main core_original_main
#include "core-fixture.c"
#undef main
#include <stddef.h>
#include <stdint.h>
typedef uint64_t u64;
#define READ_ONCE(x) (x)
#define min(a,b) ((a)<(b)?(a):(b))
#define min_t(t,a,b) min((t)(a),(t)(b))
#define NSEC_PER_SEC 1000000000ULL
#define D_EXPERIMENT_MAX_MA 720
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#define dev_err(...) ((void)0)
#define container_of(p,t,m) ((t *)((char *)(p)-offsetof(t,m)))
struct work_struct {int marker;};
struct delayed_work {struct work_struct work;};
static struct delayed_work *to_delayed_work(struct work_struct *w){return container_of(w,struct delayed_work,work);}
struct mutex {int locked;};
typedef struct {int locked;}spinlock_t;
static void mutex_lock(struct mutex *l){assert(!l->locked);l->locked=1;}
static void mutex_unlock(struct mutex *l){assert(l->locked);l->locked=0;}
static void splock(spinlock_t *l){assert(!l->locked);l->locked=1;}
static void spunlock(spinlock_t *l){assert(l->locked);l->locked=0;}
#define spin_lock_irqsave(l,f) ((f)=0,splock(l))
#define spin_unlock_irqrestore(l,f) ((void)(f),spunlock(l))
static void atomic_inc(unsigned int *e){(*e)++;}
static unsigned int atomic_read(unsigned int *e){return *e;}
struct device {int unused;};struct power_supply {int unused;};
struct d1946_snapshot {u64 generation,sampled_ns,valid_until_ns;int online,budget_ua,present,voltage_uv,temp_decic;};
struct d1946_current_provider {void *context;int(*snapshot)(void *,struct power_supply *,struct power_supply *,struct d1946_snapshot *);bool(*valid)(void *,const struct d1946_snapshot *);};
struct d1946_current {
 struct delayed_work work;struct mutex io_lock;spinlock_t queue_lock;
 struct device *dev;const struct d1946_current_provider *provider;struct power_supply *input,*battery,*psy;
 struct d1946_io io;unsigned int epoch,requested_input,requested_charge,applied_input,applied_charge,applied_epoch,status_epoch;
 bool ready,events_enabled,stopping,fault_latched,applied_valid,status_valid,inhibit_attempted;
 u8 ceiling,status[6];u64 status_time_ns;
};
static unsigned int schedules,pending,snapshots,changes;static int provider_error;
static void *system_wq;
static void mod_delayed_work(void *wq,struct delayed_work *w,unsigned long delay){(void)wq;(void)delay;assert(w->work.marker==123);schedules++;pending=1;}
static bool queue_delayed_work(void *wq,struct delayed_work *w,unsigned long delay){if(pending)return false;mod_delayed_work(wq,w,delay);return true;}
static unsigned long msecs_to_jiffies(unsigned int t){return t;}
static u64 ktime_get_ns(void){return NSEC_PER_SEC;}
static int failed_snapshot(void *ctx,struct power_supply *i,struct power_supply *b,struct d1946_snapshot *s){(void)ctx;(void)i;(void)b;(void)s;snapshots++;return provider_error;}
static bool denied_valid(void *c,const struct d1946_snapshot *s){(void)c;(void)s;return false;}
static bool owned_usb2_source,allow_usb2_current;
static const struct d1946_current_provider denied_provider={NULL,failed_snapshot,denied_valid};
static int d_read(void *ctx,unsigned int reg,u8 *out,unsigned int n){struct d1946_current *d=ctx;return d->io.read(d->io.context,reg,out,n);}
static void power_supply_changed(struct power_supply *p){assert(p);changes++;}
'''
  for name in ['d_queue','d_invalidate','d_fault','d_snapshot_valid','d_profile_floor','d_decoded_input','d_charge_for_input','d_worker']:c+=function(src,name)+'\n'
  c+=r'''
static void init(struct d1946_current *d,struct fake *f){
 memset(d,0,sizeof(*d));setup(f,0xbf);d->io=(struct d1946_io){f,read_fake,write_fake};
 d->ready=d->events_enabled=d->applied_valid=true;d->provider=&denied_provider;d->work.work.marker=123;d->ceiling=63;
 schedules=pending=snapshots=changes=0;provider_error=-EAGAIN;
}
static void fault_test(void){
 struct d1946_current d;struct fake f;init(&d,&f);
 mutex_lock(&d.io_lock);d_fault(&d);d_fault(&d);d_fault(&d);mutex_unlock(&d.io_lock);
 assert(d.fault_latched&&!d.applied_valid&&d.epoch==3&&schedules==1&&pending==1&&f.calls==0);
 mutex_lock(&d.io_lock);d.stopping=true;d_fault(&d);mutex_unlock(&d.io_lock);assert(d.epoch==4&&schedules==1);
 puts("actual repeated fault/epoch/first-only queue fixtures PASS");
}
static void worker_test(void){
 const int errors[]={-EAGAIN,-EIO,-ENODEV};
 for(unsigned int i=0;i<ARRAY_SIZE(errors);i++){
  struct d1946_current d;struct fake f;init(&d,&f);provider_error=errors[i];
  d_worker(&d.work.work);
  assert(d.fault_latched&&d.inhibit_attempted&&!d.applied_valid&&d.epoch==1&&snapshots==1&&schedules==1&&pending==1&&f.calls>0);
  unsigned int calls=f.calls,writes=f.writes;pending=0;
  d_worker(&d.work.work);
  /* First supplier error latches fault; second run must bypass supplier. */
  #ifdef HISTORICAL_BUSY_WORKER
  assert(snapshots==2&&d.epoch==2&&d.fault_latched&&f.calls==calls&&f.writes==writes);
#else
  assert(snapshots==1&&d.epoch==1&&d.fault_latched&&f.calls==calls&&f.writes==writes);
#endif
  assert(schedules==1&&pending==0);
 }
 puts("actual worker two failure iterations/bounded inhibit/no busyloop fixtures PASS");
}

static void prelatched_test(void){
 struct d1946_current d;struct fake f;init(&d,&f);
 mutex_lock(&d.io_lock);d_fault(&d);mutex_unlock(&d.io_lock);
 pending=0;d_worker(&d.work.work);
 assert(snapshots==0&&d.epoch==1); /* Explicit old worker counterexample. */
 assert(d.inhibit_attempted&&!d.applied_valid&&d.fault_latched);
 assert(f.regs[0x3f]&0x40);assert(f.regs[0x1b]&4);assert(f.regs[0x1c]&0x80);assert(f.regs[0x3d]==0);
 assert(f.writes>0&&pending==0&&schedules==1&&!d.io_lock.locked);
 unsigned int calls=f.calls;d_worker(&d.work.work);
 assert(snapshots==0&&f.calls==calls&&d.epoch==1&&!pending);
 puts("prelatched skip/inhibit/zero-input fixtures PASS");
}
static void inactive_test(void){
 struct d1946_current d;struct fake f;
 for(unsigned int i=0;i<3;i++){
  init(&d,&f);d.fault_latched=true;
  if(i==0)d.ready=false;else if(i==1)d.events_enabled=false;else d.stopping=true;
  d_worker(&d.work.work);assert(!snapshots&&!f.calls&&!schedules&&!d.inhibit_attempted);
 }puts("inactive admission fixtures PASS");
}
int main(int argc,char **argv){assert(argc==2);if(!strcmp(argv[1],"fault"))fault_test();else if(!strcmp(argv[1],"prelatched"))prelatched_test();else if(!strcmp(argv[1],"inactive"))inactive_test();else worker_test();}

'''
  cls.tmp=tempfile.TemporaryDirectory();p=Path(cls.tmp.name);(p/'test.c').write_text(c);cls.bin=p/'test';cls.old=p/'historical-test'
  old=(ROOT/'historical-d-fault.c').read_text()
  worker=(ROOT/'historical-d-worker-prelatched.c').read_text()
  expected='1d8984f18bd60e886353a3daa78a5e9360cbb395d0ad40dd994011478ffb853a'
  assert hashlib.sha256(worker.encode()).hexdigest()==expected
  # Preserve the former self-requeue defect by combining BOTH actual old
  # functions; the new early skip must not hide its invocation in this control.
  busy=c.replace(function(src,'d_worker'),function(worker,'d_worker'))
  busy=busy.replace(function(src,'d_fault'),function(old,'d_fault'))
  (p/'historical.c').write_text('#define HISTORICAL_BUSY_WORKER 1\n'+busy)
  (p/'historical-prelatched.c').write_text(c.replace(function(src,'d_worker'),function(worker,'d_worker')))
  cls.oldskip=p/'historical-prelatched'
  for source,target in [('test.c',cls.bin),('historical.c',cls.old),('historical-prelatched.c',cls.oldskip)]:
   subprocess.run(['gcc','-DCASCADIA_OFFLINE','-std=gnu11','-Wall','-Wextra','-Werror','-Wno-unused-function','-fsanitize=address,undefined','-fno-sanitize-recover=all','-fno-pie','-no-pie','-I',str(ROOT),str(p/source),str(ROOT/'d1946-current-core.c'),'-o',str(target)],check=True)

 @classmethod
 def tearDownClass(cls):cls.tmp.cleanup()
 def test_actual_first_fault_queue_and_epoch(self):self.assertIn('fixtures PASS',subprocess.check_output([str(self.bin),'fault'],text=True))
 def test_actual_two_worker_failure_iterations(self):self.assertIn('fixtures PASS',subprocess.check_output([str(self.bin),'worker'],text=True))
 def test_actual_prelatched_inhibit_without_supplier(self):self.assertIn('fixtures PASS',subprocess.check_output([str(self.bin),'prelatched'],text=True))
 def test_actual_inactive_admission(self):self.assertIn('fixtures PASS',subprocess.check_output([str(self.bin),'inactive'],text=True))
 def test_historical_prelatched_capture_is_rejected(self):
  def no_core():resource.setrlimit(resource.RLIMIT_CORE,(0,0))
  result=subprocess.run([str(self.oldskip),'prelatched'],text=True,capture_output=True,preexec_fn=no_core)
  self.assertNotEqual(result.returncode,0)
  self.assertIn('snapshots==0&&d.epoch==1',result.stderr)
 def test_historical_worker_selfrequeue_is_rejected(self):
  def no_core():resource.setrlimit(resource.RLIMIT_CORE,(0,0))
  result=subprocess.run([str(self.old),'worker'],text=True,capture_output=True,preexec_fn=no_core)
  self.assertNotEqual(result.returncode,0)
  self.assertIn('schedules==1&&pending==0',result.stderr)
if __name__=='__main__':unittest.main()
