/*
 * fb-probe — red marker bar + green scanline, then halt.
 *
 * Linked @ 0x80000000 (P105 Recovery staging).  iBEC ep4 hook @ 0x2384C
 * checks cpsid-if magic and jumps after the first USB chunk (same as memprobe).
 *
 * Tune pongo/fb-probe-config.inc or scripts/fb-probe-config.py between boots.
 */

	.arch	armv7-a
	.syntax	unified
	.arm

	.equ	WDT_BASE,	0x0F103020
	.equ	CLCD_BASE,	0x3A100000
	.equ	MARKER_W,	64

#include "fb-probe-config.inc"

	.section .text.start, "ax"
	.global	_start
_start:
	cpsid	if
	msr	cpsr_c, #0xd3

	ldr	r0, =WDT_BASE
	mov	r1, #0
	str	r1, [r0, #0x00]
	str	r1, [r0, #0x04]
	str	r1, [r0, #0x08]
	str	r1, [r0, #0x0C]

	/* Pre-FB marker (64 px @ FB_PHYS — not on screen, calibrates scanout base) */
	ldr	r4, =FB_PHYS
	ldr	r3, =MARKER
	mov	r0, #MARKER_W
	bl	paint_pixels

	/* Visible scanout base = FB_PHYS + MARKER_SKIP pixels */
	mov	r4, #0
	mov	r5, #USE_CLCD
	cmp	r5, #0
	bne	1f
	ldr	r4, =FB_PHYS
	ldr	r0, =MARKER_SKIP
	lsl	r0, r0, #2
	add	r4, r4, r0
	b	2f
1:	ldr	r0, =CLCD_BASE
	ldr	r4, [r0, #0xA0]
2:
	/* dest = scanout + ROW*STRIDE + PIXEL_OFF*4 */
	ldr	r1, =STRIDE
	ldr	r0, =ROW
	mul	r6, r0, r1
	ldr	r0, =PIXEL_OFF
	lsl	r0, r0, #2
	add	r4, r4, r6
	add	r4, r4, r0

	ldr	r3, =GREEN
	ldr	r0, =PIXEL_COUNT
	bl	paint_pixels

	bl	v7_flush_dcache_all
	dsb
	isb

halt:
	wfi
	b	halt

/* r4 = dest, r3 = color, r0 = count */
paint_pixels:
	push	{r1, lr}
1:	str	r3, [r4], #4
	subs	r0, r0, #1
	bne	1b
	pop	{r1, pc}

v7_flush_dcache_all:
	push	{r4-r11, lr}
	dmb
	mrc	p15, 1, r0, c0, c0, 1
	ands	r3, r0, #0x7000000
	mov	r3, r3, lsr #23
	beq	.Lfinished
	mov	r10, #0
.Lloop_level:
	add	r2, r10, r10, lsr #1
	mov	r1, r0, lsr r2
	and	r1, r1, #7
	cmp	r1, #2
	blt	.Lskip_level
	mcr	p15, 2, r10, c0, c0, 0
	isb
	mrc	p15, 1, r1, c0, c0, 0
	and	r2, r1, #7
	add	r2, r2, #4
	movw	r4, #0x3ff
	ands	r4, r4, r1, lsr #3
	clz	r5, r4
	movw	r7, #0x7fff
	ands	r7, r7, r1, lsr #13
.Lloop_way:
	mov	r9, r7
.Lloop_set:
	orr	r11, r10, r4, lsl r5
	orr	r11, r11, r9, lsl r2
	mcr	p15, 0, r11, c7, c14, 2
	subs	r9, r9, #1
	bge	.Lloop_set
	subs	r4, r4, #1
	bge	.Lloop_way
.Lskip_level:
	add	r10, r10, #2
	cmp	r3, r10
	bgt	.Lloop_level
.Lfinished:
	mov	r10, #0
	mcr	p15, 2, r10, c0, c0, 0
	dsb
	isb
	pop	{r4-r11, pc}
