// Heat2D_ramfb.cpp - Bare-metal Heat2D for QEMU AArch64 "virt" using ramfb + fw_cfg DMA
//
// Why this works:
//  - ramfb must be configured by writing a *packed 28-byte* RAMFBCfg to fw_cfg file "etc/ramfb" via DMA
//  - fw_cfg DMA is big-endian, so control/len/addr and the RAMFBCfg fields must be byte-swapped
//  - After a successful write, the framebuffer appears and QEMU display becomes "active"
//
// References:
//  - OSDev ramfb notes the struct and DMA usage 
//  - QEMU fw_cfg DMA is big-endian 

#include <stdint.h>
#include <stddef.h>

extern "C" char __bss_end__[];

/* ------------------------- tiny libc ------------------------- */
extern "C" void* memset(void* dst, int v, size_t n) {
    uint8_t* p = (uint8_t*)dst;
    while (n--) *p++ = (uint8_t)v;
    return dst;
}

extern "C" void* memcpy(void* dst, const void* src, size_t n) {
    uint8_t* d = (uint8_t*)dst;
    const uint8_t* s = (const uint8_t*)src;
    while (n--) *d++ = *s++;
    return dst;
}

static size_t c_strlen(const char* s) {
    size_t n = 0;
    while (s && s[n]) n++;
    return n;
}

/* ------------------------- MMIO helpers ------------------------- */
static inline uint16_t bswap16(uint16_t x) { return __builtin_bswap16(x); }
static inline uint32_t bswap32(uint32_t x) { return __builtin_bswap32(x); }
static inline uint64_t bswap64(uint64_t x) { return __builtin_bswap64(x); }

static inline void mmio_write32(uintptr_t addr, uint32_t v) {
    *(volatile uint32_t*)addr = v;
}
static inline uint32_t mmio_read32(uintptr_t addr) {
    return *(volatile uint32_t*)addr;
}

static inline void mmio_write32be(uintptr_t addr, uint32_t v) {
    *(volatile uint32_t*)addr = bswap32(v);
}

static inline void dsb_sy() { asm volatile("dsb sy" ::: "memory"); }
static inline void isb()    { asm volatile("isb" ::: "memory"); }

/* ------------------------- PL011 UART (virt) ------------------------- */
static constexpr uintptr_t UART_BASE = 0x09000000UL;

static inline void uart_putc(char c) {
    // PL011 FR TXFF (bit 5): 1 = TX FIFO full
    while (mmio_read32(UART_BASE + 0x18) & (1u << 5)) { }
    mmio_write32(UART_BASE + 0x00, (uint32_t)c);
}

static void uart_puts(const char* s) {
    if (!s) return;
    while (*s) {
        char c = *s++;
        if (c == '\n') uart_putc('\r');
        uart_putc(c);
    }
}

static void uart_hex64(uint64_t v) {
    static const char* hex = "0123456789abcdef";
    uart_puts("0x");
    for (int i = 60; i >= 0; i -= 4) {
        uart_putc(hex[(v >> i) & 0xF]);
    }
}

static void uart_hex32(uint32_t v) {
    uart_hex64((uint64_t)v);
}

/* ------------------------- Generic timer delay ------------------------- */
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
    uint64_t freq = read_cntfrq_el0();
    uint64_t start = read_cntpct_el0();
    uint64_t ticks = (freq / 1000ULL) * (uint64_t)ms;
    while ((read_cntpct_el0() - start) < ticks) { }
}

/* ------------------------- fw_cfg + DMA (virt) ------------------------- */
static constexpr uintptr_t FW_CFG_BASE     = 0x09020000UL;
static constexpr uintptr_t FW_CFG_DMA_ADDR = FW_CFG_BASE + 0x10;

static constexpr uint16_t FW_CFG_FILE_DIR = 0x0019;

static constexpr uint32_t DMA_CTL_ERROR  = 0x01;
static constexpr uint32_t DMA_CTL_READ   = 0x02;
static constexpr uint32_t DMA_CTL_SKIP   = 0x04;
static constexpr uint32_t DMA_CTL_SELECT = 0x08;
static constexpr uint32_t DMA_CTL_WRITE  = 0x10;

