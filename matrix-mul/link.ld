SECTIONS
{
    . = 0x80000; /* Standard load address for Pi 64-bit kernels */

    .text : { KEEP(*(.text.boot)) *(.text) }
    .rodata : { *(.rodata) }
    .data : { *(.data) }
    
    . = ALIGN(16);
    __bss_start = .;
    .bss : { *(.bss*) } 
    __bss_end = .;
}