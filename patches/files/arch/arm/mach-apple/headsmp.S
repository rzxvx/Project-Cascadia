/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Where a core coming out of reset lands, through the trampoline in the first
 * page of DRAM (platsmp.c).  MMU and caches are off; the core is not in SMP
 * mode yet.
 */
#include <linux/linkage.h>
#include <asm/assembler.h>

	.arm

/*
 * Invalidate this core's duplicate tags in the SCU (SCU + 0x0c, four bits
 * per core), as iBoot does when it resets a core: nothing the SCU remembers
 * about this core's L1 from before the power went is true any more.
 * Corrupts r0-r2.
 */
apple_s5l_scu_inv_self:
	mrc	p15, 4, r0, c15, c0, 0		@ CBAR: the SCU
	mrc	p15, 0, r1, c0, c0, 5		@ MPIDR
	and	r1, r1, #3
	lsl	r1, r1, #2
	mov	r2, #0xf
	lsl	r2, r2, r1
	str	r2, [r0, #0x0c]
	dsb
	ret	lr

/* First start of a secondary: __v7_ca9mp_setup cleans its L1 itself. */
ENTRY(apple_s5l_secondary_startup)
	bl	apple_s5l_scu_inv_self
	b	secondary_startup
ENDPROC(apple_s5l_secondary_startup)

/*
 * Back from power-down idle: L1 holds garbage (an A9's L1 is not cleared by
 * reset), so invalidate it before cpu_resume turns SMP and the caches on.
 */
ENTRY(apple_s5l_cpu_resume)
	bl	v7_invalidate_l1
	bl	apple_s5l_scu_inv_self
	b	cpu_resume
ENDPROC(apple_s5l_cpu_resume)
