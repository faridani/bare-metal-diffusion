.section ".text.boot"

.global _start

_start:
    // Check processor ID is 0 (we only run on one core for simplicity)
    mrs     x0, mpidr_el1
    and     x0, x0, #0xFF
    cbz     x0, master
    b       proc_hang

proc_hang: 
    wfe
    b       proc_hang

master:
    // Set stack to start below our code
    ldr     x0, =_start
    mov     sp, x0

    // Clear BSS (uninitialized variables)
    ldr     x0, =__bss_start
    ldr     x1, =__bss_end
    sub     x1, x1, x0
    cbz     x1, run_main
loop_bss:
    str     xzr, [x0], #8
    sub     x1, x1, #8
    cbnz    x1, loop_bss

run_main:
    bl      kernel_main   // Jump to C code

hang:
    b       hang