ENTRY(_start)
PHDRS { image PT_LOAD FILEHDR PHDRS FLAGS(5); }
SECTIONS {
    . = 0x10000 + SIZEOF_HEADERS;
    .text : { *(.text*) } :image
    .rodata : { *(.rodata*) } :image
    /DISCARD/ : { *(.ARM.attributes) *(.note*) *(.comment*) }
}
