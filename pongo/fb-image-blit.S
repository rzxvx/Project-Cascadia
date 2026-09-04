/*
 * fb-image-blit — copy staged RGBA @ 0x80000000+IMAGE_OFF to scanout @ 0x9f6fc000.
 *
 * Packed upload (fb-image-pack.bin) via autogo iBEC (EOF jump after full receive):
 *   [this stub][pad to IMAGE_OFF][768x1024 BGRA pixels]
 *
 * P105 panel: 768x1024, stride 3072 B, scanout 0x9f6fc000.
 */

	.arch	armv7-a
	.syntax	unified
	.arm

	.equ	WDT_BASE,	0x0F103020
	.equ	SCANOUT,	0x9F6FC000
	.equ	IMAGE_OFF,	0x8000
	.equ	STAGING,	0x80000000
	.equ	SRC,		STAGING + IMAGE_OFF
	.equ	COPY_BYTES,	768 * 1024 * 4

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

	ldr	r0, =SRC
	ldr	r1, =SCANOUT
	ldr	r2, =COPY_BYTES
	bl	memcpy32

	bl	v7_flush_dcache_all
	dsb
	isb

halt:
	wfi
	b	halt

/* r0=src, r1=dst, r2=bytes (multiple of 4) */
memcpy32:
	push	{r4, lr}
1:	ldr	r3, [r0], #4
	str	r3, [r1], #4
	subs	r2, r2, #4
	bne	1b
	pop	{r4, pc}

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
