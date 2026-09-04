/*
 * gpio-poll — dump GPIO MMIO + live-poll ADT button pins on P105 scanout.
 *
 * Linked @ 0x80000000.  direct.dfu ep4 magic jump (same as fb-text).
 * Read-only GPIO MMIO — no pinmux rewrite.
 *
 * Layout:
 *   Apple-style:  *(GPIO_BASE + 4*pin) bit0 = data
 *   Samsung-style PDAT: *(GPIO_BASE + pin*0x20 + 4)
 * Both shown; change-detect flashes the top bar green.
 */

	.arch	armv7-a
	.syntax	unified
	.arm

	.equ	WDT_BASE,	0x0F103020
	.equ	GPIO_BASE,	0x3FA00000
	.equ	SCANOUT,	0x9F6FC000
	.equ	FB_WIDTH,	768
	.equ	FB_HEIGHT,	1024
	.equ	STRIDE,		3072
	.equ	CHAR_SCALE,	2
	.equ	LINE_H,		20
	.equ	BAR_H,		40
	.equ	BG_COLOR,	0xFF101018
	.equ	FG_COLOR,	0xFFFFFFFF
	.equ	DIM_COLOR,	0xFF808090
	.equ	BAR_IDLE,	0xFF303040
	.equ	BAR_HIT,	0xFF00FF00
	.equ	DUMP_WORDS,	32
	.equ	POLL_PINS,	5

#include "font8x8.inc"

	.section .data
	.align	4
prev_apple:	.word	0, 0, 0, 0, 0
prev_sam:	.word	0, 0, 0, 0, 0
line_buf:	.space	48

	.section .rodata.msgs, "a"
	.align	2
title:		.asciz "P105 gpio-poll @ 3FA00000"
hint:		.asciz "press Home / vol / hold"
lbl_apple:	.asciz "Apple pin+4*n bit0:"
lbl_sam:	.asciz "Samsung pin*20+4:"
lbl_dump:	.asciz "GPIO dump (words):"
names:		.asciz "MENU"
		.asciz "HOLD"
		.asciz "VOL+"
		.asciz "VOL-"
		.asciz "RING"

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

	/* Clear prev_* + line_buf */
	ldr	r0, =prev_apple
	mov	r1, #0
	mov	r2, #(POLL_PINS * 4 * 2 + 48)
1:	strb	r1, [r0], #1
	subs	r2, r2, #1
	bne	1b

	ldr	r7, =CHAR_SCALE

	/* Background */
	ldr	r6, =BG_COLOR
	mov	r0, #0
	mov	r1, #0
	ldr	r2, =FB_WIDTH
	ldr	r3, =FB_HEIGHT
	bl	fill_rect

	/* Idle status bar */
	ldr	r6, =BAR_IDLE
	mov	r0, #0
	mov	r1, #0
	ldr	r2, =FB_WIDTH
	ldr	r3, =BAR_H
	bl	fill_rect

	ldr	r6, =FG_COLOR
	ldr	r0, =title
	mov	r1, #8
	mov	r2, #48
	bl	draw_string
	ldr	r0, =hint
	mov	r1, #8
	mov	r2, #68
	bl	draw_string

	/* Static dump header + hex dump (once) */
	ldr	r0, =lbl_dump
	mov	r1, #8
	mov	r2, #360
	bl	draw_string
	bl	draw_gpio_dump

	bl	v7_flush_dcache_all

/* ---- live poll loop ---- */
poll_loop:
	mov	r8, #0			/* change flag */

	/* Apple-style reads */
	mov	r9, #0
