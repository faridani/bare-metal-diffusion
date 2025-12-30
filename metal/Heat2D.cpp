#include <circle/startup.h>
#include <circle/device.h>
#include <circle/interrupt.h>
#include <circle/koptions.h>
#include <circle/logger.h>
#include <circle/screen.h>
#include <circle/screen/font.h>
#include <circle/screen/fontwriter.h>
#include <circle/fonts/FreeSans24.h>
#include <circle/usb/usbhcidevice.h>
#include <circle/usb/usbkeyboard.h>
#include <circle/usb/usbmouse.h>
#include <circle/input/keyboardbuffer.h>
#include <circle/timer.h>
#include <circle/util.h>
#include <circle/synchronize.h>
#include <circle/types.h>

#include <array>
#include <algorithm>
#include <cmath>
#include <cstdint>

#ifndef MOUSE_BUTTON_LEFT
#define MOUSE_BUTTON_LEFT 0x01
#define MOUSE_BUTTON_RIGHT 0x02
#endif

// Bare-metal Raspberry Pi 5 port of the Heat2D demo. This version runs directly
// on hardware using Circle to manage the framebuffer, USB keyboard/mouse, and
// timing. It adds airflow/convection, higher simulation throughput, better
// fonts, and runtime color-scheme switching.

static constexpr unsigned kWidth = 320;
static constexpr unsigned kHeight = 200;
static constexpr unsigned kIterationsPerFrame = 6; // faster convergence
static constexpr float kAlpha = 0.12f;              // conductivity factor
static constexpr float kAirViscosity = 0.03f;       // airflow smoothing
static constexpr float kAirStrength = 0.06f;        // convection coupling
static constexpr float kMouseHeat = 0.9f;
static constexpr float kAmbient = 0.08f;
static constexpr float kSinkTemperature = 1.5f;

struct RGB
{
	uint8_t r;
	uint8_t g;
	uint8_t b;
};

class CHeat2DKernel : public CKernel
{
public:
	CHeat2DKernel (void);
	~CHeat2DKernel (void);

	boolean Initialize (void) override;
	boolean Run (void) override;

private:
	void ResetGrid ();
	void UpdateAirflow (float dt);
	void UpdateHeat (float dt);
	void HandleInput ();
	void Render ();
	void CyclePalette ();

	unsigned GetBufferOffset (unsigned x, unsigned y) const
	{
		return (y * kWidth) + x;
	}

private:
	CInterruptSystem m_Interrupt;
	CTimer m_Timer;
	CKernelOptions m_Options;
	CScreenDevice m_Screen;
	CLogger m_Logger;
	CUSBHCIDevice m_USBHCI;
	CUSBKeyboardDevice m_Keyboard;
	CUSBMouseDevice m_Mouse;
	CKeyboardBuffer m_KeyboardBuffer;
	CFont m_Font;
	CFontWriter m_FontWriter;

	std::array<float, kWidth * kHeight> m_Temp{};
	std::array<float, kWidth * kHeight> m_TempNext{};
	std::array<float, kWidth * kHeight> m_AirUx{};
	std::array<float, kWidth * kHeight> m_AirUy{};

	unsigned m_PaletteIndex{0};
	std::array<std::array<RGB, 256>, 3> m_Palettes{};
	int m_MouseX{static_cast<int> (kWidth / 2)};
	int m_MouseY{static_cast<int> (kHeight / 2)};
};

static CHeat2DKernel Kernel;

CHeat2DKernel::CHeat2DKernel (void)
:	m_Timer (&m_Interrupt),
	m_Options (),
	m_Screen (&m_Options),
	m_Logger (LogDebug, &m_Interrupt),
	m_USBHCI (&m_Interrupt, &m_Timer),
	m_Keyboard (&m_USBHCI, &m_KeyboardBuffer),
	m_Mouse (&m_USBHCI),
	m_Font (FreeSans24),
	m_FontWriter (&m_Screen)
{
	m_Palettes[0] = [] {
		std::array<RGB, 256> p{};
		for (size_t i = 0; i < p.size (); ++i)
		{
			p[i] = {static_cast<uint8_t> (i), static_cast<uint8_t> (std::min<size_t> (255, i * 2 / 3)), static_cast<uint8_t> (255 - i / 2)};
		}
		return p;
	} ();

	m_Palettes[1] = [] {
		std::array<RGB, 256> p{};
		for (size_t i = 0; i < p.size (); ++i)
		{
			p[i] = {static_cast<uint8_t> (255 - i / 3), static_cast<uint8_t> (i), static_cast<uint8_t> (i / 2)};
		}
		return p;
	} ();

	m_Palettes[2] = [] {
		std::array<RGB, 256> p{};
		for (size_t i = 0; i < p.size (); ++i)
		{
			p[i] = {static_cast<uint8_t> ((std::sin (i * 0.05f) * 0.5f + 0.5f) * 255),
			        static_cast<uint8_t> ((std::sin (i * 0.05f + 2) * 0.5f + 0.5f) * 255),
			        static_cast<uint8_t> ((std::sin (i * 0.05f + 4) * 0.5f + 0.5f) * 255)};
		}
		return p;
	} ();
}

CHeat2DKernel::~CHeat2DKernel (void)
{
}

boolean CHeat2DKernel::Initialize (void)
{
	boolean bOK = TRUE;

	if (bOK) bOK = m_Interrupt.Initialize ();
	if (bOK) bOK = m_Timer.Initialize ();
	if (bOK) bOK = m_Screen.Initialize ();
	if (bOK) bOK = m_USBHCI.Initialize ();
	if (bOK) bOK = m_Keyboard.Initialize ();
	if (bOK) bOK = m_Mouse.Initialize ();
	if (bOK) m_Logger.Initialize (&m_Timer, &m_Screen);
	if (bOK)
	{
		ResetGrid ();
	}
	return bOK;
}

