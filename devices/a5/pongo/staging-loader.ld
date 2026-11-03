/*
 * Linker script for staging-loader.S at P105 iBEC loadaddr 0x80000000.
 */
ENTRY(_start)

SECTIONS
{
	. = 0x80000000;

	.text : {
		KEEP(*(.text.start))
		*(.text .text.*)
		*(.rodata .rodata.*)
		. = ALIGN(4);
	}

	.ARM.attributes 0 : { *(.ARM.attributes) }

	/DISCARD/ : {
		*(.comment)
		*(.note*)
		*(.eh_frame*)
	}
}
