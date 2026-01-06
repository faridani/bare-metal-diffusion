ENTRY(_start)

MEMORY {
  RAM (rwx) : ORIGIN = 0x40000000, LENGTH = 128M   /* QEMU virt RAM base :contentReference[oaicite:2]{index=2} */
}

SECTIONS
{
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

  /* A simple stack near the end of our RAM region */
  __stack_top = ORIGIN(RAM) + LENGTH(RAM) - 0x1000;
}
