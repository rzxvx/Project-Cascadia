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
	/DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
