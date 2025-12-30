// Heat2D_virt_gpu.cpp - Bare-metal AArch64 on QEMU -M virt with virtio-gpu framebuffer
// Window: 800x600, pixel format: BGRA (VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM)
// Transport: virtio-mmio @ 0x0a000000 on QEMU virt  :contentReference[oaicite:2]{index=2}
// GPU command IDs + formats from virtio_gpu.h :contentReference[oaicite:3]{index=3}

#include <stdint.h>
#include <stddef.h>

static inline void mmio_w32(uintptr_t a, uint32_t v) { *(volatile uint32_t*)a = v; }
static inline uint32_t mmio_r32(uintptr_t a) { return *(volatile uint32_t*)a; }
static inline void dmb() { asm volatile("dmb ish" ::: "memory"); }

// ---------------- UART (PL011) ----------------
static constexpr uintptr_t UART0 = 0x09000000UL;
static constexpr uintptr_t UARTDR = UART0 + 0x00;
static constexpr uintptr_t UARTFR = UART0 + 0x18;
static constexpr uintptr_t UARTIBRD = UART0 + 0x24;
static constexpr uintptr_t UARTFBRD = UART0 + 0x28;
static constexpr uintptr_t UARTLCRH = UART0 + 0x2C;
static constexpr uintptr_t UARTCR = UART0 + 0x30;
static constexpr uintptr_t UARTICR = UART0 + 0x44;

static void uart_putc(char c) {
    while (mmio_r32(UARTFR) & (1u<<5)) {}
    mmio_w32(UARTDR, (uint32_t)c);
}
static void uart_puts(const char* s) {
    while (*s) {
        char c = *s++;
        if (c=='\n') uart_putc('\r');
        uart_putc(c);
    }
}
static void uart_init() {
    mmio_w32(UARTCR, 0);
    mmio_w32(UARTICR, 0x7FF);
    mmio_w32(UARTIBRD, 13);
    mmio_w32(UARTFBRD, 1);
    mmio_w32(UARTLCRH, (1u<<4) | (3u<<5));
    mmio_w32(UARTCR, (1u<<0) | (1u<<8) | (1u<<9));
}

// ---------------- Virtio-mmio (v1) ----------------
static constexpr uintptr_t VIRTIO0 = 0x0A000000UL; // first virtio-mmio slot on QEMU virt :contentReference[oaicite:4]{index=4}

enum {
    VMMIO_MAGIC        = 0x000,
    VMMIO_VERSION      = 0x004,
    VMMIO_DEVICE_ID    = 0x008,
    VMMIO_VENDOR_ID    = 0x00c,
    VMMIO_DEVICE_FEAT  = 0x010,
    VMMIO_DEVICE_FEAT_SEL = 0x014,
    VMMIO_DRIVER_FEAT  = 0x020,
    VMMIO_DRIVER_FEAT_SEL = 0x024,
    VMMIO_QUEUE_SEL    = 0x030,
    VMMIO_QUEUE_NUM_MAX= 0x034,
    VMMIO_QUEUE_NUM    = 0x038,
    VMMIO_QUEUE_READY  = 0x044,
    VMMIO_QUEUE_NOTIFY = 0x050,
    VMMIO_INTERRUPT_STATUS = 0x060,
    VMMIO_INTERRUPT_ACK    = 0x064,
    VMMIO_STATUS       = 0x070,
    VMMIO_QUEUE_DESC_LOW   = 0x080,
    VMMIO_QUEUE_DESC_HIGH  = 0x084,
    VMMIO_QUEUE_AVAIL_LOW  = 0x090,
    VMMIO_QUEUE_AVAIL_HIGH = 0x094,
    VMMIO_QUEUE_USED_LOW   = 0x0a0,
    VMMIO_QUEUE_USED_HIGH  = 0x0a4,
};

