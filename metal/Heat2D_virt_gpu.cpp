#include <stdint.h>
#include <stddef.h>

static inline void mmio_w32(uintptr_t a, uint32_t v){ *(volatile uint32_t*)a=v; }
static inline uint32_t mmio_r32(uintptr_t a){ return *(volatile uint32_t*)a; }
static inline void dmb_ishst(){ asm volatile("dmb ishst" ::: "memory"); }
static inline void cpu_relax(){ asm volatile("nop"); }

// ---------- UART (PL011 on virt) ----------
static constexpr uintptr_t UART0=0x09000000UL;
static constexpr uintptr_t UARTDR=UART0+0x00, UARTFR=UART0+0x18, UARTIBRD=UART0+0x24, UARTFBRD=UART0+0x28, UARTLCRH=UART0+0x2C, UARTCR=UART0+0x30, UARTICR=UART0+0x44;

static void uart_putc(char c){ while(mmio_r32(UARTFR)&(1u<<5)){} mmio_w32(UARTDR,(uint32_t)c); }
static void uart_puts(const char* s){ while(*s){ char c=*s++; if(c=='\n') uart_putc('\r'); uart_putc(c);} }
static void uart_put_u32(uint32_t v){ char b[11]; int i=0; if(!v){uart_putc('0');return;} while(v&&i<10){b[i++]=char('0'+(v%10));v/=10;} while(i--) uart_putc(b[i]); }
static void uart_put_hex64(uint64_t v){ static const char*h="0123456789abcdef"; uart_puts("0x"); for(int i=15;i>=0;--i) uart_putc(h[(v>>(i*4))&0xF]); }
static void uart_init(){
  mmio_w32(UARTCR,0); mmio_w32(UARTICR,0x7FF);
  mmio_w32(UARTIBRD,13); mmio_w32(UARTFBRD,1);
  mmio_w32(UARTLCRH,(1u<<4)|(3u<<5));
  mmio_w32(UARTCR,(1u<<0)|(1u<<8)|(1u<<9));
}

// ---------- virtio-mmio regs ----------
enum{
  VMMIO_MAGIC=0x000, VMMIO_VERSION=0x004, VMMIO_DEVICE_ID=0x008,
  VMMIO_DEVICE_FEAT=0x010, VMMIO_DEVICE_FEAT_SEL=0x014,
  VMMIO_DRIVER_FEAT=0x020, VMMIO_DRIVER_FEAT_SEL=0x024,
  VMMIO_GUEST_PAGE_SIZE=0x028,
  VMMIO_QUEUE_SEL=0x030, VMMIO_QUEUE_NUM_MAX=0x034, VMMIO_QUEUE_NUM=0x038,
  VMMIO_QUEUE_ALIGN=0x03c, VMMIO_QUEUE_PFN=0x040,
  VMMIO_QUEUE_READY=0x044, VMMIO_QUEUE_NOTIFY=0x050,
  VMMIO_INTERRUPT_STATUS=0x060, VMMIO_INTERRUPT_ACK=0x064,
  VMMIO_STATUS=0x070,
  VMMIO_QUEUE_DESC_LOW=0x080, VMMIO_QUEUE_DESC_HIGH=0x084,
  VMMIO_QUEUE_AVAIL_LOW=0x090, VMMIO_QUEUE_AVAIL_HIGH=0x094,
  VMMIO_QUEUE_USED_LOW=0x0a0, VMMIO_QUEUE_USED_HIGH=0x0a4
};

enum{ VSTAT_ACKNOWLEDGE=1, VSTAT_DRIVER=2, VSTAT_DRIVER_OK=4, VSTAT_FEATURES_OK=8 };
enum{ VRING_DESC_F_NEXT=1, VRING_DESC_F_WRITE=2 };

static constexpr uint16_t QSZ=16;
static constexpr uint32_t PGSZ=4096;
static constexpr uint32_t align_up(uint32_t x,uint32_t a){ return (x+a-1)&~(a-1); }

struct __attribute__((packed)) vring_desc{ uint64_t addr; uint32_t len; uint16_t flags; uint16_t next; };
struct __attribute__((packed)) vring_used_elem{ uint32_t id; uint32_t len; };

