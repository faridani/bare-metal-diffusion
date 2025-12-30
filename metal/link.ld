/* link.ld - place kernel at RAM base for QEMU virt (0x40000000) */
ENTRY(_start)

PHDRS
{
  text PT_LOAD FLAGS(5); /* R + X */
  data PT_LOAD FLAGS(6); /* R + W */
}

SECTIONS
{
  . = 0x40000000;

  .text : ALIGN(16)
  {
    *(.text._start)
    *(.text*)
  } :text

  .rodata : ALIGN(16)
  {
    *(.rodata*)
  } :text

  .data : ALIGN(16)
  {
    *(.data*)
  } :data

  .bss (NOLOAD) : ALIGN(16)
  {
    __bss_start__ = .;
    *(.bss*)
    *(.bss.stack*)
    *(COMMON)
    __bss_end__ = .;
  } :data

  __end__ = .;
}