void CHeat2DKernel::ResetGrid ()
{
	std::fill (m_Temp.begin (), m_Temp.end (), kAmbient);
	std::fill (m_TempNext.begin (), m_TempNext.end (), kAmbient);
	std::fill (m_AirUx.begin (), m_AirUx.end (), 0.0f);
	std::fill (m_AirUy.begin (), m_AirUy.end (), 0.0f);

	for (unsigned y = kHeight / 2 - 8; y < kHeight / 2 + 8; ++y)
	{
		for (unsigned x = kWidth / 2 - 12; x < kWidth / 2 + 12; ++x)
		{
			m_Temp[GetBufferOffset (x, y)] = kSinkTemperature;
		}
	}
}

void CHeat2DKernel::CyclePalette ()
{
	m_PaletteIndex = (m_PaletteIndex + 1) % m_Palettes.size ();
}

void CHeat2DKernel::UpdateAirflow (float dt)
{
	for (unsigned y = 1; y < kHeight - 1; ++y)
	{
		for (unsigned x = 1; x < kWidth - 1; ++x)
		{
			const unsigned idx = GetBufferOffset (x, y);
			const float curl = m_Temp[GetBufferOffset (x + 1, y)] - m_Temp[GetBufferOffset (x - 1, y)];
			m_AirUy[idx] += curl * kAirStrength * dt;
			m_AirUy[idx] *= (1.0f - kAirViscosity * dt);

			const float curlY = m_Temp[GetBufferOffset (x, y + 1)] - m_Temp[GetBufferOffset (x, y - 1)];
			m_AirUx[idx] -= curlY * kAirStrength * dt;
			m_AirUx[idx] *= (1.0f - kAirViscosity * dt);
		}
	}
}

void CHeat2DKernel::UpdateHeat (float dt)
{
	for (unsigned i = 0; i < kIterationsPerFrame; ++i)
	{
		for (unsigned y = 1; y < kHeight - 1; ++y)
		{
			for (unsigned x = 1; x < kWidth - 1; ++x)
			{
				const unsigned idx = GetBufferOffset (x, y);

				const float laplace =
					m_Temp[idx - 1] + m_Temp[idx + 1] +
					m_Temp[idx - kWidth] + m_Temp[idx + kWidth] -
					4.0f * m_Temp[idx];

				const float advectX = m_AirUx[idx] * (m_Temp[idx] - m_Temp[idx - 1]);
				const float advectY = m_AirUy[idx] * (m_Temp[idx] - m_Temp[idx - kWidth]);

				float next = m_Temp[idx] + kAlpha * laplace - dt * (advectX + advectY);

				// Keep the heat sink pinned hot.
				if (x > kWidth / 2 - 12 && x < kWidth / 2 + 12 &&
				    y > kHeight / 2 - 8 && y < kHeight / 2 + 8)
				{
					next = kSinkTemperature;
				}

				m_TempNext[idx] = std::max (kAmbient, next);
			}
		}
		std::swap (m_Temp, m_TempNext);
		UpdateAirflow (dt);
	}
}

void CHeat2DKernel::HandleInput ()
{
	TMouseState state;
	if (m_Mouse.Update (&state))
	{
		m_MouseX = std::clamp (m_MouseX + state.nX, 0, static_cast<int> (kWidth) - 1);
		m_MouseY = std::clamp (m_MouseY - state.nY, 0, static_cast<int> (kHeight) - 1);
		const unsigned idx = GetBufferOffset (static_cast<unsigned> (m_MouseX), static_cast<unsigned> (m_MouseY));

		if (state.ucButtons & MOUSE_BUTTON_LEFT)
		{
			m_Temp[idx] = kSinkTemperature * kMouseHeat;
		}
		else if (state.ucButtons & MOUSE_BUTTON_RIGHT)
		{
			m_Temp[idx] = kAmbient;
		}
	}

	unsigned code;
	while (m_KeyboardBuffer.GetKey (code))
	{
		switch (code)
		{
		case 'c':
		case 'C':
			CyclePalette ();
			break;
		case 'r':
		case 'R':
			ResetGrid ();
			break;
		default:
			break;
		}
	}
}

void CHeat2DKernel::Render ()
{
	const auto &palette = m_Palettes[m_PaletteIndex];

	for (unsigned y = 0; y < kHeight; ++y)
	{
		for (unsigned x = 0; x < kWidth; ++x)
		{
			const unsigned idx = GetBufferOffset (x, y);
			const float t = std::clamp (m_Temp[idx], 0.0f, 1.6f);
			const unsigned shade = static_cast<unsigned> (std::min (255.0f, t * 160.0f));
			const RGB color = palette[shade];
			m_Screen.WritePixel (x, y, color.r, color.g, color.b);
		}
	}

	m_FontWriter.SetPosition (8, 8);
	m_FontWriter.Write (m_Font, "Heat2D bare metal – Left click adds heat, right click cools, 'c' changes palette, 'r' resets.");
	m_Screen.Update ();
}

boolean CHeat2DKernel::Run (void)
{
	m_Logger.Write (LogNotice, "Heat2D bare-metal demo started");

	while (1)
	{
		const float dt = 0.12f;
		HandleInput ();
		UpdateHeat (dt);
		Render ();
	}
	return TRUE;
}

extern "C" void main (void)
{
	Kernel.Start ();
}
