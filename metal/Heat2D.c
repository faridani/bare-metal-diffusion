// Heat2D bare-metal renderer for Raspberry Pi 5 (BCM2712)
// Build with Circle (https://github.com/rsta2/circle)
// NOTE: Although this file uses a .c extension, it must be compiled as C++17.
//       The Circle build system already treats sources as C++.

#include <circle/actled.h>
#include <circle/koptions.h>
#include <circle/exceptionhandler.h>
#include <circle/interrupt.h>
#include <circle/logger.h>
#include <circle/serial.h>
#include <circle/screen.h>
#include <circle/timer.h>
#include <circle/util.h>
#include <circle/2dgraphics.h>
#include <circle/font.h>
#include <circle/types.h>
#include <circle/startup.h>

#include <stdio.h>
#include <stdlib.h>

namespace {

static const unsigned kSimW = 180;
static const unsigned kSimH = 120;
static const float    kAlpha = 0.20f; // r parameter for explicit scheme
static const float    kCooling = 0.0008f;
static const unsigned kLegendHeight = 56;
static const unsigned kTextMargin = 12;
static const unsigned kHeatDiskRadius = 6;

struct ColorStop {
    float t;
    T2DColor c;
};

struct Palette {
    const char* name;
    ColorStop stops[4];
};

static T2DColor LerpColor(T2DColor a, T2DColor b, float t) {
    t = t < 0.f ? 0.f : (t > 1.f ? 1.f : t);
    unsigned ar = (a >> 16) & 0xFF, ag = (a >> 8) & 0xFF, ab = a & 0xFF;
    unsigned br = (b >> 16) & 0xFF, bg = (b >> 8) & 0xFF, bb = b & 0xFF;
    unsigned cr = static_cast<unsigned>(ar + (br - ar) * t);
    unsigned cg = static_cast<unsigned>(ag + (bg - ag) * t);
    unsigned cb = static_cast<unsigned>(ab + (bb - ab) * t);
    return COLOR2D(cr, cg, cb);
}

static T2DColor SamplePalette(const Palette& p, float t) {
    t = t < 0.f ? 0.f : (t > 1.f ? 1.f : t);
    for (unsigned i = 1; i < 4; ++i) {
        if (t <= p.stops[i].t) {
            float span = p.stops[i].t - p.stops[i - 1].t;
            float local = span > 0.f ? (t - p.stops[i - 1].t) / span : 0.f;
            return LerpColor(p.stops[i - 1].c, p.stops[i].c, local);
        }
    }
    return p.stops[3].c;
}

class CHeat2DKernel {
public:
    CHeat2DKernel();
    ~CHeat2DKernel();

    bool Initialize();
    TShutdownMode Run();

private:
    void HandleKey(const char* text);
    void ResetField();
    void StepSimulation(float dt);
    void Render();
    void StampHeat(float* buffer, unsigned cx, unsigned cy, float value);
    void NextPalette();

private:
    // Must follow Circle object order rules
    CActLED             m_ActLED;
    CKernelOptions      m_Options;
    CSerialDevice       m_Serial;
    CExceptionHandler   m_ExceptionHandler;
    CInterruptSystem    m_Interrupt;
    CTimer              m_Timer;
    CLogger             m_Logger;
    CScreenDevice       m_Screen;
    C2DGraphics         m_Gfx;

    volatile TShutdownMode       m_ShutdownMode;

    float* m_Field;
    float* m_FieldNext;
    unsigned m_Frame;
    unsigned m_SchemeIndex;
    unsigned m_LastPaletteSecond;