struct __attribute__((packed)) FWCfgDmaAccess {
    uint32_t control_be;
    uint32_t length_be;
    uint64_t address_be;
};
static_assert(sizeof(FWCfgDmaAccess) == 16, "FWCfgDmaAccess must be 16 bytes");

struct __attribute__((packed)) FWCfgFile {
    uint32_t size_be;
    uint16_t select_be;
    uint16_t reserved_be;
    char     name[56];
};
static_assert(sizeof(FWCfgFile) == 64, "FWCfgFile must be 64 bytes");

// IMPORTANT: must be packed => 28 bytes, not 32.
struct __attribute__((packed)) RAMFBCfg {
    uint64_t addr_be;
    uint32_t fourcc_be;
    uint32_t flags_be;
    uint32_t width_be;
    uint32_t height_be;
    uint32_t stride_be;
};
static_assert(sizeof(RAMFBCfg) == 28, "RAMFBCfg must be 28 bytes");

static volatile FWCfgDmaAccess g_dma __attribute__((aligned(16)));

static void fw_cfg_dma_transfer(uint32_t control, void* buf, uint32_t len) {
    g_dma.control_be = bswap32(control);
    g_dma.length_be  = bswap32(len);
    g_dma.address_be = bswap64((uint64_t)(uintptr_t)buf);

    dsb_sy();

    uint64_t desc_addr = (uint64_t)(uintptr_t)&g_dma;

    // Per fw_cfg DMA spec: write high 32 then low 32 (low triggers)
    mmio_write32be(FW_CFG_DMA_ADDR + 0, (uint32_t)(desc_addr >> 32));
    mmio_write32be(FW_CFG_DMA_ADDR + 4, (uint32_t)(desc_addr & 0xFFFFFFFFu));

    // Poll completion; QEMU clears control to 0, sets ERROR bit on failure
    for (;;) {
        uint32_t c = bswap32(g_dma.control_be);
        if (c == 0) break;
        if (c & DMA_CTL_ERROR) {
            uart_puts("fw_cfg DMA ERROR, control=");
            uart_hex32(c);
            uart_puts("\nHALTING.\n");
            while (1) asm volatile("wfi");
        }
    }
}

static bool fw_cfg_find_file(const char* target, uint16_t& out_sel, uint32_t& out_size) {
    uart_puts("fw_cfg: reading FILE_DIR...\n");

    uint32_t n_be = 0;
    fw_cfg_dma_transfer(((uint32_t)FW_CFG_FILE_DIR << 16) | DMA_CTL_SELECT | DMA_CTL_READ,
                        &n_be, sizeof(n_be));
    uint32_t n = bswap32(n_be);

    uart_puts("fw_cfg: FILE_DIR entries = ");
    uart_hex32(n);
    uart_puts("\n");

    FWCfgFile ent;
    for (uint32_t i = 0; i < n; i++) {
        fw_cfg_dma_transfer(DMA_CTL_READ, &ent, sizeof(ent));

        uint32_t size = bswap32(ent.size_be);
        uint16_t sel  = bswap16(ent.select_be);

        // Print entry name + sel (useful debug, matches your logs)
        uart_puts("fw_cfg: entry name = ");
        // name is NUL-terminated for these entries in practice
        uart_puts(ent.name);
        uart_puts(" sel=");
        uart_hex32(sel);
        uart_puts(" size=");
        uart_hex32(size);
        uart_puts("\n");

        // Compare with target
        bool match = true;
        for (size_t k = 0; k < 56; k++) {
            char a = ent.name[k];
            char b = (k < c_strlen(target)) ? target[k] : '\0';
            if (a != b) { match = false; break; }
            if (a == '\0' && b == '\0') break;
        }

        if (match) {
            out_sel  = sel;
            out_size = size;
            return true;
        }
    }
    return false;
}

static constexpr uint32_t fourcc(char a, char b, char c, char d) {
    return (uint32_t)(uint8_t)a |
           ((uint32_t)(uint8_t)b << 8) |
           ((uint32_t)(uint8_t)c << 16) |
           ((uint32_t)(uint8_t)d << 24);
}

/* ------------------------- Heat2D demo ------------------------- */
static constexpr uint32_t FB_W = 800;
static constexpr uint32_t FB_H = 600;