enum {
    VSTAT_ACKNOWLEDGE = 1,
    VSTAT_DRIVER      = 2,
    VSTAT_DRIVER_OK   = 4,
    VSTAT_FEATURES_OK = 8,
    VSTAT_FAILED      = 128
};

enum {
    VRING_DESC_F_NEXT = 1,
    VRING_DESC_F_WRITE = 2
};

struct __attribute__((packed)) vring_desc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
};

struct __attribute__((packed)) vring_avail {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[16];
    uint16_t used_event; // not used
};

struct __attribute__((packed)) vring_used_elem {
    uint32_t id;
    uint32_t len;
};

struct __attribute__((packed)) vring_used {
    uint16_t flags;
    uint16_t idx;
    vring_used_elem ring[16];
    uint16_t avail_event; // not used
};

// One queue = 16 entries (enough for simple command/response)
static constexpr uint16_t QSZ = 16;

// Must be aligned: desc 16 bytes, avail 2, used 4k typical; we’ll just align everything to 4096.
static __attribute__((aligned(4096))) vring_desc g_desc[QSZ];
static __attribute__((aligned(4096))) vring_avail g_avail;
static __attribute__((aligned(4096))) vring_used g_used;

static uint16_t g_free_head = 0;
static uint16_t g_used_last = 0;

// ---------------- Virtio-GPU structs/consts (subset) ----------------
// From linux uapi virtio_gpu.h :contentReference[oaicite:5]{index=5}
static constexpr uint32_t VIRTIO_GPU_CMD_GET_DISPLAY_INFO      = 0x0100;
static constexpr uint32_t VIRTIO_GPU_CMD_RESOURCE_CREATE_2D    = 0x0101;
static constexpr uint32_t VIRTIO_GPU_CMD_SET_SCANOUT           = 0x0103;
static constexpr uint32_t VIRTIO_GPU_CMD_RESOURCE_FLUSH        = 0x0104;
static constexpr uint32_t VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D   = 0x0105;
static constexpr uint32_t VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING = 0x0106;

static constexpr uint32_t VIRTIO_GPU_RESP_OK_NODATA            = 0x1100;
static constexpr uint32_t VIRTIO_GPU_RESP_OK_DISPLAY_INFO      = 0x1101;

static constexpr uint32_t VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM     = 1;

struct __attribute__((packed)) virtio_gpu_ctrl_hdr {
    uint32_t type;
    uint32_t flags;
    uint64_t fence_id;
    uint32_t ctx_id;
    uint32_t padding;
};

struct __attribute__((packed)) virtio_gpu_rect {
    uint32_t x, y, width, height;
};

struct __attribute__((packed)) virtio_gpu_resource_create_2d {
    virtio_gpu_ctrl_hdr hdr;
    uint32_t resource_id;
    uint32_t format;
    uint32_t width;
    uint32_t height;
};

struct __attribute__((packed)) virtio_gpu_set_scanout {
    virtio_gpu_ctrl_hdr hdr;
    virtio_gpu_rect r;
    uint32_t scanout_id;
    uint32_t resource_id;
};

struct __attribute__((packed)) virtio_gpu_resource_flush {
    virtio_gpu_ctrl_hdr hdr;
    virtio_gpu_rect r;
    uint32_t resource_id;
    uint32_t padding;
};

struct __attribute__((packed)) virtio_gpu_transfer_to_host_2d {
    virtio_gpu_ctrl_hdr hdr;
    virtio_gpu_rect r;
    uint64_t offset;
    uint32_t resource_id;
    uint32_t padding;
};

struct __attribute__((packed)) virtio_gpu_resource_attach_backing {
    virtio_gpu_ctrl_hdr hdr;
    uint32_t resource_id;
    uint32_t nr_entries;
};

struct __attribute__((packed)) virtio_gpu_mem_entry {
    uint64_t addr;
    uint32_t length;
    uint32_t padding;
};