apple_scan:
	cmp	r9, #POLL_PINS
	bge	sam_scan_prep
	ldr	r0, =GPIO_BASE
	lsl	r1, r9, #2
	ldr	r0, [r0, r1]
	and	r0, r0, #1
	ldr	r1, =prev_apple
	ldr	r2, [r1, r9, lsl #2]
	cmp	r0, r2
	movne	r8, #1
	str	r0, [r1, r9, lsl #2]
	add	r9, r9, #1
	b	apple_scan

sam_scan_prep:
	mov	r9, #0
sam_scan:
	cmp	r9, #POLL_PINS
	bge	redraw
	ldr	r0, =GPIO_BASE
	mov	r1, #0x20
	mul	r1, r9, r1
	add	r0, r0, r1
	ldr	r0, [r0, #4]
	and	r0, r0, #1
	ldr	r1, =prev_sam
	ldr	r2, [r1, r9, lsl #2]
	cmp	r0, r2
	movne	r8, #1
	str	r0, [r1, r9, lsl #2]
	add	r9, r9, #1
	b	sam_scan

redraw:
	/* Status bar: green if any change this frame, else idle */
	cmp	r8, #0
	ldrne	r6, =BAR_HIT
	ldreq	r6, =BAR_IDLE
	mov	r0, #0
	mov	r1, #0
	ldr	r2, =FB_WIDTH
	ldr	r3, =BAR_H
	bl	fill_rect

	/* Clear text region for live lines */
	ldr	r6, =BG_COLOR
	mov	r0, #0
	mov	r1, #88
	ldr	r2, =FB_WIDTH
	mov	r3, #260
	bl	fill_rect

	ldr	r6, =FG_COLOR
	ldr	r0, =lbl_apple
	mov	r1, #8
	mov	r2, #96
	bl	draw_string

	mov	r9, #0
	mov	r10, #116			/* y */
draw_apple_lines:
	cmp	r9, #POLL_PINS
	bge	draw_sam_hdr
	bl	format_pin_line_apple
	ldr	r0, =line_buf
	mov	r1, #8
	mov	r2, r10
	ldr	r6, =FG_COLOR
	bl	draw_string
	add	r10, r10, #LINE_H
	add	r9, r9, #1
	b	draw_apple_lines

draw_sam_hdr:
	ldr	r6, =DIM_COLOR
	ldr	r0, =lbl_sam
	mov	r1, #8
	mov	r2, r10
	bl	draw_string
	add	r10, r10, #LINE_H

	mov	r9, #0
draw_sam_lines:
	cmp	r9, #POLL_PINS
	bge	flush_frame
	bl	format_pin_line_sam
	ldr	r0, =line_buf
	mov	r1, #8
	mov	r2, r10
	ldr	r6, =DIM_COLOR
	bl	draw_string
	add	r10, r10, #LINE_H
	add	r9, r9, #1
	b	draw_sam_lines

flush_frame:
	bl	v7_flush_dcache_all

	/* Crude delay so the green flash is visible */
	ldr	r0, =0x200000
1:	subs	r0, r0, #1
	bne	1b

	b	poll_loop

/*
 * format_pin_line_apple — r9 = pin index → line_buf "NAME n=0 raw=XXXXXXXX"
 */
format_pin_line_apple:
	push	{r0-r3, r4, r5, lr}
	ldr	r0, =line_buf
	bl	copy_name_to_buf		/* r0 advanced */
	mov	r1, #' '
	strb	r1, [r0], #1
	add	r1, r9, #'0'
	strb	r1, [r0], #1
	mov	r1, #'='
	strb	r1, [r0], #1
	ldr	r1, =prev_apple
	ldr	r1, [r1, r9, lsl #2]
	add	r1, r1, #'0'
	strb	r1, [r0], #1
	mov	r1, #' '
	strb	r1, [r0], #1
	mov	r1, #'r'
	strb	r1, [r0], #1
	mov	r1, #'='
	strb	r1, [r0], #1
	ldr	r1, =GPIO_BASE
	ldr	r1, [r1, r9, lsl #2]
	bl	append_hex32
	mov	r1, #0
	strb	r1, [r0]
	pop	{r0-r3, r4, r5, pc}

format_pin_line_sam:
	push	{r0-r3, r4, r5, lr}
	ldr	r0, =line_buf
	bl	copy_name_to_buf
	mov	r1, #' '
	strb	r1, [r0], #1
	add	r1, r9, #'0'
	strb	r1, [r0], #1
	mov	r1, #'='
	strb	r1, [r0], #1
	ldr	r1, =prev_sam
	ldr	r1, [r1, r9, lsl #2]
	add	r1, r1, #'0'
	strb	r1, [r0], #1
	mov	r1, #' '
	strb	r1, [r0], #1
	mov	r1, #'r'
	strb	r1, [r0], #1
	mov	r1, #'='
	strb	r1, [r0], #1
	ldr	r2, =GPIO_BASE
	mov	r1, #0x20
	mul	r1, r9, r1
	add	r2, r2, r1
	ldr	r1, [r2, #4]
	bl	append_hex32
	mov	r1, #0
	strb	r1, [r0]
	pop	{r0-r3, r4, r5, pc}

/* r0 = dest; r9 = pin; advances r0 past name */
copy_name_to_buf:
	push	{r1, r2, r3, lr}
	ldr	r1, =names
	mov	r2, r9
1:	cmp	r2, #0
	beq	2f
3:	ldrb	r3, [r1], #1
	cmp	r3, #0
	bne	3b
	subs	r2, r2, #1
	b	1b
2:	ldrb	r3, [r1], #1
	cmp	r3, #0
	beq	4f
	strb	r3, [r0], #1
	b	2b
4:	pop	{r1, r2, r3, pc}

/* r0 = dest ptr; r1 = value; advances r0 by 8 */
append_hex32:
	push	{r2, r3, r4, lr}
	mov	r4, #8
1:	mov	r2, r1, lsr #28
	and	r2, r2, #0xf
	cmp	r2, #10
	addlt	r2, r2, #'0'
	addge	r2, r2, #('A' - 10)
	strb	r2, [r0], #1
	lsl	r1, r1, #4
	subs	r4, r4, #1
	bne	1b
	pop	{r2, r3, r4, pc}

/*
 * draw_gpio_dump — 8 rows x 4 words of GPIO_BASE
 */
draw_gpio_dump:
	push	{r4-r11, lr}
	ldr	r8, =GPIO_BASE
	mov	r9, #0			/* word index */
	mov	r10, #380		/* y */
dump_row:
	cmp	r9, #DUMP_WORDS
	bge	dump_done
	ldr	r0, =line_buf
	lsl	r1, r9, #2
	mov	r2, r1, lsr #4
	and	r2, r2, #0xf
	cmp	r2, #10
	addlt	r2, r2, #'0'
	addge	r2, r2, #('A' - 10)
	strb	r2, [r0], #1
	and	r2, r1, #0xf
	cmp	r2, #10
	addlt	r2, r2, #'0'
	addge	r2, r2, #('A' - 10)
	strb	r2, [r0], #1
	mov	r2, #':'
	strb	r2, [r0], #1
	mov	r11, #0
dump_cols:
	cmp	r11, #4
	bge	dump_emit
	mov	r2, #' '
	strb	r2, [r0], #1
	ldr	r1, [r8, r9, lsl #2]
	bl	append_hex32
	add	r9, r9, #1
	add	r11, r11, #1
	b	dump_cols
dump_emit:
	mov	r2, #0
	strb	r2, [r0]
	ldr	r0, =line_buf
	mov	r1, #8
	mov	r2, r10
	ldr	r6, =DIM_COLOR
	ldr	r7, =CHAR_SCALE
	bl	draw_string
	add	r10, r10, #LINE_H
	b	dump_row
dump_done:
	pop	{r4-r11, pc}

/* ---- fb draw (same contract as fb-text) ---- */

fill_rect:
	push	{r4, r5, r6, r7, r8, r9, lr}
	ldr	r4, =SCANOUT
	ldr	r5, =STRIDE
	mov	r7, r0
	mov	r8, r1
	mov	r9, r3
1:
	cmp	r9, #0
	beq	.Lfill_done
	mul	r0, r8, r5
	lsl	r1, r7, #2
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

put_pixel:
	push	{r2, r3, r4, r5, lr}
	ldr	r4, =SCANOUT
	ldr	r5, =STRIDE
	mul	r2, r1, r5
	lsl	r3, r0, #2
	add	r2, r4, r2
	str	r6, [r2, r3]
	pop	{r2, r3, r4, r5, pc}

draw_char:
	push	{r4, r7, r8, r9, r10, r11, r12, lr}
	ldr	r7, =CHAR_SCALE
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

draw_string:
	push	{r4, r5, r8, lr}
	mov	r4, r0
	mov	r5, r1
	mov	r8, r2
	ldr	r7, =CHAR_SCALE
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
