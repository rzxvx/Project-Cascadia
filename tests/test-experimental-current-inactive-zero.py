#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Actual setter/queue/worker bodies; default-inactive zero must not claim application."""
import argparse,hashlib,json,re,resource,subprocess,tempfile,shutil
from pathlib import Path
def function(s,n):
 p=re.search(r'\b'+re.escape(n)+r'\(',s).start();a=s.rfind('\nstatic ',0,p)+1;b=s.index('{',p);depth=1;e=b+1
 while depth:depth+=(s[e]=='{')-(s[e]=='}');e+=1
 return s[a:e]
PREFIX=r'''
#define main core_original_main
#include "core-fixture.c"
#undef main
#include <stdint.h>
#include <stddef.h>
typedef uint64_t u64;
#define READ_ONCE(v) (v)
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#define min(a,b) ((a)<(b)?(a):(b))
#define min_t(t,a,b) min((t)(a),(t)(b))
#define NSEC_PER_SEC 1000000000ULL
#define D_EXPERIMENT_MAX_MA 720
#define dev_err(...) ((void)0)
#define container_of(p,t,m) ((t *)((char *)(p)-offsetof(t,m)))
struct work_struct{int marker;};struct delayed_work{struct work_struct work;};
static struct delayed_work*to_delayed_work(struct work_struct*w){return container_of(w,struct delayed_work,work);}
struct mutex{int held;};typedef struct{int held;}spinlock_t;
static void mutex_lock(struct mutex*m){assert(!m->held);m->held=1;}
static void mutex_unlock(struct mutex*m){assert(m->held);m->held=0;}
#define spin_lock_irqsave(l,f) ((f)=0,assert(!(l)->held),(l)->held=1)
#define spin_unlock_irqrestore(l,f) ((void)(f),assert((l)->held),(l)->held=0)
static void atomic_inc(unsigned int*v){++*v;}static unsigned int atomic_read(unsigned int*v){return *v;}
struct device{int unused;};struct power_supply{void*data;};static void*power_supply_get_drvdata(struct power_supply*p){return p->data;}
enum power_supply_property{POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT,POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT,OTHER};
union power_supply_propval{int intval;};
struct d1946_snapshot{u64 generation,sampled_ns,valid_until_ns;int online,budget_ua,present,voltage_uv,temp_decic;};
struct d1946_current_provider{void*context;int(*snapshot)(void*,struct power_supply*,struct power_supply*,struct d1946_snapshot*);bool(*valid)(void*,const struct d1946_snapshot*);};
struct d1946_current{
 struct device*dev;struct mutex lifecycle_lock,io_lock,bus_lock;spinlock_t queue_lock;
 bool ready,events_enabled,stopping,fault_latched,applied_valid,status_valid,inhibit_attempted,parent_enabled;
 unsigned int epoch,requested_charge,requested_input,applied_input,applied_charge,applied_epoch,status_epoch;
 struct delayed_work work;int parent_irq,virq[3];u8 ceiling,status[6];u64 status_time_ns;
 struct d1946_io io;struct power_supply*input,*battery,*psy;const struct d1946_current_provider*provider;
};
static unsigned int queued,enabled,snapshots;static bool pending;static void*system_wq;
static void mod_delayed_work(void*wq,struct delayed_work*w,unsigned long delay){(void)wq;(void)w;(void)delay;++queued;pending=true;}
static bool queue_delayed_work(void*wq,struct delayed_work*w,unsigned long delay){if(pending)return false;mod_delayed_work(wq,w,delay);return true;}
static void enable_irq(int irq){(void)irq;++enabled;}
static const unsigned int source_events[]={3,4,52};
static u64 ktime_get_ns(void){return NSEC_PER_SEC;}static unsigned long msecs_to_jiffies(unsigned int t){return t;}
static void power_supply_changed(struct power_supply*p){(void)p;}
static int denied_snapshot(void*c,struct power_supply*i,struct power_supply*b,struct d1946_snapshot*s){(void)c;(void)i;(void)b;(void)s;++snapshots;return-EAGAIN;}
static bool denied_valid(void*c,const struct d1946_snapshot*s){(void)c;(void)s;return false;}
static const struct d1946_current_provider provider={NULL,denied_snapshot,denied_valid};
static bool owned_usb2_source,allow_usb2_current;
static int d_read(void*ctx,unsigned int reg,u8*out,unsigned int n){struct d1946_current*d=ctx;return d->io.read(d->io.context,reg,out,n);}
'''
SUFFIX=r'''
static void init(struct d1946_current*d,struct fake*f){memset(d,0,sizeof(*d));setup(f,0xbf);d->ready=true;d->ceiling=63;d->provider=&provider;d->io=(struct d1946_io){f,read_fake,write_fake};queued=enabled=snapshots=0;pending=false;}
int main(int argc,char**argv){
 assert(argc==2);struct d1946_current d;struct fake f;struct power_supply p={&d};union power_supply_propval v={0};
 if(!strcmp(argv[1],"inactive")){
  for(unsigned int prop=0;prop<2;prop++){
   init(&d,&f);d.requested_charge=480;d.requested_input=500;int ret=d_set(&p,prop,&v);
   assert(ret==-EAGAIN&&f.calls==0&&queued==0&&enabled==0&&!d.events_enabled&&d.requested_charge==480&&d.requested_input==500);
   d_worker(&d.work.work);assert(!snapshots&&f.calls==0&&queued==0);
  }
 }else if(!strcmp(argv[1],"active")){
  for(unsigned int prop=0;prop<2;prop++){
   init(&d,&f);d.events_enabled=true;d.requested_charge=480;d.requested_input=500;
   assert(!d_set(&p,prop,&v)&&queued==1&&enabled==0&&f.calls==0);
   d_worker(&d.work.work);
   assert(snapshots==1&&d.fault_latched&&d.inhibit_attempted&&(f.regs[0x3f]&0x40)&&f.regs[0x3d]==0);
   unsigned int calls=f.calls,q=queued;d_worker(&d.work.work);assert(f.calls==calls&&queued==q);
  }
 }else if(!strcmp(argv[1],"nonzero")){
  init(&d,&f);v.intval=500000;
  assert(!d_set(&p,POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT,&v)&&d.events_enabled&&queued==1&&enabled==4&&f.calls==0);
  assert(d.requested_input==500&&snapshots==0);
 }else{
  init(&d,&f);d.ready=false;assert(d_set(&p,0,&v)==-EAGAIN&&f.calls==0&&queued==0&&enabled==0);
  init(&d,&f);d.stopping=true;assert(d_set(&p,0,&v)==-EIO&&f.calls==0&&queued==0&&enabled==0);
  init(&d,&f);v.intval=39000;assert(d_set(&p,0,&v)==-EINVAL&&f.calls==0);
  init(&d,&f);v.intval=0;assert(d_set(&p,OTHER,&v)==-EINVAL&&f.calls==0); /* invalid property errno is preserved */
  init(&d,&f);d.events_enabled=true;assert(d_set(&p,OTHER,&v)==-EINVAL&&f.calls==0);
 }
 puts("actual inactive-zero setter/worker fixtures PASS");
}
'''
NAMES=['d_queue','d_invalidate','d_fault','d_set','d_snapshot_valid','d_profile_floor','d_decoded_input','d_charge_for_input','d_worker']
def build(src,root,dest):
 text=PREFIX+'\n'.join(function(src,n)for n in NAMES)+SUFFIX;dest.with_suffix('.c').write_text(text)
 subprocess.run(['gcc','-DCASCADIA_OFFLINE','-std=gnu11','-Wall','-Wextra','-Werror','-Wno-unused-function','-fno-pie','-no-pie','-fsanitize=address,undefined','-fno-sanitize-recover=all','-I',str(root),str(dest.with_suffix('.c')),str(root/'d1946-current-core.c'),'-o',str(dest)],check=True,timeout=60)