// 200x150 maps perfectly to 800x600 with 4x4 pixel blocks
static constexpr uint32_t SIM_W = 200;
static constexpr uint32_t SIM_H = 150;

static float g_field[SIM_W * SIM_H];
static float g_next[SIM_W * SIM_H];

struct RGB { uint8_t r, g, b; };
struct Stop { float t; RGB c; };
struct Palette { const char* name; Stop s[4]; };

static Palette g_pal[3] = {
    {"Fiery", {
        {0.00f, { 20,  24,  82}},
        {0.35f, { 30, 120, 200}},
        {0.65f, {255, 180,  60}},
        {1.00f, {255, 255, 245}},
    }},
    {"Ocean", {
        {0.00f, { 10,  40,  70}},
        {0.40f, { 40, 140, 170}},
        {0.75f, { 80, 210, 190}},
        {1.00f, {230, 255, 255}},
    }},
    {"Magenta", {
        {0.00f, { 55,  10,  60}},
        {0.35f, {140,  30, 140}},
        {0.70f, {240, 120, 200}},
        {1.00f, {255, 240, 255}},
    }},
};

static uint32_t g_lut[3][256];

static inline float clamp01(float x) {
    if (x < 0.f) return 0.f;
    if (x > 1.f) return 1.f;
    return x;
}

static inline uint8_t lerp_u8(uint8_t a, uint8_t b, float t) {
    float v = (float)a + ((float)b - (float)a) * t;
    if (v < 0.f) v = 0.f;
    if (v > 255.f) v = 255.f;
    return (uint8_t)(v + 0.5f);
}

static RGB sample_palette(const Palette& p, float t) {
    t = clamp01(t);
    for (int i = 1; i < 4; i++) {
        if (t <= p.s[i].t) {
            float t0 = p.s[i-1].t;
            float t1 = p.s[i].t;
            float span = (t1 - t0);
            float u = (span > 0.f) ? ((t - t0) / span) : 0.f;
            RGB a = p.s[i-1].c;
            RGB b = p.s[i].c;
            return { lerp_u8(a.r, b.r, u), lerp_u8(a.g, b.g, u), lerp_u8(a.b, b.b, u) };
        }
    }
    return p.s[3].c;
}

static void build_luts() {
    for (int p = 0; p < 3; p++) {
        for (int i = 0; i < 256; i++) {
            float t = (float)i / 255.0f;
            RGB c = sample_palette(g_pal[p], t);
            // XRGB8888: 0x00RRGGBB
            g_lut[p][i] = ((uint32_t)c.r << 16) | ((uint32_t)c.g << 8) | (uint32_t)c.b;
        }
    }
}

static void reset_field() {
    for (uint32_t i = 0; i < SIM_W * SIM_H; i++) {
        g_field[i] = 0.02f;
        g_next[i]  = 0.02f;
    }
}

static void stamp_disk(float* buf, int cx, int cy, int r, float v) {
    int r2 = r * r;
    for (int dy = -r; dy <= r; dy++) {
        for (int dx = -r; dx <= r; dx++) {
            int x = cx + dx;
            int y = cy + dy;
            if (x <= 0 || y <= 0 || x >= (int)SIM_W-1 || y >= (int)SIM_H-1) continue;
            if (dx*dx + dy*dy <= r2) buf[(uint32_t)y * SIM_W + (uint32_t)x] = v;
        }
    }
}

static void step_sim() {
    constexpr float alpha   = 0.20f;
    constexpr float cooling = 0.0008f;

    for (uint32_t y = 1; y < SIM_H - 1; y++) {
        for (uint32_t x = 1; x < SIM_W - 1; x++) {
            uint32_t idx = y * SIM_W + x;
            float t = g_field[idx];
            float lap =
                g_field[idx - 1] + g_field[idx + 1] +
                g_field[idx - SIM_W] + g_field[idx + SIM_W] -
                4.0f * t;
            float next = t + alpha * lap - cooling * t;
            g_next[idx] = clamp01(next);
        }
    }

    // boundaries
    for (uint32_t x = 0; x < SIM_W; x++) {
        g_next[x] = 0.f;
        g_next[(SIM_H - 1) * SIM_W + x] = 0.f;
    }
    for (uint32_t y = 0; y < SIM_H; y++) {
        g_next[y * SIM_W] = 0.f;
        g_next[y * SIM_W + (SIM_W - 1)] = 0.f;
    }

    // heat source
    stamp_disk(g_next, (int)SIM_W/2, (int)SIM_H/2, 7, 1.0f);

    // swap
    for (uint32_t i = 0; i < SIM_W * SIM_H; i++) g_field[i] = g_next[i];
}

