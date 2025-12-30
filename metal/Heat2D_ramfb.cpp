// Heat2D_ramfb.cpp
// Bare-metal Heat2D demo for QEMU aarch64: -M virt

#include <stdint.h>
#include <stddef.h>

extern "C" void kmain(void);

// ------------------------------------------------------------
// Small helpers (no libc)
// ------------------------------------------------------------
static inline void dmb_sy() { asm volatile("dmb sy" ::: "memory"); }
static inline void dsb_sy() { asm volatile("dsb sy" ::: "memory"); }
static inline void isb()    { asm volatile("isb" ::: "memory"); }
static inline void wfe()    { asm volatile("wfe"); }

static inline uint16_t bswap16(uint16_t v) { return __builtin_bswap16(v); }
static inline uint32_t bswap32(uint32_t v) { return __builtin_bswap32(v); }
static inline uint64_t bswap64(uint64_t v) { return __builtin_bswap64(v); }

static inline uint16_t cpu_to_be16(uint16_t v) { return bswap16(v); }
static inline uint32_t cpu_to_be32(uint32_t v) { return bswap32(v); }
static inline uint64_t cpu_to_be64(uint64_t v) { return bswap64(v); }
static inline uint16_t be16_to_cpu(uint16_t v) { return bswap16(v); }
static inline uint32_t be32_to_cpu(uint32_t v) { return bswap32(v); }
static inline uint64_t be64_to_cpu(uint64_t v) { return bswap64(v); }

extern "C" void* memcpy(void* dst, const void* src, size_t n) {
    auto* d = (uint8_t*)dst;
    auto* s = (const uint8_t*)src;
    for (size_t i = 0; i < n; ++i) d[i] = s[i];
    return dst;
}
extern "C" void* memset(void* dst, int c, size_t n) {
    auto* d = (uint8_t*)dst;
    for (size_t i = 0; i < n; ++i) d[i] = (uint8_t)c;
    return dst;
}

// ------------------------------------------------------------
// PL011 UART @ 0x09000000 (QEMU virt)
// ------------------------------------------------------------
static constexpr uintptr_t UART_BASE = 0x09000000UL;

static inline void mmio_w32(uintptr_t addr, uint32_t v) {
    *(volatile uint32_t*)addr = v;
}
static inline uint32_t mmio_r32(uintptr_t addr) {
    return *(volatile uint32_t*)addr;
}

static void uart_init() {
    constexpr uintptr_t IBRD = UART_BASE + 0x24;
    constexpr uintptr_t FBRD = UART_BASE + 0x28;
    constexpr uintptr_t LCRH = UART_BASE + 0x2C;
    constexpr uintptr_t CR   = UART_BASE + 0x30;
    constexpr uintptr_t ICR  = UART_BASE + 0x44;

    mmio_w32(CR, 0x0);
    mmio_w32(ICR, 0x7FF);
    mmio_w32(IBRD, 13);
    mmio_w32(FBRD, 2);
    mmio_w32(LCRH, (3u << 5) | (1u << 4));
    mmio_w32(CR, (1u << 0) | (1u << 8) | (1u << 9));
}

static void uart_putc(char c) {
    constexpr uintptr_t DR = UART_BASE + 0x00;
    constexpr uintptr_t FR = UART_BASE + 0x18;
    constexpr uint32_t TXFF = (1u << 5);

    if (c == '\n') uart_putc('\r');
    while (mmio_r32(FR) & TXFF) { /* wait */ }
    mmio_w32(DR, (uint32_t)c);
}

static void uart_flush() {
    // Wait for UART TX to be idle (BUSY flag clear)
    constexpr uintptr_t FR = UART_BASE + 0x18;
    constexpr uint32_t BUSY = (1u << 3);
    while (mmio_r32(FR) & BUSY) { /* wait */ }
}

static void uart_puts(const char* s) {
    while (*s) uart_putc(*s++);
    uart_flush();
}

static void uart_puthex_u8(uint8_t v) {
    static const char* hexd = "0123456789abcdef";
    uart_putc(hexd[(v >> 4) & 0xF]);
    uart_putc(hexd[v & 0xF]);
}

static void uart_puthex_u32(uint32_t v) {
    static const char* hexd = "0123456789abcdef";
    for (int i = 7; i >= 0; --i) uart_putc(hexd[(v >> (i * 4)) & 0xF]);
}

static void uart_puthex_u64(uint64_t v) {
    static const char* hexd = "0123456789abcdef";
    for (int i = 15; i >= 0; --i) uart_putc(hexd[(v >> (i * 4)) & 0xF]);
}

