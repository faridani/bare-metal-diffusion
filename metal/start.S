// start.S - AArch64 bare-metal entry for QEMU virt
// - sets up stack
// - enables FP/SIMD (needed for float math in Heat2D)
// - clears .bss
// - calls kmain()

    .section .text._start, "ax"
    .global _start
    .type _start, %function

_start:
    // Set up stack
    ldr x0, =boot_stack_top
    mov sp, x0

    // ---- Enable FP/SIMD ----
    // If FP/SIMD is trapped, the first float instruction will exception/hang.
    mrs x0, CurrentEL
    lsr x0, x0, #2              // x0 = current EL (1,2,...)

    cmp x0, #2
    b.ne 1f
    // EL2: clear CPTR_EL2.TFP so FP/SIMD won't trap
    mrs x1, cptr_el2
    bic x1, x1, #(1 << 10)      // TFP = 0
    msr cptr_el2, x1
    isb
1:
    // Enable FP at EL0/EL1
    mrs x1, cpacr_el1
    orr x1, x1, #(3 << 20)      // FPEN = 0b11
    msr cpacr_el1, x1
    isb

    // ---- Clear .bss ----
    ldr x0, =__bss_start__
    ldr x1, =__bss_end__
    mov x2, xzr
2:
    cmp x0, x1
    b.hs 3f
    str x2, [x0], #8
    b 2b
3:
    bl kmain

hang:
    wfe
    b hang

    .size _start, . - _start

    // ---- Simple boot stack in BSS ----
    .section .bss.stack, "aw", %nobits
    .align 12
boot_stack:
    .skip 16384
boot_stack_top:
