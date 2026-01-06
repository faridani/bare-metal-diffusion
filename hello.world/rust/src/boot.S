.arch armv8-a
.section .text.boot
.global _start
.extern rust_main
.extern __bss_start
.extern __bss_end
.extern __stack_top

_start:
    ldr x0, =__stack_top
    mov sp, x0

    // Zero .bss
    ldr x1, =__bss_start
    ldr x2, =__bss_end
1:
    cmp x1, x2
    b.ge 2f
    str xzr, [x1], #8
    b 1b
2:
    bl rust_main

hang:
    wfe
    b hang
