.global _start
.extern main

/* Linker provides these */
.extern __bss_start
.extern __bss_end

_start:
    /* Set up a simple stack (top defined in linker script) */
    ldr x0, =__stack_top
    mov sp, x0

    /* Zero .bss */
    ldr x1, =__bss_start
    ldr x2, =__bss_end
1:
    cmp x1, x2
    b.ge 2f
    str xzr, [x1], #8
    b 1b
2:
    bl main

/* If main returns, just spin */
hang:
    wfe
    b hang
