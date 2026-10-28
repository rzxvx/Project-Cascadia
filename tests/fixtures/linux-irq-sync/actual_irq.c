// SPDX-License-Identifier: GPL-2.0-only
/* Linux 6.12 GPL IRQ bodies inserted verbatim by test script; all APIs are Fakes.
 * Original kernel/irq/manage.c:
 * Copyright (C) 1992, 1998-2006 Linus Torvalds, Ingo Molnar
 * Copyright (C) 2005-2006 Thomas Gleixner
 */
#include <assert.h>
#include <stdbool.h>
#include <errno.h>
#include <stdio.h>
#include <setjmp.h>
#include <string.h>
struct irq_data;
enum irqchip_irq_state { IRQCHIP_STATE_ACTIVE };
struct irq_chip { int (*irq_get_irqchip_state)(struct irq_data *, enum irqchip_irq_state, bool *); };
struct irq_data { struct irq_chip *chip; struct irq_data *parent_data; };
struct irq_desc { struct irq_data irq_data; int lock, threads_active, wait_for_threads; };
static unsigned int checks, locks, unlocks, relaxes, hardware_queries, thread_waits, warnings;
static unsigned int busy_reads, hardware_active_reads;
static bool race_on_lock, physical_pending, busy_forever;
static int query_error;
static jmp_buf bounded_observer;
#define CHECK(x) do { assert(x); checks++; } while (0)
#define WARN_ON_ONCE(x) ((x) ? (warnings++, 1) : 0)
static struct irq_data *irq_desc_get_irq_data(struct irq_desc *d) { return &d->irq_data; }
static struct irq_chip *irq_data_get_irq_chip(struct irq_data *d) { return d->chip; }
static bool irqd_irq_inprogress(struct irq_data *d) {
 (void)d; if (busy_forever) return true;
 if (busy_reads) { busy_reads--; return true; } return false;
}
static void cpu_relax(void) {
 relaxes++; if (busy_forever && relaxes > 100) longjmp(bounded_observer, 1);
 CHECK(relaxes < 101);
}
#define raw_spin_lock_irqsave(lock,flags) do { (void)(lock); (flags)=0; locks++; if (race_on_lock) { race_on_lock=false; busy_reads=2; } } while (0)
#define raw_spin_unlock_irqrestore(lock,flags) do { (void)(lock); (void)(flags); unlocks++; } while (0)
#define atomic_read(x) (*(x))
static void fake_thread_progress(int *threads) { CHECK(*threads > 0); (*threads)--; thread_waits++; CHECK(thread_waits <= 100); }
#define wait_event(queue, condition) do { (void)(queue); while (!(condition)) fake_thread_progress(&desc->threads_active); } while (0)
static int state_callback(struct irq_data *d, enum irqchip_irq_state which, bool *state) {
 CHECK(d && which == IRQCHIP_STATE_ACTIVE); hardware_queries++;
 if (query_error) return query_error;
 *state = hardware_active_reads > 0;
 if (hardware_active_reads) hardware_active_reads--;
 return 0;
}
int __irq_get_irqchip_state(struct irq_data *data, enum irqchip_irq_state which,
			    bool *state)
{
	struct irq_chip *chip;
	int err = -EINVAL;

	do {
		chip = irq_data_get_irq_chip(data);
		if (WARN_ON_ONCE(!chip))
			return -ENODEV;
		if (chip->irq_get_irqchip_state)
			break;
#ifdef CONFIG_IRQ_DOMAIN_HIERARCHY
		data = data->parent_data;
#else
		data = NULL;
#endif
	} while (data);

	if (data)
		err = chip->irq_get_irqchip_state(data, which, state);
	return err;
}
static void __synchronize_hardirq(struct irq_desc *desc, bool sync_chip)
{
	struct irq_data *irqd = irq_desc_get_irq_data(desc);
	bool inprogress;

	do {
		unsigned long flags;

		/*
		 * Wait until we're out of the critical section.  This might
		 * give the wrong answer due to the lack of memory barriers.
		 */
		while (irqd_irq_inprogress(&desc->irq_data))
			cpu_relax();

		/* Ok, that indicated we're done: double-check carefully. */
		raw_spin_lock_irqsave(&desc->lock, flags);
		inprogress = irqd_irq_inprogress(&desc->irq_data);

		/*
		 * If requested and supported, check at the chip whether it
		 * is in flight at the hardware level, i.e. already pending
		 * in a CPU and waiting for service and acknowledge.
		 */
		if (!inprogress && sync_chip) {
			/*
			 * Ignore the return code. inprogress is only updated
			 * when the chip supports it.
			 */
			__irq_get_irqchip_state(irqd, IRQCHIP_STATE_ACTIVE,
						&inprogress);
		}
		raw_spin_unlock_irqrestore(&desc->lock, flags);

		/* Oops, that failed? */
	} while (inprogress);
}
static void __synchronize_irq(struct irq_desc *desc)
{
	__synchronize_hardirq(desc, true);
	/*
	 * We made sure that no hardirq handler is running. Now verify that no
	 * threaded handlers are active.
	 */
	wait_event(desc->wait_for_threads, !atomic_read(&desc->threads_active));
}
static struct irq_chip unsupported, capable = {state_callback};
static struct irq_desc desc;
static struct irq_data parent;
static void fresh(void) {
 memset(&desc,0,sizeof(desc)); memset(&parent,0,sizeof(parent));
 desc.irq_data.chip=&unsupported;
 locks=unlocks=relaxes=hardware_queries=thread_waits=warnings=0;
 busy_reads=hardware_active_reads=0; race_on_lock=physical_pending=busy_forever=false; query_error=0;
}
int main(void) {
 bool state;
 /* Missing callback cannot produce a physical ACTIVE sample. */
 fresh(); state=true; physical_pending=true;
 CHECK(__irq_get_irqchip_state(&desc.irq_data,IRQCHIP_STATE_ACTIVE,&state)==-EINVAL);
 CHECK(state && physical_pending && !hardware_queries);
 __synchronize_hardirq(&desc,true);
 CHECK(locks==1 && unlocks==1 && physical_pending && !hardware_queries);
 /* Query error is ignored by synchronization; physical pending stays unobserved. */
 fresh(); desc.irq_data.chip=&capable; query_error=-EIO; physical_pending=true;
 state=true; CHECK(__irq_get_irqchip_state(&desc.irq_data,IRQCHIP_STATE_ACTIVE,&state)==-EIO);
 CHECK(state); hardware_queries=0;
 __synchronize_hardirq(&desc,true);
 CHECK(locks==1 && hardware_queries==1 && physical_pending);
 /* Optional successful chip support adds ACTIVE polling, only in sync_chip mode. */
 fresh(); desc.irq_data.chip=&capable; hardware_active_reads=2;
 __synchronize_hardirq(&desc,true);
 CHECK(locks==3 && unlocks==3 && hardware_queries==3 && !hardware_active_reads);
 fresh(); desc.irq_data.chip=&capable; hardware_active_reads=2;
 __synchronize_hardirq(&desc,false);
 CHECK(locks==1 && !hardware_queries && hardware_active_reads==2);
 /* Software hardirq progress and lock recheck actually wait. */
 fresh(); busy_reads=2;
 __synchronize_hardirq(&desc,true);
 CHECK(relaxes==2 && locks==1 && unlocks==1);
 fresh(); busy_reads=2; race_on_lock=true;
 __synchronize_hardirq(&desc,true);
 CHECK(relaxes==3 && locks==2 && unlocks==2 && !busy_reads);
 /* Missing chip is a failed query, not an IRQ-drained hardware receipt. */
 fresh(); desc.irq_data.chip=NULL; physical_pending=true;
 state=true; CHECK(__irq_get_irqchip_state(&desc.irq_data,IRQCHIP_STATE_ACTIVE,&state)==-ENODEV);
 CHECK(state && warnings==1);
 __synchronize_hardirq(&desc,true);
 CHECK(locks==1 && warnings==2 && physical_pending);
 /* Generic optional hierarchy differs from Cascadia's actual linear AIC domain. */
 fresh(); parent.chip=&capable; desc.irq_data.parent_data=&parent; hardware_active_reads=2;
 __synchronize_hardirq(&desc,true);
#ifdef CONFIG_IRQ_DOMAIN_HIERARCHY
 CHECK(locks==3 && hardware_queries==3 && !hardware_active_reads);
#else
 CHECK(locks==1 && !hardware_queries && hardware_active_reads==2);
#endif
 /* Threaded callbacks are waited after the real hardirq synchronization body. */
 for (unsigned int n=0;n<8;n++) {
  fresh(); desc.threads_active=(int)n; physical_pending=true;
  __synchronize_irq(&desc);
  CHECK(!desc.threads_active && thread_waits==n && locks==1 && physical_pending);
 }
 /* Permanently active handler does not return a timeout: stop only in observer. */
 fresh(); busy_forever=true;
 if (!setjmp(bounded_observer)) {
  __synchronize_hardirq(&desc,true);
  assert(!"source unexpectedly returned for permanently active handler");
 }
 CHECK(relaxes==101 && !locks);
 printf("PASS %u assertions; actual Linux IRQ functions; hierarchy=%d; missing-hook/EIO/pending/thread/nonreturn controls\n",checks,
#ifdef CONFIG_IRQ_DOMAIN_HIERARCHY
 1
#else
 0
#endif
 );
 return 0;
}
