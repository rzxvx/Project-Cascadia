/*
 * fb-text — fill background, optional color bars, draw ASCII strings.
 *
 * Linked @ 0x80000000.  iBEC direct.dfu ep4 magic jump (same as memprobe/fb-probe).
 * No autogo / EOF staging — entire payload fits in 64 KiB.
 */

	.arch	armv7-a
	.syntax	unified
	.arm

	.equ	WDT_BASE,	0x0F103020

#include "fb-text-config.inc"
#include "font8x8.inc"

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

	ldr	r4, =SCANOUT
	ldr	r5, =STRIDE
	ldr	r6, =BG_COLOR
	ldr	r7, =CHAR_SCALE

	/* Full-screen background */
	mov	r0, #0
	mov	r1, #0
	ldr	r2, =FB_WIDTH
	ldr	r3, =FB_HEIGHT
	bl	fill_rect

	/* Optional RGBW bars across the top */
	ldr	r8, =USE_BARS
	cmp	r8, #0
	beq	2f
	mov	r1, #0
	ldr	r3, =BAR_H
	ldr	r6, =BAR0
	mov	r0, #0
	mov	r2, #192
	bl	fill_rect
	mov	r0, #192
	ldr	r6, =BAR1
	mov	r2, #192
	bl	fill_rect
	mov	r0, #384
	ldr	r6, =BAR2
	mov	r2, #192
	bl	fill_rect
	mov	r0, #576
	ldr	r6, =BAR3
	mov	r2, #192
	bl	fill_rect
2:
	ldr	r6, =FG_COLOR
	ldr	r7, =CHAR_SCALE
	ldr	r8, =NUM_LINES
	ldr	r9, =line_table
	mov	r10, #0
3:
	cmp	r10, r8
	bge	4f
	ldr	r0, [r9, r10, lsl #2]
	ldr	r1, =TEXT_X
	ldr	r2, =TEXT_Y
	ldr	r3, =LINE_HEIGHT
	mul	r3, r10, r3
	add	r2, r2, r3
	bl	draw_string
	add	r10, r10, #1
	b	3b
4:
	bl	v7_flush_dcache_all
	dsb
	isb

halt:
	wfi
	b	halt

/*
 * fill_rect — r0=x, r1=y, r2=w, r3=h; color in r6; uses r4=base, r5=stride.
 */
fill_rect:
	push	{r4, r5, r6, r7, r8, r9, lr}
	mov	r7, r0
	mov	r8, r1
	mov	r9, r3
1:
	cmp	r9, #0
	beq	.Lfill_done
	mov	r0, r8
	mul	r0, r5, r0
	mov	r1, r7
	lsl	r1, r1, #2
	add	r0, r4, r0
	add	r0, r0, r1
	mov	r1, r2
.Lfill_row:
	cmp	r1, #0
	beq	.Lfill_next
	str	r6, [r0], #4
	subs	r1, r1, #1
	bne	.Lfill_row
.Lfill_next:
	add	r8, r8, #1
	subs	r9, r9, #1
	bne	1b
.Lfill_done:
	pop	{r4, r5, r6, r7, r8, r9, pc}

/*
 * put_pixel — r0=x, r1=y; color in r6.
 */
put_pixel:
	push	{r2, r3, r4, r5, lr}
	ldr	r4, =SCANOUT
	ldr	r5, =STRIDE
	mul	r2, r1, r5
	lsl	r3, r0, #2
	add	r2, r4, r2
	str	r6, [r2, r3]
	pop	{r2, r3, r4, r5, pc}

/*
 * draw_char — r0=code, r1=x, r2=y; color r6, scale r7.
 */
draw_char:
	push	{r4, r7, r8, r9, r10, r11, r12, lr}
	mov	r8, r1
	mov	r9, r2
	cmp	r0, #FONT_FIRST
	movlt	r0, #FONT_FIRST
	cmp	r0, #FONT_LAST
	movgt	r0, #FONT_FIRST
	sub	r0, r0, #FONT_FIRST
	lsl	r0, r0, #3
	ldr	r4, =font8x8
	add	r4, r4, r0
	mov	r10, #0
1:
	cmp	r10, #FONT_HEIGHT
	bge	.Lchar_done
	ldrb	r11, [r4, r10]
	mov	r12, #0
.Lchar_col:
	cmp	r12, #FONT_WIDTH
	bge	.Lchar_row
	/* font8x8: bit 0 = leftmost pixel */
	tst	r11, #1
	beq	.Lchar_skip
	mov	r0, r8
	mov	r1, r12
	mul	r1, r7, r1
	add	r0, r0, r1
	mov	r1, r9
	mov	r2, r10
	mul	r2, r7, r2
	add	r1, r1, r2
	mov	r2, #0
.Lchar_dy:
	mov	r3, #0
.Lchar_dx:
	push	{r0, r1, r2, r3, lr}
	add	r0, r0, r3
	add	r1, r1, r2
	bl	put_pixel
	pop	{r0, r1, r2, r3, lr}
	add	r3, r3, #1
	cmp	r3, r7
	blt	.Lchar_dx
	add	r2, r2, #1
	cmp	r2, r7
	blt	.Lchar_dy
.Lchar_skip:
	lsr	r11, r11, #1
	add	r12, r12, #1
	b	.Lchar_col
.Lchar_row:
	add	r10, r10, #1
	b	1b
.Lchar_done:
	pop	{r4, r7, r8, r9, r10, r11, r12, pc}

/*
 * draw_string — r0=ptr, r1=x, r2=y; color r6, scale r7.
 */
draw_string:
	push	{r4, r5, r8, lr}
	mov	r4, r0
	mov	r5, r1
	mov	r8, r2
1:
	ldrb	r0, [r4], #1
	cmp	r0, #0
	beq	.Lstr_done
	mov	r1, r5
	mov	r2, r8
	bl	draw_char
	mov	r0, #FONT_WIDTH
	mul	r0, r7, r0
	add	r5, r5, r0
	b	1b
.Lstr_done:
	pop	{r4, r5, r8, pc}

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
