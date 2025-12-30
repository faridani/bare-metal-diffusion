// start.S - AArch64 bare-metal entry for QEMU virt
// Builds with: aarch64-linux-gnu-gcc -c -O2 -ffreestanding -nostdlib -nostartfiles start.S -o start.o

    .section .text._start, "ax"
    .align  2
    .global _start
    .type   _start, %function

_start:
    // Enable FP/SIMD so float code won't trap (important for Heat2D)
    mrs x0, CurrentEL
    lsr x0, x0, #2
    and x0, x0, #3
    cmp x0, #2
    b.ne 1f
    mrs x1, CPTR_EL2
    bic x1, x1, #(1 << 10)        // CPTR_EL2.TFP = 0 (don't trap FP at EL2)
    msr CPTR_EL2, x1
1:
    mrs x1, CPACR_EL1
    orr x1, x1, #(3 << 20)        // CPACR_EL1.FPEN = 0b11 (enable FP/SIMD)
    msr CPACR_EL1, x1
    isb

    // Set up stack
    ldr x0, =boot_stack_top
    mov sp, x0

    // Clear .bss
    ldr x0, =__bss_start__
    ldr x1, =__bss_end__
    mov x2, #0
2:
    cmp x0, x1
    b.ge 3f
    str x2, [x0], #8
    b 2b
3:
    bl main

// If main returns, park forever
4:
    wfi
    b 4b

    .section .bss.stack, "aw", %nobits
    .align 12                 // 4096-byte aligned
boot_stack:
    .skip 65536               // 64 KiB
boot_stack_top:
