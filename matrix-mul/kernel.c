#include <stdint.h>

// --- UART DRIVER (To replace printf) ---
// Base Address for Pi 4 MMIO is 0xFE000000
#define MMIO_BASE       0xFE000000
#define UART0_DR        ((volatile unsigned int*)(MMIO_BASE + 0x201000))
#define UART0_FR        ((volatile unsigned int*)(MMIO_BASE + 0x201018))

void uart_putc(char c) {
    // Wait until UART is ready to transmit
    while (*UART0_FR & (1 << 5)) { }
    *UART0_DR = c;
}

void uart_puts(const char* str) {
    while (*str) {
        uart_putc(*str++);
    }
}

// Simple integer to string converter
void uart_print_int(long val) {
    if (val == 0) { uart_putc('0'); return; }
    
    char buffer[20];
    int i = 0;
    if (val < 0) { uart_putc('-'); val = -val; }
    
    while (val > 0) {
        buffer[i++] = (val % 10) + '0';
        val /= 10;
    }
    while (--i >= 0) uart_putc(buffer[i]);
}

// --- MATRIX MULTIPLICATION ---

// N=1000 is safer for testing. N=6500 is ~1GB but very slow on emulator.
#define N 1000 

// We define these as global static arrays. 
// In bare metal, these go into the BSS section, mapped directly to RAM.
// We do not use malloc().
double A[N*N];
double B[N*N];
double C[N*N];

// Simple Pseudo-Random Number Generator (Linear Congruential Generator)
unsigned long next = 1;
int my_rand(void) {
    next = next * 1103515245 + 12345;
    return (unsigned int)(next / 65536) % 32768;
}

void kernel_main(void) {
    uart_puts("\n\rBare Metal Matrix Multiplication (Pi 4 Emulator)\n\r");
    uart_puts("Initializing matrices...\n\r");

    // Initialize with random values
    for (int i = 0; i < N * N; i++) {
        A[i] = (double)(my_rand() % 100) / 10.0;
        B[i] = (double)(my_rand() % 100) / 10.0;
        C[i] = 0.0;
    }

    uart_puts("Starting calculation (Naive O(N^3))...\n\r");

    // Matrix Multiply: C = A * B
    for (int i = 0; i < N; i++) {
        for (int k = 0; k < N; k++) {
            double r = A[i * N + k];
            for (int j = 0; j < N; j++) {
                C[i * N + j] += r * B[k * N + j];
            }
        }
        // Progress indicator every 10 rows
        if (i % 50 == 0) {
            uart_puts("Row completed: ");
            uart_print_int(i);
            uart_puts(" out of ");
            uart_print_int(N);
            uart_puts(" rows \n\r");
        }
    }

    uart_puts("Calculation Done!\n\r");
    uart_puts("Value at C[0][0]: ");
    uart_print_int((long)C[0]); // Cast to int just for simple printing
    uart_puts("\n\r");
    
    while(1) { } // Halt
}