    Palette m_Palettes[3];
};

CHeat2DKernel::CHeat2DKernel()
: m_Serial(&m_Interrupt)
, m_Timer(&m_Interrupt)
, m_Logger(LogNotice, &m_Timer)
, m_Screen(m_Options.GetWidth(), m_Options.GetHeight())
, m_Gfx(m_Options.GetWidth(), m_Options.GetHeight(), TRUE)
, m_ShutdownMode(ShutdownNone)
, m_Field(nullptr)
, m_FieldNext(nullptr)
, m_Frame(0)
, m_SchemeIndex(0)
, m_LastPaletteSecond(0)
{
    m_ActLED.Blink(5);

    m_Palettes[0] = {"Fiery", {
        {0.00f, COLOR2D(20, 24, 82)},
        {0.35f, COLOR2D(30, 120, 200)},
        {0.65f, COLOR2D(255, 180, 60)},
        {1.00f, COLOR2D(255, 255, 245)} } };

    m_Palettes[1] = {"Ocean", {
        {0.00f, COLOR2D(10, 40, 70)},
        {0.40f, COLOR2D(40, 140, 170)},
        {0.75f, COLOR2D(80, 210, 190)},
        {1.00f, COLOR2D(230, 255, 255)} } };

    m_Palettes[2] = {"Magenta", {
        {0.00f, COLOR2D(55, 10, 60)},
        {0.35f, COLOR2D(140, 30, 140)},
        {0.70f, COLOR2D(240, 120, 200)},
        {1.00f, COLOR2D(255, 240, 255)} } };
}

CHeat2DKernel::~CHeat2DKernel() {
    delete[] m_Field;
    delete[] m_FieldNext;
}

bool CHeat2DKernel::Initialize() {
    if (!m_Screen.Initialize() ||
        !m_Serial.Initialize(115200) ||
        !m_Interrupt.Initialize() ||
        !m_Timer.Initialize() ||
        !m_ExceptionHandler.Initialize() ||
        !m_Logger.Initialize(&m_Serial) ||
        !m_Gfx.Initialize()) {
        return false;
    }

    m_Field = new float[kSimW * kSimH];
    m_FieldNext = new float[kSimW * kSimH];
    if (!m_Field || !m_FieldNext) return false;
    ResetField();

    return true;
}

void CHeat2DKernel::ResetField() {
    for (unsigned i = 0; i < kSimW * kSimH; ++i) {
        m_Field[i] = 0.02f;
        m_FieldNext[i] = 0.02f;
    }
}

void CHeat2DKernel::StampHeat(float* buffer, unsigned cx, unsigned cy, float value) {
    for (int dy = -static_cast<int>(kHeatDiskRadius); dy <= static_cast<int>(kHeatDiskRadius); ++dy) {
        for (int dx = -static_cast<int>(kHeatDiskRadius); dx <= static_cast<int>(kHeatDiskRadius); ++dx) {
            int x = static_cast<int>(cx) + dx;
            int y = static_cast<int>(cy) + dy;
            if (x < 1 || y < 1 || x >= static_cast<int>(kSimW - 1) || y >= static_cast<int>(kSimH - 1)) continue;
            if (dx*dx + dy*dy <= static_cast<int>(kHeatDiskRadius * kHeatDiskRadius)) {
                buffer[y * kSimW + x] = value;
            }
        }
    }
}

void CHeat2DKernel::StepSimulation(float dt) {
    const float r = kAlpha * dt;
    for (unsigned y = 1; y < kSimH - 1; ++y) {
        for (unsigned x = 1; x < kSimW - 1; ++x) {
            unsigned idx = y * kSimW + x;
            float t = m_Field[idx];
            float lap = m_Field[idx - 1] + m_Field[idx + 1] + m_Field[idx - kSimW] + m_Field[idx + kSimW] - 4.f * t;
            float next = t + r * lap - kCooling * t;
            if (next < 0.f) next = 0.f;
            if (next > 1.f) next = 1.f;
            m_FieldNext[idx] = next;
        }
    }
    // Dirichlet cold boundaries
    for (unsigned x = 0; x < kSimW; ++x) {
        m_FieldNext[x] = 0.f;
        m_FieldNext[(kSimH - 1) * kSimW + x] = 0.f;
    }
    for (unsigned y = 0; y < kSimH; ++y) {
        m_FieldNext[y * kSimW] = 0.f;
        m_FieldNext[y * kSimW + (kSimW - 1)] = 0.f;
    }

    // Center heater
    StampHeat(m_FieldNext, kSimW / 2, kSimH / 2, 1.0f);

    float* tmp = m_Field;
    m_Field = m_FieldNext;
    m_FieldNext = tmp;
}

void CHeat2DKernel::Render() {
    const unsigned screenW = m_Gfx.GetWidth();
    const unsigned screenH = m_Gfx.GetHeight();
    const unsigned cellW = screenW / kSimW;
    const unsigned cellH = (screenH - kLegendHeight) / kSimH;
    const unsigned startY = kLegendHeight;
    const Palette& palette = m_Palettes[m_SchemeIndex];

    m_Gfx.ClearScreen(COLOR2D(8, 10, 16));

    for (unsigned y = 0; y < kSimH; ++y) {
        for (unsigned x = 0; x < kSimW; ++x) {
            T2DColor color = SamplePalette(palette, m_Field[y * kSimW + x]);
            m_Gfx.DrawRect(x * cellW, startY + y * cellH, cellW + 1, cellH + 1, color);
        }
    }

    char hud[128];
    snprintf(hud, sizeof(hud), "Heat2D bare-metal · palette: %s · frame %u", palette.name, m_Frame);
    m_Gfx.DrawText(kTextMargin, kTextMargin, COLOR2D(210, 220, 230), hud,
                   C2DGraphics::AlignLeft, Font12x22);

    const char* help = "Space: reset · C: palette · Esc: halt";
    m_Gfx.DrawText(screenW - kTextMargin, kTextMargin, COLOR2D(180, 190, 205), help,
                   C2DGraphics::AlignRight, Font12x22);

    // Legend bar
    unsigned legendX = kTextMargin;
    unsigned legendY = kLegendHeight - 24;
    unsigned legendW = screenW - 2 * kTextMargin;
    for (unsigned i = 0; i < legendW; ++i) {
        float t = static_cast<float>(i) / static_cast<float>(legendW - 1);
        m_Gfx.DrawRect(legendX + i, legendY, 1, 12, SamplePalette(palette, t));
    }

    m_Gfx.DrawRectOutline(legendX - 1, legendY - 1, legendW + 2, 14, COLOR2D(220, 220, 230));

    m_Gfx.UpdateDisplay();
}

void CHeat2DKernel::NextPalette() {
    m_SchemeIndex = (m_SchemeIndex + 1) % 3;
}

void CHeat2DKernel::HandleKey(const char* text) {
    if (!text || !*text) return;
    switch (text[0]) {
        case ' ': ResetField(); break;
        case 'c': case 'C': NextPalette(); break;
        case 0x1B: m_ShutdownMode = ShutdownHalt; break;
        default: break;
    }
}

TShutdownMode CHeat2DKernel::Run() {
    m_ShutdownMode = ShutdownNone;
    const float dt = 1.0f;

    while (m_ShutdownMode == ShutdownNone) {
        ++m_Frame;
        StepSimulation(dt);
        Render();
        if (m_Serial.AvailableForRead() > 0) {
            char ch;
            if (m_Serial.Read(&ch, 1) == 1) {
                char buf[2] = { ch, '\0' };
                HandleKey(buf);
            }
        }

        unsigned seconds = m_Timer.GetUptime();
        if (seconds - m_LastPaletteSecond >= 20) {
            NextPalette();
            m_LastPaletteSecond = seconds;
        }

        m_Timer.MsDelay(16);
    }
    return m_ShutdownMode;
}

} // namespace

int main(void) {
    CHeat2DKernel Kernel;
    if (!Kernel.Initialize()) {
        halt();
        return EXIT_HALT;
    }

    TShutdownMode mode = Kernel.Run();
    switch (mode) {
        case ShutdownReboot: reboot(); return EXIT_REBOOT;
        case ShutdownHalt:
        default: halt(); return EXIT_HALT;
    }
}
