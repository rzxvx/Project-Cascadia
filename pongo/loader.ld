/*
 * Linker script for the A5 handoff trampoline.
 *
 * The default linker script merges .text.start into .text and keeps a pile of
 * metadata sections, which makes "objcopy -O binary" either pad from address
 * zero (a 2.2 GB file when linking at 0x87000000) or emit nothing at all,
 * depending on which -j you guess.  Being explicit removes the guessing.
 *
 * TEXT_BASE is supplied by scripts/build-loader.sh via --defsym so the address
 * lives in exactly one place.  It is deliberately not called LOADADDR: that is
 * a builtin linker-script function and using the name as a plain symbol is a
 * syntax error.
 */

ENTRY(_start)

SECTIONS
{
	. = DEFINED(TEXT_BASE) ? TEXT_BASE : 0x87000000;

	.text : {
		KEEP(*(.text.start))
		*(.text .text.*)
		*(.rodata .rodata.*)
		. = ALIGN(4);
	}

	/*
	 * .ARM.attributes is deliberately kept.  It carries no bytes into the
	 * flat binary (objcopy takes only .text) but without it objdump does
	 * not know the target is ARMv7 and renders "cpsid if" as an undefined
	 * instruction, which is alarming and wrong.
	 */
	.ARM.attributes 0 : { *(.ARM.attributes) }

	/DISCARD/ : {
		*(.comment)
		*(.note*)
		*(.eh_frame*)
	}
}
