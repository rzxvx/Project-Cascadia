#!/usr/bin/env python3
"""Exercise actual event worker and IRQ code with deterministic kernel mocks."""
from pathlib import Path
import subprocess,tempfile
s=(Path(__file__).resolve().parents[1]/'extras/apple_z2_j171.c').read_text()
a=s.index('static int apple_z2_j171_service(');b=s.index('static void apple_z2_stop(struct apple_z2 *z2);',a)
worker=s[a:b]
a=s.index('static irqreturn_t apple_z2_irq(');b=s.index('static void apple_z2_set_gpio(',a)
irq=s[a:b]
header=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#define APPLE_Z2_J172_MAX_CONTACTS 32
#define APPLE_Z2_STATE_FIRMWARE_READY 6
#define MT_TOOL_FINGER 0
#define READ_ONCE(x) (x)
#define IRQ_HANDLED 1
#define unlikely(x) (x)
#define dev_info(d,fmt,...) test_log(fmt, ##__VA_ARGS__)
#define dev_err(d,fmt,...) test_log(fmt, ##__VA_ARGS__)
static void test_log(const char *fmt,...){(void)fmt;}
#define ktime_to_us(x) (x)
#define ktime_get_boottime() 0
#define cond_resched() ((void)0)
#define input_mt_slot(...) ((void)0)
#define input_mt_report_slot_state(...) ((void)0)
#define input_mt_sync_frame(...) ((void)0)
#define input_sync(...) ((void)0)
typedef int irqreturn_t;
typedef struct {int n;} atomic_t;
static int atomic_read(atomic_t*a){return a->n;}
static void atomic_set(atomic_t*a,int n){a->n=n;}
static void atomic_inc(atomic_t*a){a->n++;}
static int atomic_inc_return(atomic_t*a){return ++a->n;}
static int atomic_dec_if_positive(atomic_t*a){if(a->n>0)return --a->n;return -1;}
struct mutex{int depth;};
static void mutex_lock(struct mutex*m){assert(!m->depth);m->depth++;}
static void mutex_unlock(struct mutex*m){assert(m->depth==1);m->depth--;}
struct apple_z2{bool j171,booted,runtime_quiesced;atomic_t runtime_pending,runtime_irq_count,boot_irq_count;
 int runtime_wait,boot_irq,protocol_state;unsigned recovery_count;struct mutex runtime_lock,io_lock;void*input_dev;};
static bool stopped,fail,fail_boot,inject_irq;static unsigned waits,reads,boots,stops,captures,wakes;
static bool kthread_should_stop(void){return stopped;}
#define msecs_to_jiffies(x) (x)
#define wait_event_interruptible_timeout(q,cond,t) ({(void)(q);(void)(t);assert(++waits<20);bool ready=(cond);if(!ready)stopped=true;ready?1L:0L;})
static void apple_z2_quiet(struct apple_z2*z){(void)z;}
static void wake_up_interruptible(int*q){(void)q;wakes++;}
static void complete(int*c){(*c)++;}
static int apple_z2_read_packet(struct apple_z2*z){assert(z->io_lock.depth==1);reads++;if(inject_irq){inject_irq=false;atomic_inc(&z->runtime_pending);}return fail?-EIO:0;}
static void apple_z2_capture_failure(struct apple_z2*z,int e){assert(z->io_lock.depth==1&&e);captures++;}
static void __apple_z2_stop(struct apple_z2*z){assert(z->runtime_lock.depth==1&&!z->io_lock.depth);assert(captures>0);stops++;z->booted=false;atomic_set(&z->runtime_pending,0);}
static int __apple_z2_boot(struct apple_z2*z){assert(z->runtime_lock.depth==1&&!z->io_lock.depth);boots++;if(fail_boot)return -EIO;z->booted=true;atomic_set(&z->runtime_pending,1);return 0;}
'''
main=r'''
static struct apple_z2 z;
static void reset(void){memset(&z,0,sizeof(z));z.j171=z.booted=true;stopped=fail=fail_boot=inject_irq=false;waits=reads=boots=stops=captures=wakes=0;}
int main(void){
 reset();apple_z2_j171_service(&z);assert(!reads&&!boots); /* No periodic SPI. */
 reset();z.runtime_pending.n=3;apple_z2_j171_service(&z);assert(reads==3&&!boots);
 reset();z.runtime_pending.n=40;apple_z2_j171_service(&z);assert(reads==40&&waits==3); /* budget yields, retains work */
 reset();z.runtime_pending.n=1;inject_irq=true;apple_z2_j171_service(&z);assert(reads==2);
 reset();z.runtime_quiesced=true;z.runtime_pending.n=1;apple_z2_j171_service(&z);assert(!reads);
 reset();z.booted=false;z.runtime_pending.n=1;apple_z2_j171_service(&z);assert(!reads);
 reset();z.runtime_pending.n=1;fail=true;apple_z2_j171_service(&z);assert(reads==2&&boots==1&&stops==2&&captures==2&&!z.booted);
 reset();z.runtime_pending.n=1;fail=fail_boot=true;apple_z2_j171_service(&z);assert(reads==1&&boots==1&&stops==1&&captures==1);
 reset();z.booted=false;z.protocol_state=7;apple_z2_irq(0,&z);assert(z.boot_irq==1&&z.runtime_pending.n==1&&!reads);
 z.booted=true;apple_z2_j171_service(&z);assert(reads==1); /* deferred init IRQ replay */
 reset();apple_z2_irq(0,&z);apple_z2_irq(0,&z);assert(z.runtime_pending.n==2&&z.runtime_irq_count.n==2&&wakes==2&&!reads);
 apple_z2_j171_service(&z);assert(reads==2);
 reset();z.j171=false;apple_z2_irq(0,&z);assert(reads==1&&!z.runtime_pending.n); /* legacy unchanged */
 reset();z.booted=false;z.protocol_state=2;apple_z2_irq(0,&z);assert(z.boot_irq==1&&!z.runtime_pending.n);
 puts("PASS: event-only service, deferred/queued/racing IRQs, budget, quiesce, recovery limit/order, boot failure, legacy IRQ");
}
'''
with tempfile.TemporaryDirectory(prefix='j171-service-') as d:
 p=Path(d)/'test.c';p.write_text(header+irq+worker+main)
 subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror','-Wno-unused-parameter','-fsanitize=undefined,bounds',str(p),'-o',str(p.with_suffix(''))],check=True)
 subprocess.run([str(p.with_suffix(''))],check=True)
