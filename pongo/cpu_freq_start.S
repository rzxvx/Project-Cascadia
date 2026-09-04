	.arch armv7-a
	.syntax unified
	.arm
	.section .text.start, "ax"
	.global _start
	.global main
_start:
	cpsid	if
	msr	cpsr_c, #0xd3
	ldr	sp, =0x8000F000
	bl	main
1:	wfi
	b	1b
