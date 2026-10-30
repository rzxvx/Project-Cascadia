/* SPDX-License-Identifier: MIT
 *
 * Pure framebuffer-white stub, to confirm the payload EXECUTES after a real
 * (mmu_shutdown) handoff with the MMU off - the environment Linux actually
 * boots in. No dockchannel, no memory reads beyond instruction fetch.
 *
 *   fills the whole framebuffer white, then halts.
 *
 * screen white  => payload executes with MMU off (cache fix OK) -> Linux runs
 * stays m1n1 logo => the cache-off/MMU-off handoff breaks execution
 *
 * fb base 0x8fa298000, 0x5b0000 bytes.
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
	movz	x1, #0x8000
	movk	x1, #0xfa29, lsl #16
	movk	x1, #0x0008, lsl #32
	movz	x2, #0xc000
	movk	x2, #0x0016, lsl #16
	movn	w4, #0			/* white */
1:	str	w4, [x1], #4
	subs	x2, x2, #1
	b.ne	1b
2:	b	2b