struct __attribute__((packed)) virtio_gpu_resp_display_info {
    virtio_gpu_ctrl_hdr hdr;
    struct {
        virtio_gpu_rect r;
        uint32_t enabled;
        uint32_t flags;
    } pmodes[16];
};

// ---------------- Simple ring helpers ----------------
static void virtio_queue_init(uintptr_t base, uint32_t qsel) {
    mmio_w32(base + VMMIO_QUEUE_SEL, qsel);
    uint32_t max = mmio_r32(base + VMMIO_QUEUE_NUM_MAX);
    if (max < QSZ) {
        uart_puts("virtio: queue too small\n");
        while (1) {}
    }
    mmio_w32(base + VMMIO_QUEUE_NUM, QSZ);

    // Set queue addresses (physical == virtual in our no-MMU setup)
    auto set64 = [&](uintptr_t lo, uintptr_t hi, uint64_t val) {
        mmio_w32(base + lo, (uint32_t)(val & 0xffffffffu));
        mmio_w32(base + hi, (uint32_t)(val >> 32));
    };

    set64(VMMIO_QUEUE_DESC_LOW,  VMMIO_QUEUE_DESC_HIGH,  (uint64_t)(uintptr_t)g_desc);
    set64(VMMIO_QUEUE_AVAIL_LOW, VMMIO_QUEUE_AVAIL_HIGH, (uint64_t)(uintptr_t)&g_avail);
    set64(VMMIO_QUEUE_USED_LOW,  VMMIO_QUEUE_USED_HIGH,  (uint64_t)(uintptr_t)&g_used);

    mmio_w32(base + VMMIO_QUEUE_READY, 1);

    // init freelist
    g_free_head = 0;
    for (uint16_t i = 0; i < QSZ; i++) {
        g_desc[i].addr = 0;
        g_desc[i].len = 0;
        g_desc[i].flags = 0;
        g_desc[i].next = i + 1;
    }
    g_desc[QSZ - 1].next = 0xffff;

    g_avail.flags = 0;
    g_avail.idx = 0;
    g_used.flags = 0;
    g_used.idx = 0;
    g_used_last = 0;
}

static uint16_t alloc_desc() {
    uint16_t h = g_free_head;
    if (h == 0xffff) { uart_puts("virtio: out of desc\n"); while (1) {} }
    g_free_head = g_desc[h].next;
    return h;
}
static void free_chain(uint16_t head) {
    // free the chain by walking NEXT
    uint16_t cur = head;
    while (1) {
        uint16_t next = (g_desc[cur].flags & VRING_DESC_F_NEXT) ? g_desc[cur].next : 0xffff;
        g_desc[cur].next = g_free_head;
        g_desc[cur].flags = 0;
        g_free_head = cur;
        if (next == 0xffff) break;
        cur = next;
    }
}

static void notify(uintptr_t base, uint32_t qsel) {
    dmb();
    mmio_w32(base + VMMIO_QUEUE_NOTIFY, qsel);
}

// Submit one OUT buffer + one IN buffer (response), wait for used
static void submit_out_in(uintptr_t base, uint32_t qsel,
                          void* outbuf, uint32_t outlen,
                          void* inbuf,  uint32_t inlen,
                          uint32_t* out_resp_type /* optional */) {
    uint16_t d0 = alloc_desc();
    uint16_t d1 = alloc_desc();

    g_desc[d0].addr  = (uint64_t)(uintptr_t)outbuf;
    g_desc[d0].len   = outlen;
    g_desc[d0].flags = VRING_DESC_F_NEXT;
    g_desc[d0].next  = d1;

    g_desc[d1].addr  = (uint64_t)(uintptr_t)inbuf;
    g_desc[d1].len   = inlen;
    g_desc[d1].flags = VRING_DESC_F_WRITE; // device writes response
    g_desc[d1].next  = 0;

    uint16_t aidx = g_avail.idx;
    g_avail.ring[aidx % QSZ] = d0;
    dmb();
    g_avail.idx = aidx + 1;

    notify(base, qsel);

    // poll used ring
    while (g_used_last == g_used.idx) {}
    uint16_t used_i = g_used_last % QSZ;
    uint32_t id = g_used.ring[used_i].id;
    (void)id;
    g_used_last++;

    if (out_resp_type) {
        auto* hdr = (virtio_gpu_ctrl_hdr*)inbuf;
        *out_resp_type = hdr->type;
    }

    free_chain(d0);
}

