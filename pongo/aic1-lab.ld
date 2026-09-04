/* aic1-lab @ iBEC loadaddr 0x80000000 — must stay under 0x8000 for ACE EOF jump. */
ENTRY(_start)

SECTIONS
{
	. = 0x80000000;

	.vectors : {
		KEEP(*(.vectors))
	}

	.text : {
		KEEP(*(.text.start))
		*(.text .text.*)
		*(.rodata .rodata.*)
		*(.data .data.*)
		*(.bss .bss.*)
		*(COMMON)
		. = ALIGN(4);
	}

	.ARM.attributes 0 : { *(.ARM.attributes) }

	/DISCARD/ : {
		*(.comment)
		*(.note*)
		*(.eh_frame*)
	}
}