// Virtio legacy “split ring” sizes (include used_event/avail_event even if unused).
static constexpr uint32_t DESC_BYTES = sizeof(vring_desc)*QSZ;
static constexpr uint32_t AVAIL_BYTES = 6 + 2*QSZ;   // flags(2)+idx(2)+ring(2*QSZ)+used_event(2)
static constexpr uint32_t USED_BYTES  = 6 + 8*QSZ;   // flags(2)+idx(2)+ring(QSZ*8)+avail_event(2)
static constexpr uint32_t VRING_BYTES = align_up(DESC_BYTES + AVAIL_BYTES, PGSZ) + align_up(USED_BYTES, PGSZ);

static __attribute__((aligned(PGSZ))) uint8_t  vring_mem[VRING_BYTES];
static __attribute__((aligned(PGSZ))) vring_desc desc_v2[QSZ];
static __attribute__((aligned(PGSZ))) uint8_t  avail_v2_mem[PGSZ];
static __attribute__((aligned(PGSZ))) uint8_t  used_v2_mem[PGSZ];

static vring_desc* g_desc=nullptr;
static volatile uint16_t* g_avail_flags=nullptr;
static volatile uint16_t* g_avail_idx=nullptr;
static volatile uint16_t* g_avail_ring=nullptr;
static volatile uint16_t* g_used_flags=nullptr;
static volatile uint16_t* g_used_idx=nullptr;
static volatile vring_used_elem* g_used_ring=nullptr;

static uint16_t free_head=0;
static uint16_t used_last=0;

static uint32_t mmio_ver=0;
static uintptr_t dev_base=0;

// virtio-mmio slots on virt: 0x0a000000 + n*0x200
static constexpr uintptr_t MMIO_BASE=0x0A000000UL, MMIO_STRIDE=0x200UL;
static constexpr int MMIO_SLOTS=64;

static uintptr_t find_dev(uint32_t want){
  for(int i=0;i<MMIO_SLOTS;i++){
    uintptr_t b=MMIO_BASE + (uintptr_t)i*MMIO_STRIDE;
    if(mmio_r32(b+VMMIO_MAGIC)!=0x74726976u) continue; // "virt"
    if(mmio_r32(b+VMMIO_DEVICE_ID)==want) return b;
  }
  return 0;
}

static inline void set_status(uintptr_t b,uint32_t v){ mmio_w32(b+VMMIO_STATUS,v); }

static void ring_ptrs_from_base(uint8_t* base){
  g_desc = (vring_desc*)base;

  uint8_t* avail = base + DESC_BYTES;
  g_avail_flags = (uint16_t*)avail;
  g_avail_idx   = (uint16_t*)(avail + 2);
  g_avail_ring  = (uint16_t*)(avail + 4);

  uint8_t* used = base + align_up(DESC_BYTES + AVAIL_BYTES, PGSZ);
  g_used_flags = (uint16_t*)used;
  g_used_idx   = (uint16_t*)(used + 2);
  g_used_ring  = (vring_used_elem*)(used + 4);
}

static void freelist_init(){
  free_head=0;
  for(uint16_t i=0;i<QSZ;i++){ g_desc[i].flags=0; g_desc[i].next=i+1; }
  g_desc[QSZ-1].next=0xffff;

  *g_avail_flags=0; *g_avail_idx=0;
  *g_used_flags=0;  *g_used_idx=0;
  used_last=0;
}

static uint16_t alloc_desc(){
  uint16_t h=free_head;
  if(h==0xffff){ uart_puts("out of desc\n"); while(1){} }
  free_head=g_desc[h].next;
  return h;
}
static void free_chain(uint16_t head){
  uint16_t cur=head;
  while(1){
    uint16_t next = (g_desc[cur].flags & VRING_DESC_F_NEXT) ? g_desc[cur].next : 0xffff;
    g_desc[cur].next=free_head;
    g_desc[cur].flags=0;
    free_head=cur;
    if(next==0xffff) break;
    cur=next;
  }
}
static void notify(uintptr_t b){ dmb_ishst(); mmio_w32(b+VMMIO_QUEUE_NOTIFY,0); }

