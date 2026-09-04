/*
 * Reference 32-bit ARM trampoline: hand off from a checkm8 payload to Linux.
 *
 * This is the last thing that runs before the kernel. Whatever got you code
 * execution -- a checkm8 DFU payload, a patched iBoot, kloader -- eventually
 * has to satisfy the ARM Linux boot protocol, and that contract is the same
 * regardless. This file is that contract, written out.
 *
 * IMPORTANT: read pongo/README.md before using this. pongoOS upstream is
 * AArch64-only and does not run on the A5; this trampoline exists precisely
 * because the 32-bit path needs its own loader.
 *
 * Entry state assumed: privileged mode, MMU state unknown, artifacts already
 * in DRAM at the addresses below.
 *
 * Exit state, per Documentation/arm/booting.rst:
 *   - MMU off, D-cache off, I-cache either way
 *   - r0 = 0
 *   - r1 = machine number, or ~0 when booting from a device tree
 *   - r2 = physical address of the DTB
 *   - CPU in SVC mode with IRQ and FIQ masked
 *
 * Build:
 *   arm-linux-gnueabihf-gcc -nostdlib -nostartfiles -Wl,-Ttext=0x87000000 \
 *       -o loader.elf loader.S
 *   arm-linux-gnueabihf-objcopy -O binary loader.elf loader.bin
 */

	.arch	armv7-a
	.syntax	unified
	.arm

/*
 * Memory map. Must match scripts/gen_p105ap_dts.py and the addresses the
 * upload step actually used -- these are duplicated knowledge and getting them
 * out of step is the single easiest way to produce a silent hang.
 *
 *   0x80000000  DRAM base (512 MiB, to 0xa0000000)
 *   0x80008000  zImage             (DRAM base + the usual 32 KiB TEXT_OFFSET)
 *   0x87000000  this trampoline    (clear of everything else)
 *   0x88000000  initramfs          (up to 128 MiB)
 *   0x90000000  DTB
 *   0x9fc00000  framebuffer        (iBoot's, do not touch)
 */
	.equ	KERNEL_ADDR,	0x80008000
	.equ	INITRD_ADDR,	0x88000000
	.equ	DTB_ADDR,	0x90000000

/* UART0, physical. Same address the kernel's earlycon uses. */
	.equ	UART0_BASE,	0x32500000
	.equ	UART_UTRSTAT,	0x10
	.equ	UART_UTXH,	0x20
	.equ	UTRSTAT_TXE,	(1 << 2)	/* transmitter empty */

	.equ	FB_BASE,	0x9f6fc000
	.equ	FB_MARKER,	0x00ff0000	@ RGB888 orange — visible if loader runs

	.section .text.start, "ax"
	.global	_start
_start:
	/*
	 * SVC mode, interrupts masked. Whatever was running before may have
	 * left us in a different mode with IRQs live; the kernel's early code
	 * assumes neither.
	 */
	cpsid	if
	msr	cpsr_c, #0xd3			@ SVC, I=1, F=1

	bl	uart_init_marker
	bl	print_cbar

	/*
	 * Clean and invalidate the data cache before turning it off, or
	 * anything we wrote (notably the patched DTB) could still be sitting in
	 * dirty lines when the kernel starts reading with caches disabled.
	 *
	 * DCCISW-by-set/way is deliberately not open-coded here: on a Cortex-A9
	 * with a PL310 behind it, the sequence is long and easy to get subtly
	 * wrong. If your payload already has a cache maintenance routine, call
	 * it here instead. What follows is the minimum that is correct on this
	 * part.
	 */
	bl	v7_flush_dcache_all

	/* Turn off MMU, D-cache and I-cache. */
	mrc	p15, 0, r0, c1, c0, 0		@ SCTLR
	bic	r0, r0, #(1 << 0)		@ M: MMU
	bic	r0, r0, #(1 << 2)		@ C: D-cache
	bic	r0, r0, #(1 << 12)		@ I: I-cache
	bic	r0, r0, #(1 << 11)		@ Z: branch prediction
	mcr	p15, 0, r0, c1, c0, 0
	isb

	/* Invalidate I-cache, branch predictor and the whole TLB. */
	mov	r0, #0
	mcr	p15, 0, r0, c7, c5, 0		@ ICIALLU
	mcr	p15, 0, r0, c7, c5, 6		@ BPIALL
	mcr	p15, 0, r0, c8, c7, 0		@ TLBIALL
	dsb
	isb

	/* The handoff itself. Appended DTB: r2 is ignored once the decompressor
	 * finds the DTB magic at the end of the zImage blob. */
	mov	r0, #0
	mvn	r1, #0				@ ~0: device-tree boot
	mov	r2, #0
	bl	paint_fb_marker
	ldr	r3, =KERNEL_ADDR
	bx	r3

	/* Not reached. If it is, spin rather than execute garbage. */