[[noreturn]] static void panic(const char* msg) {
    uart_puts("\nPANIC: ");
    uart_puts(msg);
    uart_puts("\n");
    for (;;) wfe();
}

// ------------------------------------------------------------
// Generic timer (for delays)
// ------------------------------------------------------------
static inline uint64_t read_cntfrq_el0() {
    uint64_t v;
    asm volatile("mrs %0, cntfrq_el0" : "=r"(v));
    return v;
}
static inline uint64_t read_cntpct_el0() {
    uint64_t v;
    asm volatile("mrs %0, cntpct_el0" : "=r"(v));
    return v;
}
static void delay_ms(uint32_t ms) {
    const uint64_t freq = read_cntfrq_el0();
    const uint64_t start = read_cntpct_el0();
    const uint64_t ticks = (freq / 1000ULL) * (uint64_t)ms;
    while ((read_cntpct_el0() - start) < ticks) { /* spin */ }
}

// ------------------------------------------------------------
// fw_cfg interface
// virt machine: base=0x09020000
// - Selector register at +0x08 (16-bit BE write to select item)
// - Data register at +0x00 (byte-by-byte read/write after select)
// - DMA address at +0x10 (64-bit BE, triggers on write)
// ------------------------------------------------------------
static constexpr uintptr_t FW_CFG_BASE = 0x09020000UL;
static constexpr uintptr_t FW_CFG_DATA = FW_CFG_BASE + 0x00;
static constexpr uintptr_t FW_CFG_SEL  = FW_CFG_BASE + 0x08;
static constexpr uintptr_t FW_CFG_DMA  = FW_CFG_BASE + 0x10;

static constexpr uint16_t FW_CFG_FILE_DIR = 0x0019;

static constexpr uint32_t FW_DMA_ERROR  = 1u << 0;
static constexpr uint32_t FW_DMA_READ   = 1u << 1;
static constexpr uint32_t FW_DMA_SKIP   = 1u << 2;
static constexpr uint32_t FW_DMA_SELECT = 1u << 3;
static constexpr uint32_t FW_DMA_WRITE  = 1u << 4;

struct FWCfgDma {
    uint32_t control;
    uint32_t length;
    uint64_t address;
} __attribute__((packed, aligned(4096)));

static FWCfgDma g_dma __attribute__((aligned(4096)));

static void fwcfg_dma_wait() {
    for (volatile int i = 0; i < 100000; ++i) {
        dmb_sy();
        uint32_t ctrl = be32_to_cpu(g_dma.control);
        if (ctrl == 0) return;
        if (ctrl & FW_DMA_ERROR) {
            panic("fw_cfg DMA error");
        }
    }
    panic("fw_cfg DMA timeout");
}

static void fwcfg_dma_transfer(uint32_t control, void* buf, uint32_t len) {
    g_dma.control = cpu_to_be32(control);
    g_dma.length  = cpu_to_be32(len);
    g_dma.address = cpu_to_be64((uint64_t)(uintptr_t)buf);
    
    dsb_sy();
    
    uint64_t dma_addr = (uint64_t)(uintptr_t)&g_dma;
    
    // Write as single 64-bit big-endian value
    *(volatile uint64_t*)FW_CFG_DMA = cpu_to_be64(dma_addr);
    
    dsb_sy();
    fwcfg_dma_wait();
}

struct __attribute__((packed)) FWCfgFile {
    uint32_t size_be;
    uint16_t select_be;
    uint16_t reserved_be;
    char     name[56];
};

static size_t my_strlen(const char* s) {
    size_t len = 0;
    while (s[len]) ++len;
    return len;
}

static bool fwcfg_find_file(const char* fname, uint16_t* out_select, uint32_t* out_size) {
    uint32_t count_be = 0;
    uint32_t ctl = ((uint32_t)FW_CFG_FILE_DIR << 16) | FW_DMA_SELECT | FW_DMA_READ;
    fwcfg_dma_transfer(ctl, &count_be, 4);
    
    uint32_t count = be32_to_cpu(count_be);
    uart_puts("  File count: ");
    uart_puthex_u32(count);
    uart_puts("\n");
    
    FWCfgFile entry;
    size_t fname_len = my_strlen(fname);
    
    for (uint32_t i = 0; i < count; ++i) {
        fwcfg_dma_transfer(FW_DMA_READ, &entry, sizeof(entry));
        
        bool match = true;
        for (size_t j = 0; j < fname_len && j < 55; ++j) {
            if (entry.name[j] != fname[j]) { match = false; break; }
        }
        if (match && entry.name[fname_len] == '\0') {
            *out_select = be16_to_cpu(entry.select_be);
            *out_size = be32_to_cpu(entry.size_be);
            return true;
        }
    }
    return false;
}

