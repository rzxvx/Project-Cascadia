@ fill.S -- the smallest "kernel" for the PC-less ladder (P4 in
@ docs/research/p105-pcless-boot.md): fill the framebuffer iBoot left scanning
@ out at 0x9F6FC000 (768x1024, 32 bpp) with one colour and stop.  A green
@ screen says iBEC mounted System, took /boot/kernelcache and jumped to it.
@
@   arm-linux-gnueabihf-gcc -nostdlib -nostartfiles -static -Wl,-Ttext=0x80000000 \
@       -Wl,--build-id=none -o fill.elf tools/pcless/fill.S
@   arm-linux-gnueabihf-objcopy -O binary fill.elf fill.bin
	.arch	armv7-a
	.syntax	unified
	.arm
	.text
	.globl	_start
_start:
	cpsid	if
	ldr	r0, =0x9F6FC000
	ldr	r1, =(768 * 1024)
	ldr	r2, =0xFF00FF00
1:	str	r2, [r0], #4
	subs	r1, r1, #1
	bne	1b
	dsb	sy
2:	wfi
	b	2b
	.ltorg
