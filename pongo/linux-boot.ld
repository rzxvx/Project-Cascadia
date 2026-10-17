/*
 * Linker script for linux_boot @ iBEC loadaddr 0x80000000.
 * Loader and descending stack both stay below bundled zImage @0x80008000.
 */
ENTRY(_start)

SECTIONS
{
	. = 0x80000000;

	.text : {
		KEEP(*(.text.start))
		*(.text .text.*)
		*(.rodata .rodata.*)
		*(.data .data.*)
		*(.bss .bss.*)
		*(COMMON)
		. = ALIGN(4);
	}

	/* The bundle pads the entire first32KiB, so this NOLOAD RAM is owned.
	 * Stack grows down into8KiB; loader growth must never reach its bottom.
	 */
	__loader_end = .;
	__stack_bottom = 0x80006000;
	__stack_top = 0x80008000;
	ASSERT(__loader_end <= __stack_bottom,
	       "linux-boot loader overlaps reserved stack")
	.stack __stack_bottom (NOLOAD) : {
		. += __stack_top - __stack_bottom;
	}

	.ARM.attributes 0 : { *(.ARM.attributes) }

	/DISCARD/ : {
		*(.comment)
		*(.note*)
		*(.eh_frame*)
	}
}