// OUT+IN submit (poll used.idx) with watchdog
static void submit_out_in(uintptr_t b, void* outbuf, uint32_t outlen, void* inbuf, uint32_t inlen, uint32_t* out_type){
  uint16_t d0=alloc_desc(), d1=alloc_desc();

  g_desc[d0].addr=(uint64_t)(uintptr_t)outbuf;
  g_desc[d0].len=outlen;
  g_desc[d0].flags=VRING_DESC_F_NEXT;
  g_desc[d0].next=d1;

  g_desc[d1].addr=(uint64_t)(uintptr_t)inbuf;
  g_desc[d1].len=inlen;
  g_desc[d1].flags=VRING_DESC_F_WRITE;
  g_desc[d1].next=0;

  uint16_t a = *g_avail_idx;
  g_avail_ring[a % QSZ] = d0;
  dmb_ishst();
  *g_avail_idx = a + 1;
  dmb_ishst();
  notify(b);

  uint32_t spins=0;
  while(used_last == *g_used_idx){
    if((++spins % 20000000u)==0){
      uart_puts("WAIT used="); uart_put_u32(*g_used_idx);
      uart_puts(" avail="); uart_put_u32(*g_avail_idx);
      uart_puts(" isr="); uart_put_u32(mmio_r32(b+VMMIO_INTERRUPT_STATUS));
      uart_puts(" st="); uart_put_u32(mmio_r32(b+VMMIO_STATUS));
      uart_puts("\n");
    }
    cpu_relax();
  }

  uint16_t ui = used_last % QSZ;
  (void)g_used_ring[ui].id;
  used_last++;

  if(out_type){
    struct hdr{ uint32_t type,flags; uint64_t fence; uint32_t ctx,pad; };
    *out_type=((hdr*)inbuf)->type;
  }
  free_chain(d0);
}

// ---------- virtio-gpu subset ----------
static constexpr uint32_t CMD_GET_DISPLAY_INFO=0x0100;
static constexpr uint32_t CMD_RESOURCE_CREATE_2D=0x0101;
static constexpr uint32_t CMD_SET_SCANOUT=0x0103;
static constexpr uint32_t CMD_RESOURCE_FLUSH=0x0104;
static constexpr uint32_t CMD_TRANSFER_TO_HOST_2D=0x0105;
static constexpr uint32_t CMD_RESOURCE_ATTACH_BACKING=0x0106;

static constexpr uint32_t RESP_OK_NODATA=0x1100;
static constexpr uint32_t RESP_OK_DISPLAY_INFO=0x1101;

static constexpr uint32_t FMT_BGRA=1;

struct __attribute__((packed)) gpu_hdr{ uint32_t type,flags; uint64_t fence; uint32_t ctx,pad; };
struct __attribute__((packed)) gpu_rect{ uint32_t x,y,w,h; };
struct __attribute__((packed)) cmd_create2d{ gpu_hdr h; uint32_t rid, fmt, w, hgt; };
struct __attribute__((packed)) cmd_scanout{ gpu_hdr h; gpu_rect r; uint32_t sid, rid; };
struct __attribute__((packed)) cmd_flush{ gpu_hdr h; gpu_rect r; uint32_t rid, pad; };
struct __attribute__((packed)) cmd_xfer{ gpu_hdr h; gpu_rect r; uint64_t off; uint32_t rid, pad; };
struct __attribute__((packed)) cmd_attach{ gpu_hdr h; uint32_t rid, n; };
struct __attribute__((packed)) mem_entry{ uint64_t addr; uint32_t len, pad; };

static constexpr uint32_t FB_W=800, FB_H=600, FB_BPP=4, FB_SIZE=FB_W*FB_H*FB_BPP;
static __attribute__((aligned(4096))) uint8_t fb[FB_SIZE];

static __attribute__((aligned(16))) struct { cmd_attach c; mem_entry e; } attach;
static __attribute__((aligned(16))) cmd_create2d create2d;
static __attribute__((aligned(16))) cmd_scanout scan;
static __attribute__((aligned(16))) cmd_xfer xfer;
static __attribute__((aligned(16))) cmd_flush flushc;
static __attribute__((aligned(16))) gpu_hdr resp;
static __attribute__((aligned(16))) uint8_t resp_disp[256];

static void expect(uint32_t got,uint32_t want){
  if(got!=want){ uart_puts("bad resp "); uart_put_hex64(got); uart_puts("\n"); while(1){} }
}

