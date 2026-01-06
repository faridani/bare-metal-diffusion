#pragma once
#include <stdint.h>

#define UART0_BASE 0x09000000UL  // QEMU virt PL011 UART :contentReference[oaicite:1]{index=1}

/* PL011 registers (offsets) */
#define UART_DR   0x00
#define UART_FR   0x18
#define UART_IBRD 0x24
#define UART_FBRD 0x28
#define UART_LCRH 0x2C
#define UART_CR   0x30
#define UART_IMSC 0x38
#define UART_ICR  0x44

/* Flag Register bits */
#define FR_TXFF (1u << 5)   // Transmit FIFO full

static inline void mmio_write(uint64_t reg, uint32_t val) {
    *(volatile uint32_t *)reg = val;
}
static inline uint32_t mmio_read(uint64_t reg) {
    return *(volatile uint32_t *)reg;
}

static inline void uart_init(void) {
    /* Disable UART */
    mmio_write(UART0_BASE + UART_CR, 0);

    /* Clear pending interrupts */
    mmio_write(UART0_BASE + UART_ICR, 0x7FF);

    /*
      Set baud rate divisors.
      QEMU's virt UART clock is typically 24MHz; 115200 baud -> IBRD=13, FBRD=1 (common setup).
      If your host/QEMU build differs, printing often still works even without perfect baud.
    */
    mmio_write(UART0_BASE + UART_IBRD, 13);
    mmio_write(UART0_BASE + UART_FBRD, 1);

    /* 8N1, enable FIFOs */
    mmio_write(UART0_BASE + UART_LCRH, (1u << 4) | (3u << 5));

    /* Mask interrupts (we're polling) */
    mmio_write(UART0_BASE + UART_IMSC, 0);

    /* Enable UART, TX, RX */
    mmio_write(UART0_BASE + UART_CR, (1u << 0) | (1u << 8) | (1u << 9));
}

static inline void uart_putc(char c) {
    /* Wait until TX FIFO has space */
    while (mmio_read(UART0_BASE + UART_FR) & FR_TXFF) { }
    mmio_write(UART0_BASE + UART_DR, (uint32_t)c);
}

static inline void uart_puts(const char *s) {
    while (*s) {
        if (*s == '\n') uart_putc('\r');
        uart_putc(*s++);
    }
}
