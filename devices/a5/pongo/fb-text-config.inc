/*
 * P105 iPad mini 1 — fb-text defaults (768x1024 BGRA scanout).
 *
 * SCANOUT   0x9f6fc000 (= FB_PHYS + 64*4) — fixed live CLCD base
 * Font: font8x8.inc (ASCII 32-127), CHAR_SCALE 2
 * Tune lines via scripts/fb-text-config.py
 */
.set SCANOUT,	0x9f6fc000
.set FB_WIDTH,	768
.set FB_HEIGHT,	1024
.set STRIDE,	3072
.set TEXT_X,	24
.set TEXT_Y,	120
.set CHAR_SCALE,	2
.set LINE_HEIGHT,	22
.set NUM_LINES,	5
.set USE_BARS,	1
.set BAR_H,	48
.set BG_COLOR,	0xff1a1a2e
.set FG_COLOR,	0xffffffff
.set BAR0,	0xffff0000
.set BAR1,	0xff00ff00
.set BAR2,	0xff0000ff
.set BAR3,	0xffffffff

	.section .rodata.msgs, "a"
	.align 2
msg0:	.asciz "P105AP fb-text"
msg1:	.asciz "scanout 0x9F6FC000"
msg2:	.asciz "768x1024 stride 3072"
msg3:	.asciz "font8x8 scale 2"
msg4:	.asciz "direct.dfu ep4 OK"

	.align 2
line_table:
	.word msg0, msg1, msg2, msg3, msg4
