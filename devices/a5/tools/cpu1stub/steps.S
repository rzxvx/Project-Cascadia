/*
 * steps.S -- what Linux's secondary_startup does to a Cortex-A9 core, one
 * step at a time, for CPU1 of the A5 to run from the first page of DRAM
 * (tools/cpu1probe run).  After each step: beacon at +0xff0 = 0x300 | step,
 * ACTLR at +0xff4, SCTLR at +0xff8; it stops after the step number found at
 * +0xfe0.  Steps 5-6 take their memory attributes from the flags at +0xfdc:
 * bit 0 table walks cacheable (TTB_FLAGS_SMP), bit 1 DRAM cacheable (write-back
 * write-allocate), bit 2 shareable, bit 3 D-cache on, bit 4 skip step 3 (no
 * SMP), bit 5 beacon through the identity map rather than the 0x40000000 alias.
 * +0xfd8 counts starts from reset, +0xfec wakes from the final WFI.  Any exception lands in trap: 0xee and lr.  Every wait is a WFI:
 * PMGR+0x1210 powers a core off only once it sits in WFI, so that the next
 * run can start it again.
 */
	.arch	armv7-a
	.arm
	.text
	.globl	_start
_start:
	b	entry
	b	v1
	b	v2
	b	v3
	b	v4
	b	v5
	b	v6
	b	v7
v1:	mov	r9, #1
	b	trap
v2:	mov	r9, #2
	b	trap
v3:	mov	r9, #3
	b	trap
v4:	mov	r9, #4
	b	trap
v5:	mov	r9, #5
	b	trap
v6:	mov	r9, #6
	b	trap
v7:	mov	r9, #7
	b	trap

	@ exception n: +0xff0 = 0xee0 | n, +0xff4 lr, +0xff8 DFSR, +0xffc IFSR,
	@ +0xfe4 DFAR, +0xfe8 IFAR -- through the uncached alias once the MMU is
	@ on -- then WFI, where PMGR+0x1210 can power the core off
