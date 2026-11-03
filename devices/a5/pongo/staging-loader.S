/*
 * iBEC staging loader — 0x80000000. Magenta/cyan, then CBAR hex, then zImage.
 *
 * VIDW_BUF_SIZE @ 0x100 is packed PAGEWIDTH | (OFFSIZE << 13), same layout as
 * Samsung FIMD. Using the raw 32-bit value as bytes-per-line collapsed magenta
 * into two scanlines. Decode it, then paint stride/4 pixels so pad is filled.
 */

	.arch	armv7-a
	.syntax	unified
	.arm

	.equ	KERNEL_ENTRY,	0x80008000
	.equ	UART0_BASE,	0x32500000
	.equ	UART_UTRSTAT,	0x10
	.equ	UART_UTXH,	0x20
	.equ	UTRSTAT_TXE,	(1 << 2)
	.equ	WDT_BASE,	0x0F103020
	.equ	PL310_BASE,	0x3e000000
	.equ	CLCD_BASE,	0x3a100000
	.equ	DTB_GUESS,	0x3e100000
	.equ	FB_DEFAULT,	0x9f6fc000
	.equ	ATAGS,		0x80000100

#include "staging-generated.inc"

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

	/* Live pitch from CLCD ADD2 / (BUF_END-BUF_START)/768. */
	bl	clcd_stride_safe

	ldr	r0, =ATAGS
	mov	r1, #0
	mov	r2, #64
0:	str	r1, [r0], #4
	subs	r2, r2, #4
	bne	0b

	ldr	r0, =768
	ldr	r3, =0xffff00ff
	bl	paint_fb_full
	bl	v7_flush_dcache_all

	ldr	r0, =768
	ldr	r3, =0xff00ffff
	bl	paint_fb_full

	mrc	p15, 4, r3, c15, c0, 0
	lsr	r3, r3, #13
	lsl	r3, r3, #13
	push	{r3}

	ldr	r0, =DTB_GUESS
	cmp	r3, r0
	ldreq	r3, =0xffffff00
	ldrne	r3, =0xffff0000
	mov	r0, #20
	bl	paint_fb_full

	pop	{r3}
	push	{r3}
	bl	paint_cbar_bands
	pop	{r3}
	bl	patch_dtb_cbar
	bl	v7_flush_dcache_all

	/* CBAR bands are confirmed. Full-screen green = about to jump. */
	ldr	r0, =768
	ldr	r3, =0xff00ff00
	bl	paint_fb_full
	bl	v7_flush_dcache_all

	bl	l2x0_flush_all

	mrc	p15, 0, r0, c1, c0, 0
	bic	r0, r0, #(1 << 0)
	bic	r0, r0, #(1 << 2)
	bic	r0, r0, #(1 << 12)
	bic	r0, r0, #(1 << 11)
	mcr	p15, 0, r0, c1, c0, 0
	isb

	mov	r0, #0
	mcr	p15, 0, r0, c7, c5, 0
	mcr	p15, 0, r0, c7, c5, 6
	mcr	p15, 0, r0, c8, c7, 0
	dsb
	isb

	mov	r0, #0
	mvn	r1, #0
	ldr	r2, =DTB_PHYS
	ldr	r3, =KERNEL_ENTRY
	bx	r3

1:	wfi
	b	1b

