/*
 * memprobe v3 — P105 Recovery.
 *
 * 1. Fill known-safe vram planes (never touch iBEC @ 0x9FF00000).
 * 2. Read AppleCLCD @ 0x3A100000 for a live scanout pointer (FIMD-style).
 * 3. If that pointer sits in vram, paint it too.
 * 4. Flush D-cache so CLCD DMA sees the pixels.
 */

	.arch	armv7-a
	.syntax	unified
	.arm

	.equ	UART_BASE,	0x32500000
	.equ	UART_UTRSTAT,	0x10
	.equ	UART_UTXH,	0x20
	.equ	UTRSTAT_TXE,	(1 << 2)
	.equ	WDT_BASE,	0x0F103020
	.equ	FB0,		0x9F6FC000
	.equ	FB1,		0x9F9FC000
	.equ	FB_MARKER,	0x9F6FBF00
	.equ	IBEC_BASE,	0x9FF00000
	.equ	CLCD_BASE,	0x3A100000
	.equ	CLCD_BUF0,	0xA0		/* VIDW00ADD0B0 */
	.equ	CLCD_BUF0S,	0x40A0		/* VIDW_BUF_START_S */
	.equ	FB_WIDTH,	768
	.equ	FB_HEIGHT,	1024
	.equ	GREEN,		0xFF00FF00
	.equ	MAGENTA,	0xFFFF00FF
	.equ	CYAN,		0xFF00FFFF

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

	/* Full FB0 green — 3 MiB, ends 0x9F9FC000 (below iBEC) */
	ldr	r4, =FB0
	ldr	r3, =GREEN
	ldr	r0, =FB_HEIGHT
	bl	paint_at

	/* Full FB1 magenta — ends 0x9FCFC000 (still below iBEC) */
	ldr	r4, =FB1
	ldr	r3, =MAGENTA
	ldr	r0, =FB_HEIGHT
	bl	paint_at

	/* Live CLCD scanout pointer, if it looks like vram */
	bl	clcd_find_fb
	cmp	r4, #0
	beq	1f
	ldr	r3, =CYAN
	mov	r0, #128
	bl	paint_at
1:
	bl	v7_flush_dcache_all
	dsb
	isb

halt:
	wfi
	b	halt

/*
 * clcd_find_fb — return r4 = DRAM pointer in [FB0, IBEC), else 0.
 * Read-only MMIO. Never write CLCD.
 */
clcd_find_fb:
	push	{r0, r1, r2, r5, lr}
	ldr	r5, =CLCD_BASE
	mov	r4, #0

	ldr	r1, [r5, #CLCD_BUF0]
	bl	fb_ptr_ok
	bne	2f

	ldr	r1, [r5, #0xA4]
	bl	fb_ptr_ok
	bne	2f

	ldr	r1, [r5, #0xA8]
	bl	fb_ptr_ok
	bne	2f

	ldr	r1, [r5, #0xAC]
	bl	fb_ptr_ok
	bne	2f

	movw	r2, #0x40A0
	ldr	r1, [r5, r2]
	bl	fb_ptr_ok
	bne	2f

	/* Scan first 0x200 bytes of CLCD for a vram pointer */
	mov	r2, #0
3:	ldr	r1, [r5, r2]
	bl	fb_ptr_ok
	bne	2f
	add	r2, r2, #4
	cmp	r2, #0x200
	blo	3b
	b	4f
2:	mov	r4, r1
4:	pop	{r0, r1, r2, r5, pc}

/* r1 = candidate. NZ if FB0 <= r1 && r1 + 128*4096 <= IBEC */
fb_ptr_ok:
	ldr	r0, =FB0
	cmp	r1, r0
	blo	9f
	ldr	r0, =IBEC_BASE
	sub	r0, r0, #(128 * 4096)
	cmp	r1, r0
	bhs	9f
	movs	r0, #1
	bx	lr
9:	movs	r0, #0
	bx	lr

/* r4=base, r3=color, r0=rows  (stride = FB_WIDTH * 4) */
paint_at:
	push	{r0, r1, r5, r6, r7, lr}
	mov	r5, r3
	mov	r6, r0
	ldr	r7, =FB_WIDTH
1:	mov	r0, r7
2:	str	r5, [r4], #4
	subs	r0, r0, #1
	bne	2b
	subs	r6, r6, #1
	bne	1b
	pop	{r0, r1, r5, r6, r7, pc}

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
