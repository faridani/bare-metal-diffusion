/* link.ld - link at RAM base for QEMU virt */
ENTRY(_start)

SECTIONS
{
  . = 0x40000000;

  .text : ALIGN(16) {
    KEEP(*(.text._start))
    *(.text*)
  }

  .rodata : ALIGN(16) {
    *(.rodata*)
  }

  .data : ALIGN(16) {
    *(.data*)
  }

  .bss (NOLOAD) : ALIGN(16) {
    __bss_start__ = .;
    *(.bss*)
    *(COMMON)
    __bss_end__ = .;
  }
}
