from pathlib import Path
import shutil,subprocess,tempfile,unittest
ROOT=Path(__file__).resolve().parents[1]/'experiments/p105-current'

def function(source,name):
 a=source.rfind('\nstatic ',0,source.index(name+'('))+1
 b=source.index('{',source.index(name+'('));depth=1;e=b+1
 while depth:
  depth+=(source[e]=='{')-(source[e]=='}');e+=1
 return source[a:e]

class PM(unittest.TestCase):
 def test_actual_stop_suspend_resume(self):
  if not shutil.which('gcc'):self.skipTest('gcc required')
  source=(ROOT/'apple-d1946-current-only.c').read_text()
  text=r'''
#define main core_original_main
#include "core-fixture.c"
#undef main
#define IS_REACHABLE(x) 0
#define READ_ONCE(x) (x)
#define WRITE_ONCE(x,v) ((x)=(v))
#define dev_err(d,...) ((void)(d))
#define dev_warn(d,...) ((void)(d))
struct mutex { int locked; };
static void mutex_lock(struct mutex *m) { assert(!m->locked); m->locked=1; }
static void mutex_unlock(struct mutex *m) { assert(m->locked); m->locked=0; }
static void atomic_inc(unsigned int *v) { (*v)++; }
typedef struct {int held;} spinlock_t;
static void spin_lock(spinlock_t *l){assert(!l->held);l->held=1;}
static void spin_unlock(spinlock_t *l){assert(l->held);l->held=0;}
#define spin_lock_irqsave(l,f) ((f)=0,spin_lock(l))
#define spin_unlock_irqrestore(l,f) ((void)(f),spin_unlock(l))
static unsigned int cancels,disables,synchronizes;
static void synchronize_irq(int irq){assert(irq==17);synchronizes++;}
static void disable_irq(int irq) { assert(irq==17); disables++; }
static void cancel_delayed_work_sync(int *w) { assert(*w==123); cancels++; }
struct device { void *data; };
static void *dev_get_drvdata(struct device *d) { return d->data; }
struct d1946_current {
 struct mutex lifecycle_lock,io_lock;spinlock_t queue_lock;
 struct device *dev;
 int stop_error,parent_irq,work;
 bool stopping,ready,parent_enabled,parent_failed,applied_valid,status_valid,
  events_enabled,fault_latched;
 unsigned int epoch;
 u8 ceiling;
 struct d1946_io io;
};
'''
  text+='\n'.join(function(source,n) for n in ['d_stop','d_suspend','d_resume'])
  text+=r'''
static void make(struct d1946_current *d,struct fake *f,struct device *dev) {
 memset(d,0,sizeof(*d)); setup(f,0xbf); d->io=(struct d1946_io){f,read_fake,write_fake};
 d->dev=dev;dev->data=d;d->ready=d->events_enabled=d->parent_enabled=true;
 d->applied_valid=d->status_valid=true;d->parent_irq=17;d->work=123;d->ceiling=63;
 d->epoch=10;cancels=disables=synchronizes=0;
}
int main(void) {
 struct fake f;struct device dev;struct d1946_current d;unsigned int calls;
 make(&d,&f,&dev);
 assert(!d_suspend(&dev));
 assert(d.stopping&&!d.ready&&!d.applied_valid&&!d.status_valid);
 assert(d.epoch==11&&cancels==1&&disables==1&&synchronizes==1&&!d.fault_latched);
 assert(f.regs[0x3f]==0xff&&f.regs[0x1b]==0xa5&&f.regs[0x1c]==0x93&&f.regs[0x3d]==0);
 calls=f.calls;
 assert(!d_resume(&dev)&&d.stopping&&!d.ready&&f.calls==calls&&d.epoch==11);
 assert(!d_stop(&d)&&f.calls==calls&&cancels==1&&disables==1);
 make(&d,&f,&dev);f.fail_at=1;
 assert(d_suspend(&dev)==-EREMOTEIO&&d.fault_latched&&d.stopping);
 calls=f.calls;assert(d_stop(&d)==-EREMOTEIO&&f.calls==calls);
 assert(!d_resume(&dev)&&d.fault_latched&&d.stopping);
 make(&d,&f,&dev);f.fail_at=4;
 assert(d_suspend(&dev)==-EREMOTEIO&&d.fault_latched);
 make(&d,&f,&dev);d.events_enabled=d.parent_enabled=false;
 assert(!d_suspend(&dev)&&f.calls==0&&disables==0&&cancels==1&&d.stopping);
 puts("actual PM stop/suspend/resume/error/no-activation fixtures PASS");
}
'''
  with tempfile.TemporaryDirectory() as tmp:
   p=Path(tmp);(p/'fixture.c').write_text(text)
   subprocess.run(['gcc','-DCASCADIA_OFFLINE','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=undefined','-fno-sanitize-recover=all','-I',str(ROOT),str(p/'fixture.c'),str(ROOT/'d1946-current-core.c'),'-o',str(p/'test')],check=True)
   result=subprocess.check_output([str(p/'test')],text=True)
   self.assertIn('fixtures PASS',result)
if __name__=='__main__':unittest.main()
