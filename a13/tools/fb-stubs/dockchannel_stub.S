/* SPDX-License-Identifier: MIT
 *
 * Bare-metal dockchannel TX test stub for N104 (iPhone 11) Linux bring-up.
 *
 * m1n1 boots this exactly like the Linux kernel: entered at EL1 with the MMU
 * off and x0 = DTB phys. It ignores x0 and just hammers a marker string into
 * the dockchannel UART data FIFO (same MMIO m1n1 itself prints through), to
 * prove the handoff + our dockchannel register offsets work from a booted
 * payload. If "M1N1-STUB-ALIVE" shows on the DCSD cable, the silent Linux boot
 * is a genuine early-kernel hang (before earlycon), not a dead output channel.
 *
 * Carries a minimal arm64 Image header so m1n1's payload scanner treats it as
 * a kernel (magic "ARM\x64" at 0x38) and jumps to offset 0.
 *
 * Dockchannel data block = 0x23d128000 + 0x4000 = 0x23d12c000
 *   TX8      = +0x04 (write byte), TX_FREE = +0x14 (read != 0 => space)
 */
	.section .text
	.globl _start
_start:
	b	real		/* code0: branch past the 64-byte header   */
	.long	0		/* code1                                    */
	.quad	0		/* text_offset                              */
	.quad	0x1000		/* image_size (padded file size)            */
	.quad	0		/* flags                                    */
	.quad	0		/* res2                                     */
	.quad	0		/* res3                                     */
	.quad	0		/* res4                                     */
	.ascii	"ARM"		/* magic at offset 0x38 ...                 */
	.byte	0x64		/* ... "ARM\x64"                            */
	.long	0		/* res5 (PE header offset, none)            */

	.align	6		/* real entry at offset 0x40                */
real:
	/* x1 = 0x23d12c000 (dockchannel data block) */
	movz	x1, #0xc000
	movk	x1, #0x3d12, lsl #16
	movk	x1, #0x0002, lsl #32
loop:
	adr	x2, msg
char:
	ldrb	w3, [x2], #1
	cbz	w3, delay
wait:
	ldr	w4, [x1, #0x14]		/* TX_FREE */
	cbz	w4, wait
	str	w3, [x1, #0x04]		/* TX8     */
	b	char
delay:
	movz	x5, #0x200, lsl #16	/* ~0x2000000 iters between repeats */
dloop:
	subs	x5, x5, #1
	b.ne	dloop
	b	loop

	.align	3
msg:
	.ascii	"M1N1-STUB-ALIVE\r\n\0"
