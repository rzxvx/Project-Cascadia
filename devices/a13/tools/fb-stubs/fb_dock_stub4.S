/* SPDX-License-Identifier: MIT
 *
 * Stub v4: settle whether dockchannel TX8 writes from the payload reach the
 * host at all, separated from the TX_FREE handshake. No memory reads, no FIFO
 * polling, rate-limited writes (no overrun). Visual phases encode progress:
 *
 *   1) fill framebuffer GREEN
 *   2) write '!' to TX8 32 times, each spaced by a delay (no polling)
 *   3) fill framebuffer BLUE, then halt
 *
 * green->blue + '!' on serial => TX8 writes work; v3 hung polling TX_FREE==0
 * ends BLUE,   no '!'         => TX8 writes never reach host -> use fb console
 * stays GREEN                 => hung in the write loop (unexpected)
 *
 * fb base 0x8fa298000, 0x5b0000 bytes, x2r10g10b10 (R[29:20] G[19:10] B[9:0]).
 * dockchannel data block 0x23d12c000, TX8 +0x04.
 */
	.section .text
	.globl _start
_start:
	b	real
	.long	0
	.quad	0
	.quad	0x1000
	.quad	0
	.quad	0
	.quad	0
	.quad	0
	.ascii	"ARM"
	.byte	0x64
	.long	0

	.align	6
real:
	/* x10 = fb base, x11 = word count constant */
	movz	x10, #0x8000
	movk	x10, #0xfa29, lsl #16
	movk	x10, #0x0008, lsl #32

	/* ---- green ---- */
	mov	x1, x10
	movz	x2, #0xc000
	movk	x2, #0x0016, lsl #16
	movz	w4, #0xfc00		/* G = 0x000ffc00 */
	movk	w4, #0x000f, lsl #16
1:	str	w4, [x1], #4
	subs	x2, x2, #1
	b.ne	1b

	/* ---- rate-limited '!' to TX8, no polling ---- */
	movz	x3, #0xc000
	movk	x3, #0x3d12, lsl #16
	movk	x3, #0x0002, lsl #32
	movz	w9, #32
2:	movz	w6, #0x21
	str	w6, [x3, #0x04]
	movz	x8, #0x100, lsl #16	/* ~16M iters delay */
3:	subs	x8, x8, #1
	b.ne	3b
	subs	w9, w9, #1
	b.ne	2b

	/* ---- blue ---- */
	mov	x1, x10
	movz	x2, #0xc000
	movk	x2, #0x0016, lsl #16
	movz	w4, #0x03ff		/* B = 0x000003ff */
4:	str	w4, [x1], #4
	subs	x2, x2, #1
	b.ne	4b

5:	b	5b
