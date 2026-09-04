	.arch armv7-a
	.syntax unified
	.arm
	.section .text.start, "ax"
	.global _start
	.global main
_start:
	cpsid	if
	msr	cpsr_c, #0xd3
	isb

	/*
	 * iBEC Recovery runs with MMU on. GPIO/PMGR/scanout are mapped; SPI1
	 * @ 0x32100000 is not — reads as 0 and writes do not stick. Drop
	 * M/C/I so MMIO uses physical addresses (same as aic1 secondary_stub).
	 */
	mrc	p15, 0, r0, c1, c0, 0
	bic	r0, r0, #0x1000		@ I-cache
	bic	r0, r0, #0x0005		@ D-cache | MMU
	mcr	p15, 0, r0, c1, c0, 0
	isb
	mov	r0, #0
	mcr	p15, 0, r0, c8, c7, 0	@ TLBIALL
	mcr	p15, 0, r0, c7, c5, 0	@ ICIALLU
	dsb
	isb

	ldr	sp, =0x8000F000
	bl	main
1:	wfi
	b	1b