trap:
	adr	r4, _start
	mrc	p15, 0, r0, c1, c0, 0
	tst	r0, #1
	movne	r4, #0x40000000
	orr	r1, r9, #0xee0
	str	r1, [r4, #0xff0]
	str	lr, [r4, #0xff4]
	mrc	p15, 0, r0, c5, c0, 0
	str	r0, [r4, #0xff8]
	mrc	p15, 0, r0, c5, c0, 1
	str	r0, [r4, #0xffc]
	mrc	p15, 0, r0, c6, c0, 0
	str	r0, [r4, #0xfe4]
	mrc	p15, 0, r0, c6, c0, 2
	str	r0, [r4, #0xfe8]
	dsb
1:	wfi
	b	1b

	@ The core comes out of reset at physical 0, an alias of the first
	@ page of DRAM: move over to 0x80000000, where the identity map is.
entry:
	ldr	pc, 1f
1:	.word	0x80000000 + entry_hi - _start
entry_hi:
	adr	r4, _start
	ldr	r0, [r4, #0xfd8]		@ one more start from reset
	add	r0, r0, #1
	str	r0, [r4, #0xfd8]
	mcr	p15, 0, r4, c12, c0, 0		@ VBAR: this page's vectors
	ldr	r5, [r4, #0xfe0]		@ last step to take
	ldr	r10, [r4, #0xfdc]		@ flags
	add	r6, r4, #0xff0			@ beacon

	@ 1: invalidate L1 -- D by set/way (32 KB, 4 ways, 32-byte lines), then I
	mov	r0, #0
2:	mov	r1, #0
3:	mov	r2, r0, lsl #30
	orr	r2, r2, r1, lsl #5
	mcr	p15, 0, r2, c7, c6, 2		@ DCISW
	add	r1, r1, #1
	cmp	r1, #256
	blt	3b
	add	r0, r0, #1
	cmp	r0, #4
	blt	2b
	mov	r0, #0
	mcr	p15, 0, r0, c7, c5, 0		@ ICIALLU
	dsb
	isb
	mov	r7, #1
	bl	mark

	@ 2: ACTLR.FW -- cache/TLB maintenance broadcast
	mrc	p15, 0, r0, c1, c0, 1
	orr	r0, r0, #(1 << 0)
	mcr	p15, 0, r0, c1, c0, 1
	isb
	mov	r7, #2
	bl	mark

	@ 3: ACTLR.SMP -- the core joins coherency
	mrc	p15, 0, r0, c1, c0, 1
	tst	r10, #0x10
	orreq	r0, r0, #(1 << 6)
	mcr	p15, 0, r0, c1, c0, 1
	isb
	mov	r7, #3
	bl	mark

	@ 4: SCTLR.I -- instruction cache on
	mrc	p15, 0, r0, c1, c0, 0
	orr	r0, r0, #(1 << 12)
	mcr	p15, 0, r0, c1, c0, 0
	isb
	mov	r7, #4
	bl	mark

	@ 5: page table at base + 0x4000 (unused by Linux: below PHYS_OFFSET):
	@ DRAM 0x80000000-0x9fffffff identity-mapped in 1 MB sections, Linux's
	@ attributes -- normal, write-back write-allocate (TEX 001 C B),
	@ shareable, full access; TTBR0 with TTB_FLAGS_SMP, domain 0 client
	add	r8, r4, #0x4000
	mov	r0, #0				@ the whole 16 KB table cleared first
	mov	r1, #0
13:	str	r1, [r8, r0, lsl #2]
	add	r0, r0, #1
	cmp	r0, #0x1000
	blt	13b
	ldr	r2, =0x00001c02			@ section, AP full, TEX 001: normal non-cacheable
	tst	r10, #2
	orrne	r2, r2, #0xc			@ C B: write-back write-allocate
	tst	r10, #4
	orrne	r2, r2, #0x10000		@ S
	mov	r0, #0x800			@ first section index, 0x80000000 >> 20
10:	orr	r3, r2, r0, lsl #20
	str	r3, [r8, r0, lsl #2]
	add	r0, r0, #1
	cmp	r0, #0xa00
	blt	10b
	@ and the first MB again at 0x40000000, strongly-ordered: the beacon
	@ written there reaches memory whatever the caches do
	ldr	r3, =0x80000c02
	add	r0, r8, #0x1000			@ entry 0x400, VA 0x40000000
	str	r3, [r0]
	dsb
	mov	r0, #0
	mcr	p15, 0, r0, c2, c0, 2		@ TTBCR = 0
	mov	r0, r8
	tst	r10, #1
	orrne	r0, r0, #0x6a			@ TTB_FLAGS_SMP
	mcr	p15, 0, r0, c2, c0, 0		@ TTBR0
	mov	r0, #1
	mcr	p15, 0, r0, c3, c0, 0		@ DACR: domain 0 client
	mov	r0, #0
	mcr	p15, 0, r0, c8, c7, 0		@ TLBIALL
	mcr	p15, 0, r0, c7, c5, 6		@ BPIALL
	dsb
	isb
	mov	r7, #5
	bl	mark

	@ 6: MMU, D-cache, I-cache and branch prediction on
	mrc	p15, 0, r0, c1, c0, 0
	ldr	r1, =0x1801			@ Z I M
	tst	r10, #8
	orrne	r1, r1, #0x4			@ C
	orr	r0, r0, r1
	mcr	p15, 0, r0, c1, c0, 0
	isb
	tst	r10, #0x20
	moveq	r6, #0x40000000			@ beacon through the uncached alias
	addeq	r6, r6, #0xff0
	mov	r7, #6
	bl	mark

	@ 7: 64 KB of cacheable writes and reads at base + 0x10000
	add	r8, r4, #0x10000
	mov	r0, #0
11:	str	r0, [r8, r0, lsl #2]
	add	r0, r0, #1
	cmp	r0, #0x4000
	blt	11b
	mov	r0, #0
	mov	r1, #0
12:	ldr	r2, [r8, r0, lsl #2]
	add	r1, r1, r2
	add	r0, r0, #1
	cmp	r0, #0x4000
	blt	12b
	str	r1, [r6, #12]			@ sum, 0x7ffe000 if all read back
	mov	r7, #7
	bl	mark
	b	idle

mark:
	orr	r0, r7, #0x300
	str	r0, [r6]
	mrc	p15, 0, r0, c1, c0, 1
	str	r0, [r6, #4]
	mrc	p15, 0, r0, c1, c0, 0
	str	r0, [r6, #8]
	dsb
	mcr	p15, 0, r6, c7, c10, 1		@ DCCMVAC: the beacon line out to memory
	dsb
	cmp	r7, r5
	bxlt	lr

	@ the end: WFI, and each wake from it counted at +0xfec
idle:
	wfi
	ldr	r0, [r6, #-4]
	add	r0, r0, #1
	str	r0, [r6, #-4]
	dsb
	b	idle
	.ltorg