1:	wfi
	b	1b

/*
 * Cortex-A9 set/way clean+invalidate of the whole data cache.
 * Straight out of the ARM ARM; iterates every set and way of every level
 * reported by CLIDR.
 */
v7_flush_dcache_all:
	push	{r4-r11, lr}
	dmb
	mrc	p15, 1, r0, c0, c0, 1		@ CLIDR
	ands	r3, r0, #0x7000000
	mov	r3, r3, lsr #23			@ levels of coherency * 2
	beq	.Lfinished
	mov	r10, #0
.Lloop_level:
	add	r2, r10, r10, lsr #1
	mov	r1, r0, lsr r2
	and	r1, r1, #7
	cmp	r1, #2
	blt	.Lskip_level
	mcr	p15, 2, r10, c0, c0, 0		@ CSSELR = this level
	isb
	mrc	p15, 1, r1, c0, c0, 0		@ CCSIDR
	and	r2, r1, #7			@ log2(line size) - 4
	add	r2, r2, #4
	movw	r4, #0x3ff
	ands	r4, r4, r1, lsr #3		@ max way
	clz	r5, r4				@ bit position of way field
	movw	r7, #0x7fff
	ands	r7, r7, r1, lsr #13		@ max set
.Lloop_way:
	mov	r9, r7
.Lloop_set:
	orr	r11, r10, r4, lsl r5
	orr	r11, r11, r9, lsl r2
	mcr	p15, 0, r11, c7, c14, 2		@ DCCISW
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
	mcr	p15, 2, r10, c0, c0, 0		@ CSSELR = 0
	dsb
	isb
	pop	{r4-r11, pc}

/*
 * Print PERIPHBASE. This is here because it is the one number this port could
 * not derive from the firmware device tree, and the loader is the cheapest
 * place to read it -- see docs/read-cbar.md.
 */
print_cbar:
	push	{r4, lr}
	adr	r0, .Lmsg_cbar
	bl	uart_puts
	mrc	p15, 4, r0, c15, c0, 0		@ CBAR
	/*
	 * Clear the low 13 reserved bits. Not "bic r0, r0, #0x1fff": 0x1fff is
	 * not an encodable ARM immediate (8 bits rotated by an even amount).
	 */
	lsr	r0, r0, #13
	lsl	r0, r0, #13
	bl	uart_put_hex
	mov	r0, #'\n'
	bl	uart_putc
	pop	{r4, pc}

uart_init_marker:
	bl	paint_fb_marker
	push	{lr}
	adr	r0, .Lmsg_hello
	bl	uart_puts
	pop	{pc}

/* r0 = character */
uart_putc:
	ldr	r1, =UART0_BASE
1:	ldr	r2, [r1, #UART_UTRSTAT]
	tst	r2, #UTRSTAT_TXE
	beq	1b
	str	r0, [r1, #UART_UTXH]
	bx	lr

/* r0 = NUL-terminated string */
uart_puts:
	push	{r4, lr}
	mov	r4, r0
1:	ldrb	r0, [r4], #1
	cmp	r0, #0
	beq	2f
	bl	uart_putc
	b	1b
2:	pop	{r4, pc}

/* r0 = value, printed as eight hex digits */
uart_put_hex:
	push	{r4, r5, lr}
	mov	r4, r0
	mov	r5, #28
1:	mov	r0, r4, lsr r5
	and	r0, r0, #0xf
	cmp	r0, #10
	addlt	r0, r0, #'0'
	addge	r0, r0, #('a' - 10)
	bl	uart_putc
	subs	r5, r5, #4
	bge	1b
	pop	{r4, r5, pc}

	.align	2
.Lmsg_hello:
	.asciz	"\r\n[a5-loader] alive, handing off to Linux\r\n"
	.align	2
.Lmsg_cbar:
	.asciz	"[a5-loader] CBAR/PERIPHBASE = 0x"
	.align	2

/*
 * Paint a band at the top of the simplefb region so bring-up without UART
 * still shows whether go <loader> actually executed.
 */
paint_fb_marker:
	push	{r4, r5, lr}
	ldr	r4, =FB_BASE
	ldr	r5, =FB_MARKER
	mov	r0, #256
1:	str	r5, [r4], #4
	subs	r0, r0, #1
	bne	1b
	pop	{r4, r5, pc}

	.align	2