// ---------------- GPU init + framebuffer ----------------
static constexpr uint32_t FB_W = 800;
static constexpr uint32_t FB_H = 600;
static constexpr uint32_t FB_BPP = 4;
static constexpr uint32_t FB_SIZE = FB_W * FB_H * FB_BPP;

static __attribute__((aligned(4096))) uint8_t framebuffer[FB_SIZE];

// command/response buffers
static __attribute__((aligned(16))) virtio_gpu_resp_display_info resp_disp;
static __attribute__((aligned(16))) virtio_gpu_ctrl_hdr resp_hdr;

static __attribute__((aligned(16))) virtio_gpu_resource_create_2d cmd_create;
static __attribute__((aligned(16))) virtio_gpu_set_scanout cmd_scanout;
static __attribute__((aligned(16))) virtio_gpu_resource_flush cmd_flush;
static __attribute__((aligned(16))) virtio_gpu_transfer_to_host_2d cmd_xfer;
static __attribute__((aligned(16))) struct {
    virtio_gpu_resource_attach_backing cmd;
    virtio_gpu_mem_entry entry;
} cmd_attach;

static void gpu_send(void* cmd, uint32_t cmd_len, void* resp, uint32_t resp_len, uint32_t expect_type) {
    uint32_t rtype = 0;
    submit_out_in(VIRTIO0, 0, cmd, cmd_len, resp, resp_len, &rtype);
    if (rtype != expect_type) {
        uart_puts("virtio-gpu: unexpected resp type\n");
        while (1) {}
    }
}

