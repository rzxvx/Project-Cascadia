	.arch	armv7-a
	.syntax	unified
	.arm

	.equ	WDT_BASE,	0x0F103020

	.global	vector_table
	.global	aic1_lab_irq
	.global	aic1_lab_exc
	.global	cpu_get_cpsr
	.global	cpu_probe_mrs
	.global	cpu_probe_bic
	.global	cpu_probe_msri
	.global	cpu_probe_i_clear
	.global	cpu_try_unmask_i
	.global	g_step
	.global	g_force_i
	.global	_start
	.global	lab_ret_ctx

	.section .bss
	.align	2
g_step:
	.word	0
	/* g_force_i lives in .bss from C (aic1_lab.c) */
	.global	secondary_flag
secondary_flag:
	.word	0
	/* Saved iBEC USB-callback context for call-return egress */
lab_ret_ctx:
	.word	0		/* lr */
	.word	0		/* sp */
	.word	0		/* cpsr */
	.word	0		/* vbar */

	.section .vectors, "ax", %progbits
	.align	5
vector_table:
	ldr	pc, =_start		/* 0x00 reset */
	ldr	pc, =undef_stub		/* 0x04 undef */
	ldr	pc, =svc_stub		/* 0x08 svc */
	ldr	pc, =prefetch_stub	/* 0x0C prefetch abort */
	ldr	pc, =data_abort_stub	/* 0x10 data abort */
	ldr	pc, =unused_stub	/* 0x14 reserved */
	ldr	pc, =aic1_irq_stub	/* 0x18 irq */
	ldr	pc, =fiq_stub		/* 0x1C fiq */

	.section .text.start, "ax"