clcd_stride_safe:
	push	{r0-r5, lr}
	ldr	r0, =CLCD_BASE
	ldr	r1, [r0, #0xA0]
	ldr	r2, [r0, #0xD0]
	ldr	r5, [r0, #0x100]
	ldr	r0, =0x9f000000
	cmp	r1, r0
	blo	1f
	ldr	r0, =0x9ff00000
	cmp	r1, r0
	bhs	1f
	adr	r0, fb_base
	str	r1, [r0]
1:	ldr	r4, =0x1fff
	and	r3, r5, r4
	lsr	r5, r5, #13
	and	r5, r5, r4
	cmp	r3, #4096
	blo	2f
	add	r3, r3, r5
	cmp	r3, #8192
	bhi	2f
	tst	r3, #3
	bne	2f
	b	8f
2:	subs	r2, r2, r1
	bls	9f
	lsr	r4, r2, #8
	ldr	r5, =0xaaaaaaab
	umull	r0, r5, r4, r5
	lsr	r3, r5, #1
	mov	r4, r3, lsl #8
	add	r4, r4, r3, lsl #9
	cmp	r4, r2
	bne	9f
	cmp	r3, #4096
	blo	9f
	cmp	r3, #8192
	bhi	9f
	tst	r3, #3
	bne	9f
8:	adr	r0, fb_stride
	str	r3, [r0]
	lsr	r3, r3, #2
	adr	r0, fb_pix
	str	r3, [r0]
9:	pop	{r0-r5, pc}

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

l2x0_flush_all:
	push	{r0, r1, r2, lr}
	ldr	r0, =PL310_BASE
	ldr	r1, [r0]
	lsr	r2, r1, #6
	and	r2, r2, #0x3f
	cmp	r2, #3
	cmpne	r2, #2
	cmpne	r2, #1
	bne	2f
	ldr	r1, [r0, #0x100]
	tst	r1, #1
	beq	2f
	mov	r1, #0xff00
	orr	r1, r1, #0xff
	str	r1, [r0, #0x7fc]
1:	ldr	r1, [r0, #0x7fc]
	cmp	r1, #0
	bne	1b
	mov	r1, #0
	str	r1, [r0, #0x730]
1:	ldr	r1, [r0, #0x730]
	tst	r1, #1
	bne	1b
	dsb
2:	pop	{r0, r1, r2, pc}

/*
 * r3 = live CBAR. Rewrite DTB_GUESS timer cells and 0x9f6fc000 fb base.
 */
patch_dtb_cbar:
	push	{r0-r6, lr}
	ldr	r0, =DTB_PHYS
	ldr	r1, =DTB_SIZE
	add	r1, r0, r1
	ldr	r5, =DTB_GUESS
	ldr	r6, =FB_DEFAULT
	adr	r4, fb_base
	ldr	r6, [r4]
1:	cmp	r0, r1
	bhs	4f
	ldr	r2, [r0]
	rev	r2, r2
	cmp	r2, r5
	beq	2f
	add	r4, r5, #0x200
	cmp	r2, r4
	beq	2f
	add	r4, r5, #0x600
	cmp	r2, r4
	beq	2f
	ldr	r4, =FB_DEFAULT
	cmp	r2, r4
	bne	5f
	rev	r4, r6
	str	r4, [r0]
	b	3f
5:	ldr	r4, =4112
	cmp	r2, r4
	bne	3f
	adr	r4, fb_stride
	ldr	r4, [r4]
	rev	r4, r4
	str	r4, [r0]
2:	sub	r4, r2, r5
	add	r4, r4, r3
	rev	r4, r4
	str	r4, [r0]
3:	add	r0, r0, #4
	b	1b
4:	pop	{r0-r6, pc}

/* r0 = rows, r3 = color. Starts at fb_base. */
paint_fb_full:
	ldr	r4, fb_base
	b	paint_rows

/*
 * r0 = rows, r3 = color, r4 = dest. Leaves r4 at the next scanline.
 * r7 (caller) is preserved.
 */
paint_rows:
	push	{r0-r3, r5-r7, lr}
	mov	r5, r3
	mov	r6, r0
	ldr	r7, fb_pix
	ldr	r1, fb_stride
	sub	r1, r1, r7, lsl #2
1:	mov	r0, r7
2:	str	r5, [r4], #4
	subs	r0, r0, #1
	bne	2b
	add	r4, r4, r1
	subs	r6, r6, #1
	bne	1b
	pop	{r0-r3, r5-r7, pc}

/* 8 horizontal bands, one nybble each, from the top of the remaining cyan. */
paint_cbar_bands:
	push	{r3-r10, lr}
	mov	r10, r3
	ldr	r4, fb_base
	ldr	r9, fb_stride
	mov	r0, #48
	mul	r1, r0, r9
	add	r4, r4, r1
	mov	r7, #28
1:	mov	r0, r10, lsr r7
	and	r0, r0, #15
	adr	r1, nybble_color
	ldr	r3, [r1, r0, lsl #2]
	mov	r0, #40
	bl	paint_rows
	subs	r7, r7, #4
	bge	1b
	pop	{r3-r10, pc}

	.align	2
fb_base:	.word	FB_DEFAULT
fb_pix:		.word	1024
fb_stride:	.word	4112

nybble_color:
	.word	0xff000000	@ 0 black
	.word	0xff0000aa	@ 1 blue
	.word	0xff00aa00	@ 2 green
	.word	0xffffffff	@ 3 white
	.word	0xffaa0000	@ 4 dark red
	.word	0xffaa00aa	@ 5 purple
	.word	0xffff8000	@ 6 orange
	.word	0xff555555	@ 7 gray
	.word	0xff5555ff	@ 8 light blue
	.word	0xff55ff55	@ 9 light green
	.word	0xffffff00	@ A yellow
	.word	0xff8b4513	@ B brown
	.word	0xffff0000	@ C red
	.word	0xffff00ff	@ D magenta
	.word	0xff808000	@ E olive
	.word	0xffff80c0	@ F pink
	.align	2