static void virtio_gpu_init_800x600() {
    // Basic virtio-mmio sanity
    uint32_t magic = mmio_r32(VIRTIO0 + VMMIO_MAGIC);
    uint32_t ver   = mmio_r32(VIRTIO0 + VMMIO_VERSION);
    uint32_t devid = mmio_r32(VIRTIO0 + VMMIO_DEVICE_ID);
    if (magic != 0x74726976u) { uart_puts("virtio: bad magic\n"); while (1) {} }
    if (ver < 2) { uart_puts("virtio: need mmio v2\n"); while (1) {} }
    (void)devid; // should be 16 for GPU, but we won’t hard-fail.

    // Reset + status handshake
    mmio_w32(VIRTIO0 + VMMIO_STATUS, 0);
    mmio_w32(VIRTIO0 + VMMIO_STATUS, VSTAT_ACKNOWLEDGE);
    mmio_w32(VIRTIO0 + VMMIO_STATUS, VSTAT_ACKNOWLEDGE | VSTAT_DRIVER);

    // Feature negotiation (minimal: accept none)
    mmio_w32(VIRTIO0 + VMMIO_DRIVER_FEAT_SEL, 0);
    mmio_w32(VIRTIO0 + VMMIO_DRIVER_FEAT, 0);
    mmio_w32(VIRTIO0 + VMMIO_DRIVER_FEAT_SEL, 1);
    mmio_w32(VIRTIO0 + VMMIO_DRIVER_FEAT, 0);

    mmio_w32(VIRTIO0 + VMMIO_STATUS, VSTAT_ACKNOWLEDGE | VSTAT_DRIVER | VSTAT_FEATURES_OK);
    // Some devices require re-read; keep simple.

    // Init control queue 0 (we ignore cursor queue 1 for now)
    virtio_queue_init(VIRTIO0, 0);

    mmio_w32(VIRTIO0 + VMMIO_STATUS, VSTAT_ACKNOWLEDGE | VSTAT_DRIVER | VSTAT_FEATURES_OK | VSTAT_DRIVER_OK);

    // (Optional) GET_DISPLAY_INFO
    virtio_gpu_ctrl_hdr cmd_info{};
    cmd_info.type = VIRTIO_GPU_CMD_GET_DISPLAY_INFO;
    gpu_send(&cmd_info, sizeof(cmd_info), &resp_disp, sizeof(resp_disp), VIRTIO_GPU_RESP_OK_DISPLAY_INFO);

    // RESOURCE_CREATE_2D (id=1)
    cmd_create = {};
    cmd_create.hdr.type = VIRTIO_GPU_CMD_RESOURCE_CREATE_2D;
    cmd_create.resource_id = 1;
    cmd_create.format = VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM;
    cmd_create.width = FB_W;
    cmd_create.height = FB_H;
    gpu_send(&cmd_create, sizeof(cmd_create), &resp_hdr, sizeof(resp_hdr), VIRTIO_GPU_RESP_OK_NODATA);

    // ATTACH_BACKING (resource 1 -> framebuffer)
    cmd_attach = {};
    cmd_attach.cmd.hdr.type = VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING;
    cmd_attach.cmd.resource_id = 1;
    cmd_attach.cmd.nr_entries = 1;
    cmd_attach.entry.addr = (uint64_t)(uintptr_t)framebuffer;
    cmd_attach.entry.length = FB_SIZE;
    gpu_send(&cmd_attach, sizeof(cmd_attach), &resp_hdr, sizeof(resp_hdr), VIRTIO_GPU_RESP_OK_NODATA);

    // SET_SCANOUT (scanout 0)
    cmd_scanout = {};
    cmd_scanout.hdr.type = VIRTIO_GPU_CMD_SET_SCANOUT;
    cmd_scanout.r = {0,0,FB_W,FB_H};
    cmd_scanout.scanout_id = 0;
    cmd_scanout.resource_id = 1;
    gpu_send(&cmd_scanout, sizeof(cmd_scanout), &resp_hdr, sizeof(resp_hdr), VIRTIO_GPU_RESP_OK_NODATA);
}

// push current framebuffer to host + flush
static void gpu_present_full() {
    cmd_xfer = {};
    cmd_xfer.hdr.type = VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D;
    cmd_xfer.r = {0,0,FB_W,FB_H};
    cmd_xfer.offset = 0;
    cmd_xfer.resource_id = 1;
    gpu_send(&cmd_xfer, sizeof(cmd_xfer), &resp_hdr, sizeof(resp_hdr), VIRTIO_GPU_RESP_OK_NODATA);

    cmd_flush = {};
    cmd_flush.hdr.type = VIRTIO_GPU_CMD_RESOURCE_FLUSH;
    cmd_flush.r = {0,0,FB_W,FB_H};
    cmd_flush.resource_id = 1;
    gpu_send(&cmd_flush, sizeof(cmd_flush), &resp_hdr, sizeof(resp_hdr), VIRTIO_GPU_RESP_OK_NODATA);
}

// ---------------- Heat2D -> pixels ----------------
static constexpr unsigned kSimW = 180;
static constexpr unsigned kSimH = 120;
static constexpr float    kAlpha = 0.20f;
static constexpr float    kCooling = 0.0008f;

static float field[kSimW*kSimH];
static float nextf[kSimW*kSimH];

static inline float clamp01(float x) { return x < 0.f ? 0.f : (x > 1.f ? 1.f : x); }

static void reset_field() {
    for (unsigned i=0;i<kSimW*kSimH;i++) field[i]=nextf[i]=0.02f;
}

