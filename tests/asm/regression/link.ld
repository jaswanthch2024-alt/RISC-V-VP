ENTRY(_start)
SECTIONS {
  . = 0x80000000;
  .text.init : { *(.text.init) }
  .text      : { *(.text) }
  .data      : { *(.data) }
  . = 0x80001000;
  .tohost    : { KEEP(*(.tohost)) }
}
