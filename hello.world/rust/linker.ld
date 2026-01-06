ENTRY(_start)

MEMORY {
  RAM (rwx) : ORIGIN = 0x40000000, LENGTH = 128M
}

SECTIONS {
  .text : {
    *(.text.boot)
    *(.text*)
    *(.rodata*)
  } > RAM

  .data : {
    *(.data*)
  } > RAM

  .bss (NOLOAD) : {
    __bss_start = .;
    *(.bss*)
    *(COMMON)
    __bss_end = .;
  } > RAM

  __stack_top = ORIGIN(RAM) + LENGTH(RAM) - 0x1000;
}