static void render(uint32_t* fb, uint32_t palette_idx) {
    constexpr uint32_t SCALE_X = FB_W / SIM_W; // 4
    constexpr uint32_t SCALE_Y = FB_H / SIM_H; // 4

    for (uint32_t y = 0; y < SIM_H; y++) {
        for (uint32_t x = 0; x < SIM_W; x++) {
            float t = g_field[y * SIM_W + x];
            uint32_t pi = (uint32_t)(t * 255.0f);
            if (pi > 255) pi = 255;
            uint32_t color = g_lut[palette_idx][pi];

            uint32_t base_y = y * SCALE_Y;
            uint32_t base_x = x * SCALE_X;

            for (uint32_t dy = 0; dy < SCALE_Y; dy++) {
                uint32_t* row = fb + (base_y + dy) * FB_W + base_x;
                for (uint32_t dx = 0; dx < SCALE_X; dx++) {
                    row[dx] = color;
                }
            }
        }
    }
}

/* ------------------------- Main ------------------------- */
extern "C" int main(void) {
    uart_puts("\n=== Heat2D on QEMU virt via ramfb (800x600) ===\n");
    uart_puts("PL011 @ "); uart_hex64(UART_BASE); uart_puts("\n");
    uart_puts("fw_cfg @ "); uart_hex64(FW_CFG_BASE);
    uart_puts(", DMA @ "); uart_hex64(FW_CFG_DMA_ADDR); uart_puts("\n");

    // Find etc/ramfb in fw_cfg directory
    uint16_t ramfb_sel = 0;
    uint32_t ramfb_size = 0;
    if (!fw_cfg_find_file("etc/ramfb", ramfb_sel, ramfb_size)) {
        uart_puts("fw_cfg: could not find etc/ramfb\nHALTING.\n");
        while (1) asm volatile("wfi");
    }

    uart_puts("fw_cfg: FOUND etc/ramfb select=");
    uart_hex32(ramfb_sel);
    uart_puts(" size=");
    uart_hex32(ramfb_size);
    uart_puts("\n");

    // Framebuffer placed right after .bss
    uintptr_t fb_addr = (uintptr_t)__bss_end__;
    fb_addr = (fb_addr + 0xFFFu) & ~0xFFFu; // 4K align

    uart_puts("Framebuffer addr = "); uart_hex64((uint64_t)fb_addr); uart_puts("\n");

    // Configure ramfb (IMPORTANT: packed struct = 28 bytes)
    RAMFBCfg cfg;
    cfg.addr_be   = bswap64((uint64_t)fb_addr);
    cfg.fourcc_be = bswap32(fourcc('X','R','2','4')); // XRGB8888
    cfg.flags_be  = bswap32(0);
    cfg.width_be  = bswap32(FB_W);
    cfg.height_be = bswap32(FB_H);
    cfg.stride_be = bswap32(0); // let QEMU compute stride (safe)

    uart_puts("Configuring ramfb...\n");
    fw_cfg_dma_transfer(((uint32_t)ramfb_sel << 16) | DMA_CTL_SELECT | DMA_CTL_WRITE,
                        &cfg, (uint32_t)sizeof(cfg));

    uart_puts("ramfb configured OK. Painting test screen...\n");

    // If ramfb config worked, you should immediately see this red screen
    uint32_t* fb = (uint32_t*)fb_addr;
    for (uint32_t i = 0; i < FB_W * FB_H; i++) fb[i] = 0x00FF0000; // red
    delay_ms(250);

    build_luts();
    reset_field();

    uart_puts("virt ramfb init OK, rendering Heat2D...\n");

    uint32_t pal = 0;
    uint32_t frame = 0;

    while (1) {
        step_sim();
        render(fb, pal);

        frame++;
        if ((frame % 600) == 0) { // roughly every ~10s at ~60fps-ish
            pal = (pal + 1) % 3;
        }

        delay_ms(16);
    }
}