static void stamp_heat(unsigned cx, unsigned cy, float v) {
    const int r = 6;
    for (int dy=-r; dy<=r; ++dy) for (int dx=-r; dx<=r; ++dx) {
        int x = (int)cx + dx, y = (int)cy + dy;
        if (x<1||y<1||x>=(int)kSimW-1||y>=(int)kSimH-1) continue;
        if (dx*dx+dy*dy <= r*r) nextf[y*kSimW + x] = v;
    }
}

static void step_sim(float dt) {
    const float r = kAlpha * dt;
    for (unsigned y=1;y<kSimH-1;y++) for (unsigned x=1;x<kSimW-1;x++) {
        unsigned idx = y*kSimW + x;
        float t = field[idx];
        float lap = field[idx-1]+field[idx+1]+field[idx-kSimW]+field[idx+kSimW]-4.f*t;
        nextf[idx] = clamp01(t + r*lap - kCooling*t);
    }
    for (unsigned x=0;x<kSimW;x++){ nextf[x]=0; nextf[(kSimH-1)*kSimW+x]=0; }
    for (unsigned y=0;y<kSimH;y++){ nextf[y*kSimW]=0; nextf[y*kSimW+(kSimW-1)]=0; }

    stamp_heat(kSimW/2, kSimH/2, 1.0f);

    for (unsigned i=0;i<kSimW*kSimH;i++) field[i]=nextf[i];
}

// simple palette (BGRA output)
static void sample_palette(float t, uint8_t& b, uint8_t& g, uint8_t& r, uint8_t& a) {
    t = clamp01(t);
    // dark blue -> cyan -> orange -> white (cheap)
    float r1,g1,b1;
    if (t < 0.35f) {
        float u = t/0.35f;
        r1 = 20 + (30-20)*u;
        g1 = 24 + (120-24)*u;
        b1 = 82 + (200-82)*u;
    } else if (t < 0.65f) {
        float u = (t-0.35f)/0.30f;
        r1 = 30 + (255-30)*u;
        g1 = 120 + (180-120)*u;
        b1 = 200 + (60-200)*u;
    } else {
        float u = (t-0.65f)/0.35f;
        r1 = 255;
        g1 = 180 + (255-180)*u;
        b1 = 60 + (245-60)*u;
    }
    r = (uint8_t)r1; g = (uint8_t)g1; b = (uint8_t)b1; a = 255;
}

static void render_to_framebuffer() {
    // scale sim to 800x600 with nearest-neighbor
    for (uint32_t y=0; y<FB_H; ++y) {
        uint32_t sy = (uint64_t)y * kSimH / FB_H;
        for (uint32_t x=0; x<FB_W; ++x) {
            uint32_t sx = (uint64_t)x * kSimW / FB_W;
            float t = field[sy*kSimW + sx];
            uint8_t b,g,r,a;
            sample_palette(t,b,g,r,a);
            uint32_t off = (y*FB_W + x) * 4;
            framebuffer[off+0] = b;
            framebuffer[off+1] = g;
            framebuffer[off+2] = r;
            framebuffer[off+3] = a;
        }
    }
}

static void busy_wait(uint64_t iters) { for (volatile uint64_t i=0;i<iters;i++) asm volatile("nop"); }

// ---------------- Entry ----------------
extern "C" void kmain() {
    uart_init();
    uart_puts("\n=== Heat2D virtio-gpu (800x600) on QEMU virt ===\n");

    // IMPORTANT: add the virtio-gpu device on QEMU command line (see below),
    // then it will appear at the first virtio-mmio slot.
    virtio_gpu_init_800x600();
    uart_puts("virtio-gpu init OK, starting frames...\n");

    reset_field();

    while (1) {
        step_sim(1.0f);
        render_to_framebuffer();
        gpu_present_full();

        // ~60-ish fps crude delay
        busy_wait(1'000'000);
    }
}

extern "C" void __cxa_pure_virtual() {
    uart_puts("pure virtual call\n");
    while (1) {}
}
