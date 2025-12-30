// Heat2D_virt.cpp - Bare-metal AArch64 C++ for QEMU -M virt
// - No Circle, no libc, no syscalls
// - Serial output via PL011 UART @ 0x09000000 (QEMU virt default)
// - Links at 0x40000000 and runs as a flat binary loaded by QEMU -kernel

#include <stdint.h>
#include <stddef.h>

// -----------------------------
// Minimal MMIO helpers
// -----------------------------
static inline void mmio_write(uintptr_t addr, uint32_t val) {
    *(volatile uint32_t*)addr = val;
}
static inline uint32_t mmio_read(uintptr_t addr) {
    return *(volatile uint32_t*)addr;
}

// -----------------------------
// PL011 UART (QEMU virt)
// Base address: 0x09000000
// -----------------------------
static constexpr uintptr_t UART0_BASE = 0x09000000UL;

static constexpr uintptr_t UARTDR   = UART0_BASE + 0x00;  // Data register
static constexpr uintptr_t UARTFR   = UART0_BASE + 0x18;  // Flag register
static constexpr uintptr_t UARTIBRD = UART0_BASE + 0x24;  // Integer baud rate
static constexpr uintptr_t UARTFBRD = UART0_BASE + 0x28;  // Fractional baud rate
static constexpr uintptr_t UARTLCRH = UART0_BASE + 0x2C;  // Line control
static constexpr uintptr_t UARTCR   = UART0_BASE + 0x30;  // Control
static constexpr uintptr_t UARTICR  = UART0_BASE + 0x44;  // Interrupt clear

static inline void uart_putc(char c) {
    // Wait until TX FIFO not full (FR bit 5 = TXFF)
    while (mmio_read(UARTFR) & (1u << 5)) {}
    mmio_write(UARTDR, (uint32_t)c);
}

static inline void uart_puts(const char* s) {
    while (*s) {
        char c = *s++;
        if (c == '\n') uart_putc('\r');
        uart_putc(c);
    }
}

static void uart_init() {
    // Disable UART
    mmio_write(UARTCR, 0x00000000);

    // Clear interrupts
    mmio_write(UARTICR, 0x7FF);

    // Baud rate (optional in QEMU; leaving sane defaults helps some setups)
    // Assuming UARTCLK=24MHz:
    // 115200 => IBRD=13, FBRD=1 (approx)
    mmio_write(UARTIBRD, 13);
    mmio_write(UARTFBRD, 1);

    // 8N1, FIFO enabled
    mmio_write(UARTLCRH, (1u << 4) | (3u << 5));

    // Enable UART, TX, RX
    mmio_write(UARTCR, (1u << 0) | (1u << 8) | (1u << 9));
}

// -----------------------------
// Tiny utilities (no libc)
// -----------------------------
static void u32_to_dec(char* out, uint32_t v) {
    // Writes decimal digits into out, null-terminated.
    // out must have >= 11 bytes.
    char buf[11];
    int i = 0;
    if (v == 0) {
        out[0] = '0';
        out[1] = 0;
        return;
    }
    while (v > 0 && i < 10) {
        buf[i++] = char('0' + (v % 10));
        v /= 10;
    }
    int j = 0;
    while (i > 0) out[j++] = buf[--i];
    out[j] = 0;
}

static void busy_wait(uint64_t iters) {
    // Simple delay loop
    for (volatile uint64_t i = 0; i < iters; ++i) {
        __asm__ volatile("nop");
    }
}

// -----------------------------
// Heat2D simulation (serial-only visualization)
// -----------------------------
static constexpr unsigned kW = 80;
static constexpr unsigned kH = 50;
static constexpr float    kAlpha = 0.18f;
static constexpr float    kCooling = 0.0009f;

static float field[kW * kH];
static float nextf[kW * kH];

static inline float clamp01(float x) {
    return (x < 0.f) ? 0.f : (x > 1.f) ? 1.f : x;
}

static void reset_field() {
    for (unsigned i = 0; i < kW * kH; ++i) {
        field[i] = 0.02f;
        nextf[i] = 0.02f;
    }
}