// ------------------------------------------------------------
// ramfb config
// ------------------------------------------------------------
struct __attribute__((packed, aligned(16))) RAMFBCfg {
    uint64_t addr;
    uint32_t fourcc;
    uint32_t flags;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
};

static constexpr uint32_t DRM_FORMAT_XRGB8888 = 
    (uint32_t)'X' | ((uint32_t)'R' << 8) | ((uint32_t)'2' << 16) | ((uint32_t)'4' << 24);

static constexpr uint32_t FB_W = 800;
static constexpr uint32_t FB_H = 600;

alignas(4096) static uint32_t g_fb[FB_W * FB_H];

static RAMFBCfg g_ramfb_cfg __attribute__((aligned(4096)));

static void ramfb_init() {
    uart_puts("  Searching for etc/ramfb...\n");
    
    uint16_t sel = 0;
    uint32_t size = 0;
    if (!fwcfg_find_file("etc/ramfb", &sel, &size)) {
        panic("etc/ramfb not found - use: -device ramfb");
    }
    
    uart_puts("  Found: sel=0x");
    uart_puthex_u32(sel);
    uart_puts(" size=");
    uart_puthex_u32(size);
    uart_puts("\n");
    
    uart_puts("  FB addr: 0x");
    uart_puthex_u64((uint64_t)(uintptr_t)g_fb);
    uart_puts("\n");
    
    // Build config in global buffer (page-aligned for DMA)
    g_ramfb_cfg.addr   = cpu_to_be64((uint64_t)(uintptr_t)g_fb);
    g_ramfb_cfg.fourcc = cpu_to_be32(DRM_FORMAT_XRGB8888);
    g_ramfb_cfg.flags  = cpu_to_be32(0);
    g_ramfb_cfg.width  = cpu_to_be32(FB_W);
    g_ramfb_cfg.height = cpu_to_be32(FB_H);
    g_ramfb_cfg.stride = cpu_to_be32(FB_W * 4);
    
    uart_puts("  Config built, dumping bytes: ");
    uint8_t* p = (uint8_t*)&g_ramfb_cfg;
    for (int i = 0; i < 28; ++i) {
        uart_puthex_u8(p[i]);
        uart_putc(' ');
    }
    uart_puts("\n");
    
    uart_puts("  Sending config via DMA WRITE...\n");
    uart_flush();
    
    uint32_t ctl = ((uint32_t)sel << 16) | FW_DMA_SELECT | FW_DMA_WRITE;
    
    uart_puts("  DMA ctl=0x");
    uart_puthex_u32(ctl);
    uart_puts("\n");
    uart_flush();
    
    fwcfg_dma_transfer(ctl, &g_ramfb_cfg, 28);
    
    uart_puts("  ramfb configured!\n");
}

// ------------------------------------------------------------
// Heat2D simulation + palette
// ------------------------------------------------------------
static constexpr uint32_t SIM_W = 180;
static constexpr uint32_t SIM_H = 120;

static float field[SIM_W * SIM_H];
static float nextf[SIM_W * SIM_H];

struct ColorStop { float t; uint8_t r, g, b; };
struct Palette { const char* name; ColorStop s[4]; };

static Palette palettes[] = {
    {"Fiery", {{0.00f,20,24,82},{0.35f,30,120,200},{0.65f,255,180,60},{1.00f,255,255,245}}},
    {"Ocean", {{0.00f,10,40,70},{0.40f,40,140,170},{0.75f,80,210,190},{1.00f,230,255,255}}},
    {"Magenta", {{0.00f,55,10,60},{0.35f,140,30,140},{0.70f,240,120,200},{1.00f,255,240,255}}},
};
static uint32_t palette_idx = 0;

static inline float clamp01(float x) { return x < 0.f ? 0.f : (x > 1.f ? 1.f : x); }

