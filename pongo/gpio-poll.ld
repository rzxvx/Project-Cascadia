OUTPUT_FORMAT("elf32-littlearm", "elf32-littlearm", "elf32-littlearm")
OUTPUT_ARCH(arm)
ENTRY(_start)

SECTIONS
{
	. = 0x80000000;
	.text : {
		*(.text.start)
		*(.text*)
		*(.rodata*)
	}
	.data : {
		*(.data*)
	}
}