static void stamp_heat(unsigned cx, unsigned cy, float value) {
    const int r = 4;
    for (int dy = -r; dy <= r; ++dy) {
        for (int dx = -r; dx <= r; ++dx) {
            int x = int(cx) + dx;
            int y = int(cy) + dy;
            if (x < 1 || y < 1 || x >= int(kW - 1) || y >= int(kH - 1)) continue;
            if (dx*dx + dy*dy <= r*r) {
                nextf[unsigned(y) * kW + unsigned(x)] = value;
            }
        }
    }
}

static void step_sim(float dt) {
    const float r = kAlpha * dt;

    for (unsigned y = 1; y < kH - 1; ++y) {
        for (unsigned x = 1; x < kW - 1; ++x) {
            unsigned idx = y * kW + x;
            float t = field[idx];
            float lap = field[idx - 1] + field[idx + 1] + field[idx - kW] + field[idx + kW] - 4.f * t;
            float n = t + r * lap - kCooling * t;
            nextf[idx] = clamp01(n);
        }
    }

    // borders
    for (unsigned x = 0; x < kW; ++x) {
        nextf[x] = 0.f;
        nextf[(kH - 1) * kW + x] = 0.f;
    }
    for (unsigned y = 0; y < kH; ++y) {
        nextf[y * kW] = 0.f;
        nextf[y * kW + (kW - 1)] = 0.f;
    }

    // heat source
    stamp_heat(kW / 2, kH / 2, 1.0f);

    // swap
    for (unsigned i = 0; i < kW * kH; ++i) field[i] = nextf[i];
}

static char sample_char(float t) {
    // ASCII ramp
    // 0 -> ' ', 1 -> '@'
    static const char* ramp = " .:-=+*#%@";
    int idx = (int)(t * 9.0f + 0.5f);
    if (idx < 0) idx = 0;
    if (idx > 9) idx = 9;
    return ramp[idx];
}

static void render_ascii(unsigned frame) {
    uart_puts("\n[Heat2D] frame ");
    char num[16];
    u32_to_dec(num, frame);
    uart_puts(num);
    uart_puts("\n");

    // downsample a bit to keep output readable
    const unsigned sx = 2;
    const unsigned sy = 2;
    for (unsigned y = 0; y < kH; y += sy) {
        for (unsigned x = 0; x < kW; x += sx) {
            float t = field[y * kW + x];
            uart_putc(sample_char(t));
        }
        uart_puts("\n");
    }
}

// -----------------------------
// AArch64 entry / minimal runtime
// -----------------------------
extern "C" void kmain();

__attribute__((aligned(16)))
static uint8_t boot_stack[16 * 1024];

extern "C" void _start();
__attribute__((naked))
extern "C" void _start() {
    __asm__ volatile(
        // Set up stack
        "ldr x0, =boot_stack        \n"
        "add x0, x0, %[stksz]       \n"
        "mov sp, x0                 \n"

        // Optional: clear .bss would go here if we had a proper linker script.
        // We keep everything in .data/.bss simple + rely on static zero init.

        "bl kmain                   \n"
        "b .                        \n"
        :
        : [stksz] "i"(sizeof(boot_stack))
        : "x0"
    );
}

extern "C" void kmain() {
    uart_init();
    uart_puts("\n\n=== Heat2D bare-metal on QEMU virt (AArch64) ===\n");
    uart_puts("UART: PL011 @ 0x09000000\n");
    uart_puts("If you see this, boot + serial are working.\n");

    reset_field();

    uint32_t frame = 0;
    while (true) {
        ++frame;
        step_sim(1.0f);

        // print every N frames to avoid spamming too hard
        if ((frame % 20) == 0) {
            render_ascii(frame);
        }

        // crude delay
        busy_wait(3'000'000);
    }
}

// Provide a dummy for the C++ runtime if the toolchain expects it
extern "C" void __cxa_pure_virtual() {
    uart_puts("pure virtual call\n");
    while (true) {}
}