static inline uint32_t pack_xrgb(uint8_t r, uint8_t g, uint8_t b) {
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

static inline uint8_t lerp_u8(uint8_t a, uint8_t b, float t) {
    float v = (float)a + ((float)b - (float)a) * t;
    return (uint8_t)(v < 0.f ? 0.f : (v > 255.f ? 255.f : v));
}

static uint32_t sample_palette(float t) {
    t = clamp01(t);
    const Palette& p = palettes[palette_idx];
    for (int i = 1; i < 4; ++i) {
        if (t <= p.s[i].t) {
            float span = p.s[i].t - p.s[i-1].t;
            float local = span > 0.f ? (t - p.s[i-1].t) / span : 0.f;
            return pack_xrgb(
                lerp_u8(p.s[i-1].r, p.s[i].r, local),
                lerp_u8(p.s[i-1].g, p.s[i].g, local),
                lerp_u8(p.s[i-1].b, p.s[i].b, local));
        }
    }
    return pack_xrgb(p.s[3].r, p.s[3].g, p.s[3].b);
}

static void heat_reset() {
    for (uint32_t i = 0; i < SIM_W * SIM_H; ++i) {
        field[i] = nextf[i] = 0.02f;
    }
}

static void stamp_heat(uint32_t cx, uint32_t cy, float value) {
    const int R = 6;
    for (int dy = -R; dy <= R; ++dy) {
        for (int dx = -R; dx <= R; ++dx) {
            int x = (int)cx + dx, y = (int)cy + dy;
            if (x > 0 && y > 0 && x < (int)SIM_W-1 && y < (int)SIM_H-1 && dx*dx+dy*dy <= R*R)
                nextf[y * SIM_W + x] = value;
        }
    }
}

static void heat_step(float dt) {
    const float alpha = 0.20f, cooling = 0.0008f, r = alpha * dt;
    for (uint32_t y = 1; y < SIM_H - 1; ++y) {
        for (uint32_t x = 1; x < SIM_W - 1; ++x) {
            uint32_t i = y * SIM_W + x;
            float lap = field[i-1] + field[i+1] + field[i-SIM_W] + field[i+SIM_W] - 4.f*field[i];
            nextf[i] = clamp01(field[i] + r*lap - cooling*field[i]);
        }
    }
    for (uint32_t x = 0; x < SIM_W; ++x) { nextf[x] = 0.f; nextf[(SIM_H-1)*SIM_W+x] = 0.f; }
    for (uint32_t y = 0; y < SIM_H; ++y) { nextf[y*SIM_W] = 0.f; nextf[y*SIM_W+SIM_W-1] = 0.f; }
    stamp_heat(SIM_W/2, SIM_H/2, 1.0f);
    for (uint32_t i = 0; i < SIM_W * SIM_H; ++i) field[i] = nextf[i];
}

static void heat_render() {
    volatile uint32_t* fb = (volatile uint32_t*)g_fb;
    for (uint32_t y = 0; y < FB_H; ++y) {
        uint32_t sy = (y * SIM_H) / FB_H;
        for (uint32_t x = 0; x < FB_W; ++x) {
            uint32_t sx = (x * SIM_W) / FB_W;
            fb[y * FB_W + x] = sample_palette(field[sy * SIM_W + sx]);
        }
    }
    dsb_sy();
}

// ------------------------------------------------------------
// C++ ABI stubs
// ------------------------------------------------------------
extern "C" void __cxa_pure_virtual() { panic("pure virtual"); }
extern "C" int  __cxa_atexit(void (*)(void*), void*, void*) { return 0; }
extern "C" void* __dso_handle = nullptr;

// ------------------------------------------------------------
// Entry
// ------------------------------------------------------------
extern "C" void kmain(void) {
    uart_init();
    uart_puts("\n=== Heat2D ramfb ===\n");
    
    ramfb_init();
    
    uart_puts("Drawing test pattern...\n");
    volatile uint32_t* fb = (volatile uint32_t*)g_fb;
    for (uint32_t y = 0; y < FB_H; ++y) {
        for (uint32_t x = 0; x < FB_W; ++x) {
            fb[y * FB_W + x] = pack_xrgb(
                (uint8_t)((x * 255) / FB_W),
                (uint8_t)((y * 255) / FB_H),
                128);
        }
    }
    dsb_sy();
    
    uart_puts("Test pattern drawn. Waiting...\n");
    delay_ms(2000);
    
    heat_reset();
    uart_puts("Starting simulation...\n");
    
    uint64_t last_switch = read_cntpct_el0();
    uint64_t freq = read_cntfrq_el0();
    
    for (;;) {
        heat_step(1.0f);
        heat_render();
        
        uint64_t now = read_cntpct_el0();
        if ((now - last_switch) > freq * 8) {
            palette_idx = (palette_idx + 1) % 3;
            last_switch = now;
        }
        delay_ms(16);
    }
}
