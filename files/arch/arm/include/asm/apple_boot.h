/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __ASM_ARM_APPLE_BOOT_H
#define __ASM_ARM_APPLE_BOOT_H

#include <linux/gfp.h>
#include <linux/irqflags.h>
#include <linux/rwlock.h>
#include <linux/spinlock.h>

extern bool early_boot_irqs_disabled;

static inline bool apple_boot_irqs_off(void)
{
	return early_boot_irqs_disabled;
}

static inline gfp_t apple_boot_gfp(gfp_t gfp)
{
	if (apple_boot_irqs_off() || irqs_disabled())
		return (gfp & (__GFP_ZERO | __GFP_ACCOUNT | __GFP_COMP |
			       __GFP_NOWARN)) | GFP_ATOMIC;
	return gfp;
}

/*
 * Never use *_irq() variants here.  Sticky nIRQ means spin_unlock_irq() /
 * write_unlock_irq() → cpsie → infinite IRQ storm once early_irq_escape is
 * off (cp_sig hang).  Plain lock/unlock; caller already manages CPSR.I.
 */
static inline void apple_spin_lock(spinlock_t *lock)
{
	spin_lock(lock);
}

static inline void apple_spin_unlock(spinlock_t *lock)
{
	spin_unlock(lock);
}

static inline void apple_write_lock(rwlock_t *lock)
{
	write_lock(lock);
}

static inline void apple_write_unlock(rwlock_t *lock)
{
	write_unlock(lock);
}

#endif