static void virtio_gpu_init(){
  dev_base = find_dev(16);
  if(!dev_base){ uart_puts("no virtio-gpu\n"); while(1){} }

  uart_puts("virtio-gpu mmio base = "); uart_put_hex64(dev_base); uart_puts("\n");
  mmio_ver = mmio_r32(dev_base+VMMIO_VERSION);
  uart_puts("virtio-mmio version = "); uart_put_u32(mmio_ver); uart_puts("\n");

  set_status(dev_base,0);
  set_status(dev_base,VSTAT_ACKNOWLEDGE);
  set_status(dev_base,VSTAT_ACKNOWLEDGE|VSTAT_DRIVER);

  // Legacy rule: in mmio v1, do not use FEAT_SEL > 0 (QEMU logs guest_error if you do). :contentReference[oaicite:3]{index=3}
  mmio_w32(dev_base+VMMIO_DRIVER_FEAT_SEL,0);
  mmio_w32(dev_base+VMMIO_DRIVER_FEAT,0);
  uart_puts("virtio: legacy feature write (sel=0 only)\n");

  // Legacy transport wants GuestPageSize before queue config. :contentReference[oaicite:4]{index=4}
  mmio_w32(dev_base+VMMIO_GUEST_PAGE_SIZE,PGSZ);

  set_status(dev_base,VSTAT_ACKNOWLEDGE|VSTAT_DRIVER|VSTAT_FEATURES_OK);

  // Queue 0
  mmio_w32(dev_base+VMMIO_QUEUE_SEL,0);
  uint32_t qmax=mmio_r32(dev_base+VMMIO_QUEUE_NUM_MAX);
  uart_puts("virtio: QUEUE_NUM_MAX="); uart_put_u32(qmax); uart_puts("\n");
  if(qmax < QSZ){ uart_puts("queue too small\n"); while(1){} }
  mmio_w32(dev_base+VMMIO_QUEUE_NUM,QSZ);

  if(mmio_ver>=2){
    ring_ptrs_from_base((uint8_t*)desc_v2);
    auto set64=[&](uintptr_t lo,uintptr_t hi,uint64_t v){ mmio_w32(dev_base+lo,(uint32_t)v); mmio_w32(dev_base+hi,(uint32_t)(v>>32)); };
    set64(VMMIO_QUEUE_DESC_LOW,VMMIO_QUEUE_DESC_HIGH,(uint64_t)(uintptr_t)desc_v2);
    set64(VMMIO_QUEUE_AVAIL_LOW,VMMIO_QUEUE_AVAIL_HIGH,(uint64_t)(uintptr_t)avail_v2_mem);
    set64(VMMIO_QUEUE_USED_LOW,VMMIO_QUEUE_USED_HIGH,(uint64_t)(uintptr_t)used_v2_mem);
    mmio_w32(dev_base+VMMIO_QUEUE_READY,1);
  } else {
    ring_ptrs_from_base(vring_mem);
    mmio_w32(dev_base+VMMIO_QUEUE_ALIGN,PGSZ);
    mmio_w32(dev_base+VMMIO_QUEUE_PFN,(uint32_t)((uintptr_t)vring_mem/PGSZ));
  }
  freelist_init();

  uart_puts("AFTER QUEUE INIT: about to GET_DISPLAY_INFO\n");
  set_status(dev_base,VSTAT_ACKNOWLEDGE|VSTAT_DRIVER|VSTAT_FEATURES_OK|VSTAT_DRIVER_OK);

  gpu_hdr info{}; info.type=CMD_GET_DISPLAY_INFO;
  uart_puts("gpu: GET_DISPLAY_INFO...\n");
  uint32_t rtype=0; submit_out_in(dev_base,&info,sizeof(info),resp_disp,sizeof(resp_disp),&rtype);
  expect(rtype,RESP_OK_DISPLAY_INFO);
  uart_puts("gpu: GET_DISPLAY_INFO ok\n");

  create2d = {}; create2d.h.type=CMD_RESOURCE_CREATE_2D; create2d.rid=1; create2d.fmt=FMT_BGRA; create2d.w=FB_W; create2d.hgt=FB_H;
  submit_out_in(dev_base,&create2d,sizeof(create2d),&resp,sizeof(resp),&rtype); expect(rtype,RESP_OK_NODATA);

  attach = {}; attach.c.h.type=CMD_RESOURCE_ATTACH_BACKING; attach.c.rid=1; attach.c.n=1; attach.e.addr=(uint64_t)(uintptr_t)fb; attach.e.len=FB_SIZE;
  submit_out_in(dev_base,&attach,sizeof(attach),&resp,sizeof(resp),&rtype); expect(rtype,RESP_OK_NODATA);

  scan = {}; scan.h.type=CMD_SET_SCANOUT; scan.r={0,0,FB_W,FB_H}; scan.sid=0; scan.rid=1;
  uart_puts("gpu: SET_SCANOUT...\n");
  submit_out_in(dev_base,&scan,sizeof(scan),&resp,sizeof(resp),&rtype); expect(rtype,RESP_OK_NODATA);
  uart_puts("gpu: SET_SCANOUT ok\n");
}

