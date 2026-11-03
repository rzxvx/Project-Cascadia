ENTRY(mt_stub)
SECTIONS {
  . = 0x80083108;
  .text : {
    *(.text.stub)
    *(.text .text.*)
    *(.rodata .rodata.*)
    *(.data .data.*)
  }
}
