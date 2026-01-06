#include "uart_pl011.h"

int main(void) {
    uart_init();
    uart_puts("Hello, world (bare metal AArch64)!\n");

    for (;;) {
        /* idle forever */
        __asm__ volatile("wfe");
    }
}
