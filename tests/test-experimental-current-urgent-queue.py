#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Actual PMU dispatch/worker and pinned Linux queue decisions; fake time/I/O."""
from pathlib import Path
import ast
import hashlib
import os
import re
import resource
import shutil
import subprocess
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[1]
ROOT = Path(os.environ.get('CASCADIA_CURRENT_ROOT', REPO / 'experiments/p105-current'))
LINUX = Path(os.environ.get('CASCADIA_TEST_LINUX', REPO / 'build/linux'))
WORKQUEUE_SHA = '0596a9529ba2e35fe25df06e78640887fe40b9f0c4bfd93ca2f5ce4e1ab41e77'
HISTORICAL_SHA = '5aabb130f1efc79126ae3c344f0762cbd5278683597efe36e2a6522995b148a5'


def function(s, n):
    pos = re.search(r'\b' + re.escape(n) + r'\(', s).start()
    a = s.rfind('\n', 0, pos) + 1
    b = s.index('{', pos)
    depth, e = 1, b + 1
    while depth:
        depth += (s[e] == '{') - (s[e] == '}')
        e += 1
    return s[a:e]


class UrgentQueue(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not shutil.which('gcc') or not (LINUX / 'kernel/workqueue.c').is_file():
            raise unittest.SkipTest('GCC and pinned Linux 6.12 workqueue.c required')
        kernel = (LINUX / 'kernel/workqueue.c').read_text()
        assert hashlib.sha256(kernel.encode()).hexdigest() == WORKQUEUE_SHA
        src = (ROOT / 'apple-d1946-current-only.c').read_text()
        old = (ROOT / 'historical-d-queue-postponed-urgent.c').read_text()
        assert hashlib.sha256(old.encode()).hexdigest() == HISTORICAL_SHA
        # Reuse declarations/kernel mocks from the retained real-worker fixture,
        # without running its compiler or replacing the production worker.
        tree = ast.parse((REPO / 'tests/test-experimental-current-small.py').read_text())
        prefix = next(node.value.value for node in ast.walk(tree)
                      if isinstance(node, ast.Assign)
                      and any(isinstance(t, ast.Name) and t.id == 'c' for t in node.targets)
                      and isinstance(node.value, ast.Constant)
                      and isinstance(node.value.value, str))
        prefix = prefix.replace('static const struct d1946_current_provider denied_provider={NULL,failed_snapshot,denied_valid};', '')
        prefix = prefix.replace('struct work_struct {int marker;};',
                                'struct work_struct {int marker;unsigned long flags;};')
        prefix = prefix.replace('u8 ceiling,status[6];u64 status_time_ns;',
                                'u8 ceiling,status[6],masks[9];u64 status_time_ns;void *domain;bool parent_failed;')
        start = prefix.index('static void mod_delayed_work(')
        end = prefix.index('static unsigned long msecs_to_jiffies', start)
        prefix = prefix[:start] + r'''
/* Only Linux's PENDING/disabled/IRQ/timer engine is mocked. The two actual
 * Linux queue entry-point bodies below decide whether to replace a deadline.
 * This is a deterministic state model, not execution of a real workqueue. */
struct workqueue_struct {int unused;};
#define WORK_STRUCT_PENDING_BIT 0
#define WORK_CANCEL_DELAYED 1
#define WORK_CPU_UNBOUND -1
#define EXPORT_SYMBOL(x)
#define EXPORT_SYMBOL_GPL(x)
#define local_irq_save(f) ((f)=0)
#define local_irq_restore(f) ((void)(f))
static unsigned long fake_now=1000,deadline;
static bool running;
static unsigned int queue_calls,mod_calls;
static unsigned long *work_data_bits(struct work_struct *w){return &w->flags;}
static bool test_and_set_bit(unsigned int bit,unsigned long *flags){
 unsigned long mask=1UL<<bit;bool prior=!!(*flags&mask);*flags|=mask;return prior;
}
static bool clear_pending_if_disabled(struct work_struct *w){(void)w;return false;}
static bool work_grab_pending(struct work_struct *w,int flags,unsigned long *irq){
 assert(flags==WORK_CANCEL_DELAYED);*irq=0;return test_and_set_bit(0,&w->flags);
}
static void __queue_delayed_work(int cpu,struct workqueue_struct *q,struct delayed_work *w,unsigned long delay){
 (void)q;assert(cpu==WORK_CPU_UNBOUND&&w->work.marker==123&&(w->work.flags&1));
 schedules++;pending=1;deadline=fake_now+delay;
}
''' + '\n'.join(function(kernel, n) for n in ['queue_delayed_work_on', 'mod_delayed_work_on']) + r'''
static bool queue_delayed_work(void *q,struct delayed_work *w,unsigned long delay){
 queue_calls++;return queue_delayed_work_on(WORK_CPU_UNBOUND,q,w,delay);
}
static bool mod_delayed_work(void *q,struct delayed_work *w,unsigned long delay){
 mod_calls++;return mod_delayed_work_on(WORK_CPU_UNBOUND,q,w,delay);
}
''' + prefix[end:]
        # Inject the real PMU parent dispatch only after the worker releases
        # io_lock, immediately before its normal periodic rearm.
        prefix = prefix.replace('static void power_supply_changed(struct power_supply *p){assert(p);changes++;}', r'''
typedef int irqreturn_t;
#define IRQ_HANDLED 1
#define D_NBANKS 9
static bool inject_parent;
static unsigned int parents,children,disables;
static irqreturn_t d_event(int irq,void *context);
static irqreturn_t d_parent(int irq,void *context);
static unsigned int irq_find_mapping(void *domain,unsigned int raw){assert(domain==active);return raw==3?303:0;}
static void handle_nested_irq(unsigned int irq){assert(irq==303);children++;assert(d_event(irq,active)==IRQ_HANDLED);}
static void disable_irq_nosync(int irq){assert(irq==19);disables++;}
static void power_supply_changed(struct power_supply *p){
 assert(p);changes++;
 if(inject_parent){
  inject_parent=false;assert(running&&!active->io_lock.locked);
  ((struct fake *)active->io.context)->regs[1]=8;parents++;
  assert(d_parent(19,active)==IRQ_HANDLED);
 }
}
''')
        c = prefix + '\n'.join(function(src, n) for n in [
            'd_queue', 'd_invalidate', 'd_fault', 'd_event', 'd_parent',
            'd_snapshot_valid', 'd_profile_floor', 'd_decoded_input',
            'd_charge_for_input', 'd_worker']) + r'''
static void init(struct d1946_current *d,struct fake *f){
 memset(d,0,sizeof(*d));setup(f,0xbf);active=d;d->domain=d;
 d->io=(struct d1946_io){f,read_fake,write_fake};
 d->provider=&explicit_provider;d->ready=d->events_enabled=true;d->ceiling=63;
 d->requested_input=500;d->requested_charge=480;d->work.work.marker=123;
 static struct power_supply psy;d->psy=&psy;
 positive_sample=(struct d1946_snapshot){.generation=1,.sampled_ns=NSEC_PER_SEC-1,.valid_until_ns=NSEC_PER_SEC+1000,.online=1,.budget_ua=500000,.present=1,.voltage_uv=3900000,.temp_decic=250};
 f->regs[0x40]=0x40;f->regs[0x0a]=8;memset(f->regs+0x0b,0,5);memset(f->regs+1,0,9);
 owned_usb2_source=allow_usb2_current=true;provider_error=0;
 validity_calls=deny_at=invalidate_at=0;inject_epoch=validator_revoked=inject_parent=false;
 schedules=pending=snapshots=changes=parents=children=disables=queue_calls=mod_calls=0;
 fake_now=1000;deadline=0;running=false;
}
static void worker_begin(struct d1946_current *d){
 /* process_one_work clears PENDING before current_func. No real CPU timing. */
 d->work.work.flags&=~1UL;pending=0;running=true;
}
static void worker_end(void){running=false;}
int main(int argc,char **argv){
 assert(argc==2);struct d1946_current d;struct fake f;init(&d,&f);
 if(!strcmp(argv[1],"event-before-poll")){
  worker_begin(&d);inject_parent=true;d_worker(&d.work.work);worker_end();
  assert(parents==1&&children==1&&d.epoch==1&&!d.fault_latched&&pending);
  /* Publishing occurred before the event; epoch-based readout rejects it. */
  assert(d.applied_valid&&d.applied_epoch==0&&d.applied_epoch!=d.epoch);
  fprintf(stderr,"actual parent->event->invalidate then worker rearm: next_delay=%lu; physical cutoff unmeasured\n",deadline-fake_now);
  assert(deadline==fake_now&&"urgent_deadline_postponed");
 }else if(!strcmp(argv[1],"poll-before-event")){
  worker_begin(&d);d_worker(&d.work.work);worker_end();assert(pending&&deadline==1100);
  f.regs[1]=8;assert(d_parent(19,&d)==IRQ_HANDLED);
  assert(children==1&&d.epoch==1&&deadline==1000&&pending);
 }else if(!strcmp(argv[1],"regular-poll")){
  worker_begin(&d);d_worker(&d.work.work);worker_end();
  assert(pending&&deadline==1100&&d.applied_valid&&d.applied_charge==480);
  fake_now=1100;worker_begin(&d);unsigned int writes=f.writes;
  d_worker(&d.work.work);worker_end();assert(pending&&deadline==1200&&f.writes==writes);
 }else if(!strcmp(argv[1],"preserve-earlier-poll")){
  d_queue(&d,30,false);assert(pending&&deadline==1030);
  d_queue(&d,100,false);assert(pending&&deadline==1030);
 }else if(!strcmp(argv[1],"gates")){
  d.stopping=true;d_queue(&d,100,false);d_invalidate(&d);assert(!pending&&!schedules);
  d.stopping=false;d.ready=false;d_queue(&d,100,false);d_invalidate(&d);assert(!pending&&!schedules);
  d.ready=true;d.events_enabled=false;d_queue(&d,100,false);d_invalidate(&d);assert(!pending&&!schedules);
  d.events_enabled=true;d.fault_latched=true;d_queue(&d,100,false);assert(!pending&&!schedules);
  d_queue(&d,0,true);assert(pending&&deadline==1000&&schedules==1);
  d_queue(&d,100,false);assert(pending&&deadline==1000&&schedules==1);
 }else{
  assert(!strcmp(argv[1],"parent-error"));f.fail_at=1;
  assert(d_parent(19,&d)==IRQ_HANDLED);
  assert(d.parent_failed&&d.fault_latched&&disables==1&&pending&&deadline==1000);
  worker_begin(&d);d_worker(&d.work.work);worker_end();
  assert(d.inhibit_attempted&&!pending&&(f.regs[0x3f]&0x40)&&f.regs[0x3d]==0);
 }
 puts("actual queue/worker/PMU source deadline fixtures PASS");
}
'''
        cls.tmp = tempfile.TemporaryDirectory()
        p = Path(cls.tmp.name)
        cls.fixed, cls.old = p / 'fixed', p / 'historical'
        for text, target in [(c, cls.fixed), (c.replace(function(src, 'd_queue'), function(old, 'd_queue')), cls.old)]:
            source = target.with_suffix('.c')
            source.write_text(text)
            subprocess.run(['gcc', '-DCASCADIA_OFFLINE', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                            '-Wno-unused-function', '-Wno-unused-parameter', '-fno-pie', '-no-pie', '-fsanitize=address,undefined',
                            '-fno-sanitize-recover=all', '-I', str(ROOT), str(source),
                            str(ROOT / 'd1946-current-core.c'), '-o', str(target)], check=True, timeout=60)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def case(self, name):
        result = subprocess.run([str(self.fixed), name], text=True, capture_output=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('fixtures PASS', result.stdout)

    def test_actual_pmu_event_urgent_before_worker_poll(self): self.case('event-before-poll')
    def test_actual_pmu_event_advances_existing_poll(self): self.case('poll-before-event')
    def test_executing_worker_requeues_regular_poll(self): self.case('regular-poll')
    def test_periodic_rearm_preserves_earlier_pending_poll(self): self.case('preserve-earlier-poll')
    def test_stop_ready_events_fault_queue_gates(self): self.case('gates')
    def test_actual_parent_read_error_queues_bounded_inhibit(self): self.case('parent-error')

    def test_historical_worker_postpones_urgent_event(self):
        def no_core(): resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
        result = subprocess.run([str(self.old), 'event-before-poll'], text=True, capture_output=True,
                                preexec_fn=no_core, timeout=10)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('urgent_deadline_postponed', result.stderr)
        self.assertIn('next_delay=100;', result.stderr)
        print('historical controlled rejection: ' + result.stderr.splitlines()[0])


if __name__ == '__main__':
    unittest.main()