_start:
	/* Preserve iBEC Recovery USB callback so we can return (autogo blx). */
	ldr	r0, =lab_ret_ctx
	str	lr, [r0, #0]
	str	sp, [r0, #4]
	mrs	r1, cpsr
	str	r1, [r0, #8]
	mrc	p15, 0, r1, c12, c0, 0
	str	r1, [r0, #12]

	cpsid	if
	msr	cpsr_c, #0xd3
	isb

	ldr	r0, =WDT_BASE
	mov	r1, #0
	str	r1, [r0, #0x00]
	str	r1, [r0, #0x04]
	str	r1, [r0, #0x08]
	str	r1, [r0, #0x0C]

	/*
	 * Exception stacks.  Missing ABT/UND stacks made fault handlers hang
	 * silently (sp garbage → stuck with no UNDEF/DABT text).
	 */
	msr	cpsr_c, #0xD2		/* IRQ */
	ldr	sp, =0x8000E000
	msr	cpsr_c, #0xD1		/* FIQ */
	ldr	sp, =0x8000D000
	msr	cpsr_c, #0xD7		/* ABT */
	ldr	sp, =0x8000C000
	msr	cpsr_c, #0xDB		/* UND */
	ldr	sp, =0x8000B000
	msr	cpsr_c, #0xD3		/* SVC */
	ldr	sp, =0x8000F000

	ldr	r0, =vector_table
	mcr	p15, 0, r0, c12, c0, 0
	isb

	bl	main

	/* EP4 direct.dfu enters via bx — must NOT return (lr garbage → iOS). */
1:	wfi
	b	1b

	/*
	 * Secondary stub: cpu1 may arrive with iBoot MMU on. Drop M/C/I, use
	 * absolute PA for the flag store (v33: start regs stick but no retire).
	 */
	.global	secondary_stub
	.align	5
secondary_stub:
	/* SCTLR: clear M, C, I */
	mrc	p15, 0, r0, c1, c0, 0
	bic	r0, r0, #0x1000		@ I
	bic	r0, r0, #0x0005		@ C|M
	mcr	p15, 0, r0, c1, c0, 0
	isb
	mov	r0, #0
	mcr	p15, 0, r0, c8, c7, 0	@ TLBIALL
	mcr	p15, 0, r0, c7, c5, 0	@ ICIALLU
	dsb
	isb
	movw	r0, #:lower16:secondary_flag
	movt	r0, #:upper16:secondary_flag
	movw	r1, #0x0001
	movt	r1, #0xC0DE
	str	r1, [r0]
	dsb
2:	wfi
	b	2b

	/*
	 * DRAM park tramp @0x800C3800: drop MMU first, write ALIVE via
	 * PC-relative slot (proves fetch even if abs mailbox VA wrong) and
	 * absolute mailbox, then cpsie i + poll GO.
	 */
	.global	secondary_park_tramp
	.global	secondary_park_tramp_end
	.align	5
secondary_park_tramp:
	mrc	p15, 0, r0, c1, c0, 0
	bic	r0, r0, #0x1000		@ I
	bic	r0, r0, #0x0005		@ C|M
	mcr	p15, 0, r0, c1, c0, 0
	isb
	mov	r0, #0
	mcr	p15, 0, r0, c8, c7, 0	@ TLBIALL
	mcr	p15, 0, r0, c7, c5, 0	@ ICIALLU
	dsb
	isb
	movw	r1, #0xBEEF
	movt	r1, #0xC0DE
	/* PC-relative alive — offset fixed in image */
	adr	r0, park_alive_slot
	str	r1, [r0]
	dsb
	movw	r0, #0x3F00
	movt	r0, #0x800C
	str	r1, [r0]
	dsb
	cpsie	i
1:
	ldr	r1, [r0]
	movw	r2, #0x0001
	movt	r2, #0xC0DE
	cmp	r1, r2
	bne	2f
	ldr	r1, [r0, #4]
	bx	r1
2:
	wfi
	b	1b
	.align	2
park_alive_slot:
	.word	0
secondary_park_tramp_end:

	/*
	 * IRQ hook for phys VBAR=0 (iBoot table): only cpu1 sets the flag;
	 * other CPUs jump to the saved original IRQ target.
	 */
	.global	secondary_irq_hook
	.align	5
secondary_irq_hook:
	sub	lr, lr, #4
	push	{r0-r3, r12, lr}
	mrc	p15, 0, r0, c0, c0, 5	@ MPIDR
	and	r0, r0, #0x3
	cmp	r0, #1
	bne	3f
	/* cpu1: drop MMU briefly and plant flag */
	mrc	p15, 0, r0, c1, c0, 0
	bic	r0, r0, #0x1000
	bic	r0, r0, #0x0005
	mcr	p15, 0, r0, c1, c0, 0
	isb
	movw	r0, #:lower16:secondary_flag
	movt	r0, #:upper16:secondary_flag
	movw	r1, #0x0001
	movt	r1, #0xC0DE
	str	r1, [r0]
	dsb
4:	wfi
	b	4b
3:
	pop	{r0-r3, r12, lr}
	ldr	r12, =g_saved_irq_target
	ldr	r12, [r12]
	cmp	r12, #0
	beq	5f
	bx	r12
5:
	movs	pc, lr

	.section .bss
	.align	2
	.global	g_saved_irq_target
g_saved_irq_target:
	.word	0

	.section .text.start, "ax"
.macro	lab_exc, kind
	stmdb	sp!, {r0-r3, r12, lr}
	mov	r0, #\kind
	mov	r1, lr
	mov	r2, #0
	bl	aic1_lab_exc
	b	.
.endm

undef_stub:
	lab_exc 1

svc_stub:
	lab_exc 6

prefetch_stub:
	sub	lr, lr, #4
	lab_exc 2

data_abort_stub:
	sub	lr, lr, #8
	stmdb	sp!, {r0-r3, r12, lr}
	mov	r0, #3
	mov	r1, lr
	mrc	p15, 0, r2, c5, c0, 0
	bl	aic1_lab_exc
	b	.

unused_stub:
	lab_exc 5

fiq_stub:
	lab_exc 4

/*
 * IRQ stub: if g_force_i != 0, set I in SPSR before return (safe against
 * sticky nIRQ storms).  Phase B clears g_force_i to test real I=0 hold.
 */
aic1_irq_stub:
	sub	lr, lr, #4
	cpsid	i
	push	{r0-r3, r12, lr}
	bl	aic1_lab_irq
	ldr	r0, =g_force_i
	ldr	r0, [r0]
	cmp	r0, #0
	beq	1f
	mrs	r1, spsr
	orr	r1, r1, #0x80
	msr	spsr_cxsf, r1
1:
	pop	{r0-r3, r12, lr}
	movs	pc, lr

cpu_get_cpsr:
	mrs	r0, cpsr
	bx	lr

cpu_probe_mrs:
	mrs	r0, cpsr
	bx	lr

cpu_probe_bic:
	mrs	r1, cpsr
	bic	r0, r1, #0x80
	bx	lr

/*
 * u32 cpu_probe_msri(void)
 * g_step: 1=entry 2=pre-msr 3=post-msr 4=post-isb 5=sampled 6=restored
 */
cpu_probe_msri:
	ldr	r3, =g_step
	mov	r0, #1
	str	r0, [r3]

	mrs	r1, cpsr
	bic	r2, r1, #0x80

	mov	r0, #2
	str	r0, [r3]

	msr	cpsr_c, r2

	mov	r0, #3
	str	r0, [r3]

	isb

	mov	r0, #4
	str	r0, [r3]

	mrs	r0, cpsr

	mov	r12, #5
	str	r12, [r3]

	msr	cpsr_c, r1
	isb

	mov	r12, #6
	str	r12, [r3]
	bx	lr

cpu_probe_i_clear:
	b	cpu_probe_msri

cpu_try_unmask_i:
	cmp	r0, #1
	beq	.bic_i
	cmp	r0, #2
	beq	.force53
	cpsie	i
	isb
	b	.read_save
.bic_i:
	mrs	r1, cpsr
	bic	r1, r1, #0x80
	msr	cpsr_c, r1
	isb
	b	.read_save
.force53:
	msr	cpsr_c, #0x53
	isb
.read_save:
	mrs	r0, cpsr
	push	{r0}
	cpsid	i
	isb
	pop	{r0}
	bx	lr