static void gpu_present(){
  uint32_t rtype=0;
  xfer = {}; xfer.h.type=CMD_TRANSFER_TO_HOST_2D; xfer.r={0,0,FB_W,FB_H}; xfer.off=0; xfer.rid=1;
  submit_out_in(dev_base,&xfer,sizeof(xfer),&resp,sizeof(resp),&rtype); expect(rtype,RESP_OK_NODATA);

  flushc = {}; flushc.h.type=CMD_RESOURCE_FLUSH; flushc.r={0,0,FB_W,FB_H}; flushc.rid=1;
  submit_out_in(dev_base,&flushc,sizeof(flushc),&resp,sizeof(resp),&rtype); expect(rtype,RESP_OK_NODATA);
}

// Heat2D -> pixels
static constexpr unsigned kW=180,kH=120;
static constexpr float kA=0.20f,kC=0.0008f;
static float f0[kW*kH], f1[kW*kH];
static inline float clamp01(float x){ return x<0?0:(x>1?1:x); }
static void reset(){ for(unsigned i=0;i<kW*kH;i++) f0[i]=f1[i]=0.02f; }
static void stamp(unsigned cx,unsigned cy,float v){
  const int r=6;
  for(int dy=-r;dy<=r;dy++) for(int dx=-r;dx<=r;dx++){
    int x=(int)cx+dx,y=(int)cy+dy;
    if(x<1||y<1||x>=(int)kW-1||y>=(int)kH-1) continue;
    if(dx*dx+dy*dy<=r*r) f1[y*(int)kW+x]=v;
  }
}
static void step(){
  for(unsigned y=1;y<kH-1;y++) for(unsigned x=1;x<kW-1;x++){
    unsigned i=y*kW+x;
    float t=f0[i];
    float lap=f0[i-1]+f0[i+1]+f0[i-kW]+f0[i+kW]-4.f*t;
    f1[i]=clamp01(t+kA*lap-kC*t);
  }
  for(unsigned x=0;x<kW;x++){ f1[x]=0; f1[(kH-1)*kW+x]=0; }
  for(unsigned y=0;y<kH;y++){ f1[y*kW]=0; f1[y*kW+(kW-1)]=0; }
  stamp(kW/2,kH/2,1.0f);
  for(unsigned i=0;i<kW*kH;i++) f0[i]=f1[i];
}
static void palette(float t,uint8_t&b,uint8_t&g,uint8_t&r,uint8_t&a){
  t=clamp01(t);
  float rr,gg,bb;
  if(t<0.35f){ float u=t/0.35f; rr=20+(30-20)*u; gg=24+(120-24)*u; bb=82+(200-82)*u; }
  else if(t<0.65f){ float u=(t-0.35f)/0.30f; rr=30+(255-30)*u; gg=120+(180-120)*u; bb=200+(60-200)*u; }
  else { float u=(t-0.65f)/0.35f; rr=255; gg=180+(255-180)*u; bb=60+(245-60)*u; }
  r=(uint8_t)rr; g=(uint8_t)gg; b=(uint8_t)bb; a=255;
}
static void render(){
  for(uint32_t y=0;y<FB_H;y++){
    uint32_t sy=(uint64_t)y*kH/FB_H;
    for(uint32_t x=0;x<FB_W;x++){
      uint32_t sx=(uint64_t)x*kW/FB_W;
      float t=f0[sy*kW+sx];
      uint8_t b,g,r,a; palette(t,b,g,r,a);
      uint32_t off=(y*FB_W+x)*4;
      fb[off+0]=b; fb[off+1]=g; fb[off+2]=r; fb[off+3]=a;
    }
  }
}

extern "C" void kmain(){
  uart_init();
  uart_puts("\n=== Heat2D virtio-gpu 800x600 on QEMU virt ===\n");
  virtio_gpu_init();
  uart_puts("ENTERING RENDER LOOP\n");
  reset();
  uint32_t frame=0;
  while(1){
    frame++;
    step();
    render();
    gpu_present();
    if((frame%60)==0){ uart_puts("frame "); uart_put_u32(frame); uart_puts("\n"); }
  }
}

extern "C" void __cxa_pure_virtual(){ uart_puts("pure virtual\n"); while(1){} }