def main():
 default=Path(__file__).resolve().parents[1]/'experiments/p105-current'
 p=argparse.ArgumentParser();p.add_argument('--current-dir',type=Path,default=default);p.add_argument('--historical-setter',type=Path);a=p.parse_args();root=a.current_dir
 if not shutil.which('gcc'):
  print('SKIP: GCC required for actual-C source fixture');return
 historical=a.historical_setter or root/'historical-d-set-inactive-zero.c'
 fixed=(root/'apple-d1946-current-only.c').read_text();raw=historical.read_bytes()
 if hashlib.sha256(raw).hexdigest()!='ba7ee519a754c1e8774ed15fa011ce4aec0c37a63d617aa4ebbdc46eb2938ca7':raise ValueError('historical fixture SHA mismatch')
 old=fixed.replace(function(fixed,'d_set'),function(raw.decode(),'d_set'))
 resource.setrlimit(resource.RLIMIT_CORE,(0,0));results={}
 with tempfile.TemporaryDirectory()as d:
  d=Path(d);build(fixed,root,d/'fixed');build(old,root,d/'historical')
  for case in ['inactive','active','nonzero','bounds']:
   r=subprocess.run([str(d/'fixed'),case],text=True,capture_output=True,timeout=20);assert r.returncode==0,(case,r.stderr);results[case]={'fixed_return':r.returncode,'output':r.stdout.strip()}
  r=subprocess.run([str(d/'historical'),'inactive'],text=True,capture_output=True,timeout=20);assert r.returncode!=0 and 'ret==-EAGAIN' in r.stderr
  results['historical_inactive']={'return':r.returncode,'assertion_failure_expected':True,'stderr':r.stderr.replace(str(d),'<temporary>')}
 print(json.dumps({'hardware_access':False,'fixed_sha256':hashlib.sha256(fixed.encode()).hexdigest(),'historical_sha256':hashlib.sha256(old.encode()).hexdigest(),'groups':results,'mocks':['all locks/IRQ/queue/corePMU I/O synthetic','active worker receives real mocked provider error -EAGAIN, no positive permission','zero-before-activation performs no transport']},indent=2))
if __name__=='__main__':main()
