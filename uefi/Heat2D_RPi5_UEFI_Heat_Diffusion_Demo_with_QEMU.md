# Heat2D (UEFI) — 2D Heat Diffusion Demo on Raspberry Pi 5 (HDMI, Pre‑OS)

> **Repository note:** All UEFI sources, helper scripts, and documentation now live under `uefi/` in this repository. When the guide below refers to `Heat2D.c`, `Heat2D.inf`, or `Heat2D.sh`, use the copies in `uefi/`.


It contains:

- Context + motivation (why do this in UEFI / before the OS)
- Pros/cons vs. running the same simulation under Linux
- A complete **EDK2 UEFI application** (AArch64) that:
  - Solves a simple **2D heat diffusion** equation on a grid
  - Renders a live **heatmap** to HDMI via UEFI **GOP** framebuffer writes
  - Draws a **legend bar** and **cursor crosshair**
  - Supports keyboard controls + (optional) mouse/touch painting via UEFI pointer protocols
- Step-by-step build + copy-to-SD + run instructions for **Raspberry Pi 5**

---

## Big picture: what you’re building

You will compile a single file UEFI application:

- Output: `Heat2D.efi` (AArch64 UEFI executable)

Then you will place it on the SD card’s EFI partition as either:

- `EFI/BOOT/BOOTAA64.EFI` (**auto-boot** path for removable media UEFI), or
- `Heat2D.efi` anywhere, and run it manually from the **UEFI Shell** (if your firmware includes one)

When the Pi 5 boots into UEFI, your app runs **without Linux / without Windows / without any OS**.

---

## Why do this in UEFI?

UEFI is firmware-level runtime that runs *before* an operating system. Doing graphics + simulation here is useful for:

- Learning low-level boot/firmware concepts
- Proving you can do “real” compute + graphics with minimal stack
- Building boot-time demos, diagnostics, or visualizations

This particular project is intentionally simple (explicit diffusion solver + direct framebuffer writes) but it demonstrates:

- Finding UEFI protocols (`LocateProtocol`)
- Using **Graphics Output Protocol** (GOP)
- Managing your own memory buffers
- Handling keyboard + pointer input using UEFI protocols

---

## UEFI method vs OS method (Linux/Windows)

### Benefits of running this as a UEFI app (pre‑OS)

- **Runs before any OS boots**
  - Great for boot-time demos, diagnostics, “wow factor”
- **Minimal dependencies**
  - No packages, no SDL/OpenGL, no userspace stack
- **Low-level learning**
  - Direct framebuffer pixel formats
  - UEFI input protocols
  - Boot-time constraints
- **Safe experimentation**
  - Use a separate SD card and swap back to your normal OS SD card at any time

### Cons of running this as a UEFI app

- **Input support varies**
  - Mouse/touch availability depends on what your UEFI firmware exposes
- **Graphics limitations**
  - Some GOP modes expose `PixelBltOnly` (no direct framebuffer access)
- **Debugging is harder**
  - No gdb, no logs unless you print or add serial output
- **No rich libraries**
  - No easy fonts, no file I/O conveniences (unless you add them)

### If you run the same demo under an OS (Linux + SDL2 / DRM / OpenGL)

- Easier input, better performance tools, richer UI
- Much easier to save images/video, use networking, etc.
- But it’s not “pre‑OS”, and you depend on the OS stack

**Rule of thumb:**  
- Choose **UEFI** when you want **boot-time / minimal-stack / low-level learning**.  
- Choose **Linux/OS** when you want **feature-rich, debuggable, polished apps**.

---

## How the heat diffusion works

We simulate a normalized temperature field `T` on a 2D grid.

The explicit finite-difference update:

\[
T^{n+1}_{i,j} = T^{n}_{i,j} + r \left(
T^n_{i+1,j} + T^n_{i-1,j} + T^n_{i,j+1} + T^n_{i,j-1} - 4T^n_{i,j}
\right)
\]

Where:

- `r = α Δt / Δx²` (we treat this as a tuning/stability parameter)
- For a 2D explicit scheme, a common stability guideline is **`r ≤ 0.25`**

This demo uses:

- `r = 0.20` (conservative and stable for typical conditions)
- A permanent “base” hot disk in the center (re-stamped each step)
- User-painted hot disks (via mouse/touch) that act like additional heat sources

---

## Requirements

### Hardware

- Raspberry Pi 5
- HDMI monitor
- USB keyboard
- Optional: USB mouse (recommended)
- SD card prepared to boot the Pi 5 into **UEFI** (EDK2-based firmware)

### Build machine (recommended)

A Linux PC/VM with:

- `git`, `make`, Python 3
- AArch64 cross compiler: `aarch64-linux-gnu-gcc`
- `nasm` and `iasl`

On Debian/Ubuntu:

```bash
sudo apt update
sudo apt install -y build-essential git python3 \
  gcc-aarch64-linux-gnu g++-aarch64-linux-gnu \
  nasm iasl uuid-dev
```

---

## SD card safety (important)

If you already have a Pi 5 SD card used as a print server (or any other role):

✅ You can keep that card unchanged and use a **separate SD card** for UEFI experiments.

Workflow:

1. Power off Pi
2. Insert UEFI/demo SD card → test Heat2D
3. Power off Pi
4. Insert original SD card → Pi returns to print server function

This is the safest way to experiment.

---

## Build instructions (EDK2)

### 1) Clone EDK2

```bash
git clone https://github.com/tianocore/edk2.git
cd edk2
git submodule update --init --recursive
```

### 2) Build BaseTools and set up the environment

```bash
make -C BaseTools
. edksetup.sh
```

---

## Project layout

Inside the `edk2/` directory you will create:

```
edk2/
  Heat2D/
    Heat2D.inf
    Heat2D.c
```

Create the folder:

```bash
mkdir -p Heat2D
```

---

## Source: `Heat2D/Heat2D.inf`

> **Important:** This INF includes `UefiBootServicesTableLib`, which provides the global `gBS` pointer (boot services). Without it, many UEFI apps fail to link.

```assembly
[Defines]
  INF_VERSION                    = 0x0001001A
  BASE_NAME                      = Heat2D
  FILE_GUID                      = 9A2DBA29-5D2E-4F58-8CC3-4D0D6D0D3F8C
  MODULE_TYPE                    = UEFI_APPLICATION
  VERSION_STRING                 = 2.2
  ENTRY_POINT                    = UefiMain

[Sources]
  Heat2D.c

[Packages]
  MdePkg/MdePkg.dec
  MdeModulePkg/MdeModulePkg.dec

[LibraryClasses]
  UefiApplicationEntryPoint
  UefiLib
  UefiBootServicesTableLib
  MemoryAllocationLib
  BaseMemoryLib
  PrintLib

[Protocols]
  gEfiGraphicsOutputProtocolGuid
  gEfiSimplePointerProtocolGuid
  gEfiAbsolutePointerProtocolGuid
```

---

## Source: `Heat2D/Heat2D.c` (revised + reviewed)

This code has been reviewed and includes fixes for common pitfalls:

- **AbsolutePointer “steals” the cursor:**  
  We only apply absolute pointer position when it is **pressed** or when it **moves and the mouse is idle**. This prevents idle touchscreen devices from constantly overriding USB mouse input.

- **PixelBltOnly:**  
  We detect and exit cleanly if direct framebuffer access is unsupported.

- **Legend coordinate underflow:**  
  Legend panel coordinates are clamped safely.

- **PixelBitMask support:**  
  If GOP is `PixelBitMask`, colors are packed using the provided masks.

### Controls

Keyboard:

- `Esc` — Quit
- `Space` — Pause/resume
- `R` — Reset (clear sim)
- `C` — Clear painted heat (base heat will reappear)
- `B` — Cycle boundary: Dirichlet → Neumann → Mixed
- `+` / `-` — Brush size
- `1` / `2` / `3` — Brush temperature (0.5 / 0.8 / 1.0)

Pointer (if available):

- Move mouse/touch — Move cursor
- Left click / press — Paint heat (a hot disk at the cursor)

---

```c
#include <Uefi.h>

#include <Library/UefiLib.h>
#include <Library/UefiApplicationEntryPoint.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/PrintLib.h>

#include <Protocol/GraphicsOutput.h>
#include <Protocol/SimplePointer.h>
#include <Protocol/AbsolutePointer.h>

typedef enum {
  BC_DIRICHLET_COLD = 0,   // fixed cold edges (0)
  BC_NEUMANN_INSULATED,    // zero-flux edges
  BC_MIXED,                // left/right cold, top/bottom insulated
  BC_COUNT
} BOUNDARY_MODE;

typedef struct {
  EFI_GRAPHICS_PIXEL_FORMAT Fmt;
  EFI_PIXEL_BITMASK         Masks;  // only used when Fmt == PixelBitMask
} PIXEL_PACKER;

STATIC
FLOAT32
ClampF32(FLOAT32 v, FLOAT32 lo, FLOAT32 hi)
{
  if (v < lo) return lo;
  if (v > hi) return hi;
  return v;
}

STATIC
INT32
ClampI32(INT32 v, INT32 lo, INT32 hi)
{
  if (v < lo) return lo;
  if (v > hi) return hi;
  return v;
}

STATIC
UINT32
Scale8ToMask(UINT8 c, UINT32 mask)
{
  if (mask == 0) return 0;

  // Find shift (lsb position)
  UINT32 shift = 0;
  UINT32 tmp = mask;
  while ((tmp & 1u) == 0u) {
    tmp >>= 1;
    shift++;
    if (shift >= 32) return 0; // defensive
  }

  // Count contiguous bits
  UINT32 bits = 0;
  while ((tmp & 1u) == 1u) {
    tmp >>= 1;
    bits++;
    if (bits >= 32) break;
  }

  if (bits == 0) return 0;

  UINT32 maxVal = (bits >= 32) ? 0xFFFFFFFFu : ((1u << bits) - 1u);

  // Map 0..255 -> 0..maxVal with rounding
  UINT32 scaled = (((UINT32)c) * maxVal + 127u) / 255u;

  return (scaled << shift) & mask;
}

STATIC
UINT32
PackPixel(const PIXEL_PACKER *P, UINT8 r, UINT8 g, UINT8 b)
{
  switch (P->Fmt) {
    case PixelRedGreenBlueReserved8BitPerColor:
      // memory: [R][G][B][X]
      return ((UINT32)r) | ((UINT32)g << 8) | ((UINT32)b << 16) | (0xFFu << 24);

    case PixelBlueGreenRedReserved8BitPerColor:
      // memory: [B][G][R][X]
      return ((UINT32)b) | ((UINT32)g << 8) | ((UINT32)r << 16) | (0xFFu << 24);

    case PixelBitMask: {
      UINT32 out = 0;
      out |= Scale8ToMask(r, P->Masks.RedMask);
      out |= Scale8ToMask(g, P->Masks.GreenMask);
      out |= Scale8ToMask(b, P->Masks.BlueMask);
      // Set reserved bits to 1 if present (often alpha/reserved)
      if (P->Masks.ReservedMask != 0) {
        out |= P->Masks.ReservedMask;
      }
      return out;
    }

    default:
      // PixelBltOnly should be rejected earlier.
      // Unknown formats are extremely unlikely; fall back to BGRX.
      return ((UINT32)b) | ((UINT32)g << 8) | ((UINT32)r << 16) | (0xFFu << 24);
  }
}

STATIC
VOID
TempToRGB(FLOAT32 t, UINT8 *r, UINT8 *g, UINT8 *b)
{
  t = ClampF32(t, 0.0f, 1.0f);

  // "thermal" ramp: blue -> cyan -> green -> yellow -> red -> white
  FLOAT32 x = t * 5.0f; // 0..5

  if (x < 1.0f) {            // blue -> cyan
    *r = 0;
    *g = (UINT8)(x * 255.0f);
    *b = 255;
  } else if (x < 2.0f) {     // cyan -> green
    *r = 0;
    *g = 255;
    *b = (UINT8)((2.0f - x) * 255.0f);
  } else if (x < 3.0f) {     // green -> yellow
    *r = (UINT8)((x - 2.0f) * 255.0f);
    *g = 255;
    *b = 0;
  } else if (x < 4.0f) {     // yellow -> red
    *r = 255;
    *g = (UINT8)((4.0f - x) * 255.0f);
    *b = 0;
  } else {                   // red -> white
    *r = 255;
    *g = (UINT8)((x - 4.0f) * 255.0f);
    *b = (UINT8)((x - 4.0f) * 255.0f);
  }
}

STATIC
BOOLEAN
TryReadKey(EFI_SYSTEM_TABLE *SystemTable, EFI_INPUT_KEY *OutKey)
{
  EFI_STATUS st = SystemTable->ConIn->ReadKeyStroke(SystemTable->ConIn, OutKey);
  return !EFI_ERROR(st);
}

STATIC
VOID
ApplyBoundary(FLOAT32 *T, INT32 NX, INT32 NY, BOUNDARY_MODE Mode)
{
  if (Mode == BC_DIRICHLET_COLD) {
    // edges fixed cold
    for (INT32 i = 0; i < NX; i++) {
      T[i] = 0.0f;
      T[(NY-1)*NX + i] = 0.0f;
    }
    for (INT32 j = 0; j < NY; j++) {
      T[j*NX] = 0.0f;
      T[j*NX + (NX-1)] = 0.0f;
    }
    return;
  }

  if (Mode == BC_NEUMANN_INSULATED) {
    // zero-flux: copy nearest interior
    for (INT32 i = 1; i < NX-1; i++) {
      T[0*NX + i]      = T[1*NX + i];
      T[(NY-1)*NX + i] = T[(NY-2)*NX + i];
    }
    for (INT32 j = 1; j < NY-1; j++) {
      T[j*NX + 0]      = T[j*NX + 1];
      T[j*NX + (NX-1)] = T[j*NX + (NX-2)];
    }

    // corners: copy nearest interior corner
    T[0] = T[1*NX + 1];
    T[NX-1] = T[1*NX + (NX-2)];
    T[(NY-1)*NX] = T[(NY-2)*NX + 1];
    T[(NY-1)*NX + (NX-1)] = T[(NY-2)*NX + (NX-2)];
    return;
  }

  // BC_MIXED:
  // left/right cold; top/bottom insulated.
  // Corner rule: if any side is Dirichlet, corners are Dirichlet (cold).
  for (INT32 j = 0; j < NY; j++) {
    T[j*NX + 0] = 0.0f;
    T[j*NX + (NX-1)] = 0.0f;
  }

  // top/bottom insulated (skip corners)
  for (INT32 i = 1; i < NX-1; i++) {
    T[0*NX + i]      = T[1*NX + i];
    T[(NY-1)*NX + i] = T[(NY-2)*NX + i];
  }

  // corners explicitly cold
  T[0] = 0.0f;
  T[NX-1] = 0.0f;
  T[(NY-1)*NX] = 0.0f;
  T[(NY-1)*NX + (NX-1)] = 0.0f;
}

STATIC
VOID
StampDisk(FLOAT32 *T, INT32 NX, INT32 NY, INT32 cx, INT32 cy, INT32 rad, FLOAT32 val)
{
  INT32 r2 = rad * rad;

  INT32 y0 = ClampI32(cy - rad, 0, NY-1);
  INT32 y1 = ClampI32(cy + rad, 0, NY-1);
  INT32 x0 = ClampI32(cx - rad, 0, NX-1);
  INT32 x1 = ClampI32(cx + rad, 0, NX-1);

  for (INT32 j = y0; j <= y1; j++) {
    INT32 dy = j - cy;
    for (INT32 i = x0; i <= x1; i++) {
      INT32 dx = i - cx;
      if (dx*dx + dy*dy <= r2) {
        FLOAT32 *p = &T[j*NX + i];
        if (val > *p) *p = val; // max-stamp
      }
    }
  }
}

STATIC
VOID
DrawRect(UINT32 *Fb, UINTN Width, UINTN Height, UINTN Ppsl,
         UINTN x0, UINTN y0, UINTN w, UINTN h, UINT32 px)
{
  if (x0 >= Width || y0 >= Height) return;

  UINTN x1 = x0 + w; if (x1 > Width) x1 = Width;
  UINTN y1 = y0 + h; if (y1 > Height) y1 = Height;

  for (UINTN y = y0; y < y1; y++) {
    UINT32 *row = Fb + y * Ppsl;
    for (UINTN x = x0; x < x1; x++) {
      row[x] = px;
    }
  }
}

STATIC
VOID
DrawLegend(UINT32 *Fb, UINTN Width, UINTN Height, UINTN Ppsl, const PIXEL_PACKER *Packer)
{
  // Right-side vertical bar legend
  UINTN barW = (Width > 200) ? 24 : 16;
  UINTN barH = (Height > 200) ? (Height / 2) : (Height * 2 / 3);

  UINTN x0 = (Width > (barW + 12)) ? (Width - barW - 12) : 0;
  UINTN y0 = 12;

  // Clamp panel coords explicitly (avoid unsigned underflow)
  UINTN panelX = (x0 >= 6) ? (x0 - 6) : 0;
  UINTN panelY = (y0 >= 6) ? (y0 - 6) : 0;
  UINTN panelW = barW + 12;
  UINTN panelH = barH + 12;

  UINT32 panel = PackPixel(Packer, 20, 20, 20);
  DrawRect(Fb, Width, Height, Ppsl, panelX, panelY, panelW, panelH, panel);

  for (UINTN y = 0; y < barH; y++) {
    FLOAT32 t = 1.0f - (FLOAT32)y / (FLOAT32)((barH > 1) ? (barH - 1) : 1);
    UINT8 r,g,b; TempToRGB(t, &r, &g, &b);
    UINT32 px = PackPixel(Packer, r, g, b);
    DrawRect(Fb, Width, Height, Ppsl, x0, y0 + y, barW, 1, px);
  }

  UINT32 border = PackPixel(Packer, 220, 220, 220);

  UINTN bx = (x0 > 0) ? (x0 - 1) : 0;
  UINTN by = (y0 > 0) ? (y0 - 1) : 0;

  DrawRect(Fb, Width, Height, Ppsl, bx, by, barW + 2, 1, border);
  DrawRect(Fb, Width, Height, Ppsl, bx, y0 + barH, barW + 2, 1, border);
  DrawRect(Fb, Width, Height, Ppsl, bx, by, 1, barH + 2, border);
  DrawRect(Fb, Width, Height, Ppsl, x0 + barW, by, 1, barH + 2, border);
}

STATIC
VOID
DrawCursor(UINT32 *Fb, UINTN Width, UINTN Height, UINTN Ppsl,
           UINTN x, UINTN y, const PIXEL_PACKER *Packer)
{
  UINT32 w = PackPixel(Packer, 255, 255, 255);
  DrawRect(Fb, Width, Height, Ppsl, (x > 2 ? x - 2 : 0), y, 5, 1, w);
  DrawRect(Fb, Width, Height, Ppsl, x, (y > 2 ? y - 2 : 0), 1, 5, w);
}

typedef struct {
  BOOLEAN HasAbs;
  BOOLEAN HasRel;

  EFI_ABSOLUTE_POINTER_PROTOCOL *Abs;
  EFI_SIMPLE_POINTER_PROTOCOL   *Rel;

  // cursor in screen pixels
  INT32 X;
  INT32 Y;

  // abs ranges
  INT32 AbsMinX, AbsMaxX;
  INT32 AbsMinY, AbsMaxY;

  // last absolute mapped position (for "absMoved" detection)
  BOOLEAN LastAbsValid;
  INT32   LastAbsX;
  INT32   LastAbsY;

  // rel scaling
  INT32 RelScale; // bigger = slower
} POINTER_STATE;

STATIC
VOID
InitPointer(POINTER_STATE *P, EFI_SYSTEM_TABLE *SystemTable, UINTN Width, UINTN Height)
{
  SetMem(P, sizeof(*P), 0);

  P->RelScale = 8;
  P->X = (INT32)(Width / 2);
  P->Y = (INT32)(Height / 2);

  EFI_STATUS st;

  st = gBS->LocateProtocol(&gEfiAbsolutePointerProtocolGuid, NULL, (VOID**)&P->Abs);
  if (!EFI_ERROR(st) && P->Abs) {
    P->HasAbs = TRUE;
    EFI_ABSOLUTE_POINTER_MODE *M = P->Abs->Mode;
    P->AbsMinX = (INT32)M->AbsoluteMinX;
    P->AbsMaxX = (INT32)M->AbsoluteMaxX;
    P->AbsMinY = (INT32)M->AbsoluteMinY;
    P->AbsMaxY = (INT32)M->AbsoluteMaxY;
  }

  st = gBS->LocateProtocol(&gEfiSimplePointerProtocolGuid, NULL, (VOID**)&P->Rel);
  if (!EFI_ERROR(st) && P->Rel) {
    P->HasRel = TRUE;
  }

  // Reset console input to avoid stale keystrokes
  SystemTable->ConIn->Reset(SystemTable->ConIn, FALSE);
}

STATIC
BOOLEAN
PollPointer(POINTER_STATE *P, UINTN Width, UINTN Height, BOOLEAN *OutPressed)
{
  // We read both protocols, but we avoid a common pitfall:
  // some firmwares expose an AbsolutePointer device that reports a fixed position
  // even when not "active". If we always apply absolute position every frame,
  // it can override mouse movement.
  //
  // Policy:
  // - If absolute pointer is pressed -> use absolute position (touch active)
  // - Else if mouse moved -> apply mouse delta
  // - Else if absolute moved -> use absolute position (touch moved)
  //
  // Pressed is true if either device reports a press.

  BOOLEAN absAvail = FALSE;
  BOOLEAN absPressed = FALSE;
  BOOLEAN absMoved = FALSE;
  INT32 absX = P->X;
  INT32 absY = P->Y;

  BOOLEAN relAvail = FALSE;
  BOOLEAN relPressed = FALSE;
  BOOLEAN relMoved = FALSE;
  INT32 dx = 0;
  INT32 dy = 0;

  // --- Absolute pointer read ---
  if (P->HasAbs && P->Abs) {
    EFI_ABSOLUTE_POINTER_STATE st;
    EFI_STATUS e = P->Abs->GetState(P->Abs, &st);
    if (!EFI_ERROR(e)) {
      absAvail = TRUE;

      INT32 ax = (INT32)st.CurrentX;
      INT32 ay = (INT32)st.CurrentY;

      INT32 rx = P->AbsMaxX - P->AbsMinX;
      INT32 ry = P->AbsMaxY - P->AbsMinY;
      if (rx <= 0) rx = 1;
      if (ry <= 0) ry = 1;

      // Map absolute range to [0..Width-1], [0..Height-1]
      absX = (INT32)((((INT64)(ax - P->AbsMinX)) * (INT64)((Width  > 0) ? (Width  - 1) : 0)) / rx);
      absY = (INT32)((((INT64)(ay - P->AbsMinY)) * (INT64)((Height > 0) ? (Height - 1) : 0)) / ry);

      absX = ClampI32(absX, 0, (INT32)Width  - 1);
      absY = ClampI32(absY, 0, (INT32)Height - 1);

      absPressed = (st.ActiveButtons != 0);

      if (!P->LastAbsValid || absX != P->LastAbsX || absY != P->LastAbsY) {
        absMoved = TRUE;
      }
      P->LastAbsValid = TRUE;
      P->LastAbsX = absX;
      P->LastAbsY = absY;
    }
  }

  // --- Relative pointer read ---
  if (P->HasRel && P->Rel) {
    EFI_SIMPLE_POINTER_STATE st;
    EFI_STATUS e = P->Rel->GetState(P->Rel, &st);
    if (!EFI_ERROR(e)) {
      relAvail = TRUE;

      dx = (INT32)(st.RelativeMovementX / P->RelScale);
      dy = (INT32)(st.RelativeMovementY / P->RelScale);
      relMoved = (dx != 0 || dy != 0);

      relPressed = (st.LeftButton != 0);
    }
  }

  BOOLEAN pressed = absPressed || relPressed;
  BOOLEAN moved = FALSE;

  if (absAvail && absPressed) {
    // Touch active: absolute wins
    moved = (absX != P->X) || (absY != P->Y);
    P->X = absX;
    P->Y = absY;
  } else {
    if (relAvail && relMoved) {
      // Mouse moved: apply delta
      INT32 nx = ClampI32(P->X + dx, 0, (INT32)Width  - 1);
      INT32 ny = ClampI32(P->Y + dy, 0, (INT32)Height - 1);
      moved = (nx != P->X) || (ny != P->Y);
      P->X = nx;
      P->Y = ny;
    } else if (absAvail && absMoved) {
      // No mouse movement; allow absolute motion to move cursor
      moved = (absX != P->X) || (absY != P->Y);
      P->X = absX;
      P->Y = absY;
    }
  }

  *OutPressed = pressed;
  return moved || pressed;
}

EFI_STATUS
EFIAPI
UefiMain(IN EFI_HANDLE ImageHandle, IN EFI_SYSTEM_TABLE *SystemTable)
{
  EFI_STATUS Status;
  EFI_GRAPHICS_OUTPUT_PROTOCOL *Gop = NULL;

  Status = gBS->LocateProtocol(&gEfiGraphicsOutputProtocolGuid, NULL, (VOID**)&Gop);
  if (EFI_ERROR(Status) || Gop == NULL) {
    Print(L"GOP not available: %r\n", Status);
    return Status;
  }

  EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *Info = Gop->Mode->Info;
  UINTN  Width  = Info->HorizontalResolution;
  UINTN  Height = Info->VerticalResolution;
  UINTN  Ppsl   = Info->PixelsPerScanLine;

  if (Width == 0 || Height == 0 || Ppsl == 0) {
    Print(L"Invalid GOP mode information.\n");
    return EFI_UNSUPPORTED;
  }

  // Initialize pixel packer + validate pixel format
  PIXEL_PACKER Packer;
  Packer.Fmt = Info->PixelFormat;

  if (Packer.Fmt == PixelBltOnly) {
    Print(L"Error: GOP PixelFormat is PixelBltOnly.\n");
    Print(L"This demo requires direct framebuffer access.\n");
    return EFI_UNSUPPORTED;
  }
  if (Packer.Fmt == PixelBitMask) {
    Packer.Masks = Info->PixelInformation;
  } else {
    SetMem(&Packer.Masks, sizeof(Packer.Masks), 0);
  }

  UINT32 *Fb = (UINT32*)(UINTN)Gop->Mode->FrameBufferBase;

  Print(L"Heat2D UEFI (Raspberry Pi 5)\n");
  Print(L"Resolution: %ux%u, PixelsPerScanLine=%u, PixelFormat=%u\n", Width, Height, Ppsl, (UINT32)Packer.Fmt);
  Print(L"Keys: Esc quit | Space pause | R reset | B boundary | C clear\n");
  Print(L"Brush: +/- size | 1/2/3 temp | Click/drag paint (if pointer available)\n");

  // ---- Simulation grid ----
  // These values are chosen for 1080p/4K typical monitors.
  // If you run an unusually tiny resolution (<240px wide), the display mapping may be imperfect.
  const INT32 NX = 240;
  const INT32 NY = 240;

  FLOAT32 *A = AllocateZeroPool(sizeof(FLOAT32) * NX * NY);
  FLOAT32 *B = AllocateZeroPool(sizeof(FLOAT32) * NX * NY);
  if (!A || !B) {
    Print(L"Out of memory\n");
    if (A) FreePool(A);
    if (B) FreePool(B);
    return EFI_OUT_OF_RESOURCES;
  }

  // Stability guide: r <= 0.25 for 2D explicit diffusion.
  const FLOAT32 r = 0.20f;

  // Base heat source (keeps it interesting without interaction)
  const INT32 baseCx = NX / 2, baseCy = NY / 2;
  const INT32 baseRad = NX / 14;
  const FLOAT32 baseTemp = 1.0f;

  // User brush
  INT32 brushRad = NX / 30;
  FLOAT32 brushTemp = 1.0f;

  BOUNDARY_MODE bc = BC_DIRICHLET_COLD;
  BOOLEAN Paused = FALSE;

  // ---- Rendering scaling ----
  UINTN cellW = Width / NX;
  UINTN cellH = Height / NY;
  if (cellW < 1) cellW = 1;
  if (cellH < 1) cellH = 1;

  // Performance knob: subsample on large displays
  UINTN drawSkip = 1;
  if (Width * Height > 1920u * 1080u) drawSkip = 2;

  UINTN drawW = (UINTN)NX * cellW;
  UINTN drawH = (UINTN)NY * cellH;
  if (drawW > Width)  drawW = Width;
  if (drawH > Height) drawH = Height;

  POINTER_STATE Ptr;
  InitPointer(&Ptr, SystemTable, Width, Height);

  // Clear background once
  UINT32 bg = PackPixel(&Packer, 0, 0, 0);
  DrawRect(Fb, Width, Height, Ppsl, 0, 0, Width, Height, bg);

  BOOLEAN dirty = TRUE;

  while (TRUE) {
    // ---- Keyboard ----
    EFI_INPUT_KEY Key;
    while (TryReadKey(SystemTable, &Key)) {
      if (Key.ScanCode == SCAN_ESC) goto done;

      if (Key.UnicodeChar == L' ') {
        Paused = !Paused;
        dirty = TRUE;
      } else if (Key.UnicodeChar == L'r' || Key.UnicodeChar == L'R') {
        SetMem(A, sizeof(FLOAT32)*NX*NY, 0);
        SetMem(B, sizeof(FLOAT32)*NX*NY, 0);
        dirty = TRUE;
      } else if (Key.UnicodeChar == L'c' || Key.UnicodeChar == L'C') {
        SetMem(A, sizeof(FLOAT32)*NX*NY, 0);
        dirty = TRUE;
      } else if (Key.UnicodeChar == L'b' || Key.UnicodeChar == L'B') {
        bc = (BOUNDARY_MODE)((bc + 1) % BC_COUNT);
        dirty = TRUE;
      } else if (Key.UnicodeChar == L'+' || Key.UnicodeChar == L'=') {
        brushRad = ClampI32(brushRad + 2, 2, NX/4);
        dirty = TRUE;
      } else if (Key.UnicodeChar == L'-' || Key.UnicodeChar == L'_') {
        brushRad = ClampI32(brushRad - 2, 2, NX/4);
        dirty = TRUE;
      } else if (Key.UnicodeChar == L'1') {
        brushTemp = 0.5f; dirty = TRUE;
      } else if (Key.UnicodeChar == L'2') {
        brushTemp = 0.8f; dirty = TRUE;
      } else if (Key.UnicodeChar == L'3') {
        brushTemp = 1.0f; dirty = TRUE;
      }
    }

    // ---- Pointer ----
    BOOLEAN pressed = FALSE;
    BOOLEAN ptrEvent = PollPointer(&Ptr, Width, Height, &pressed);

    // Map cursor pixel -> grid cell (use drawW/drawH region)
    INT32 gx = (INT32)((((INT64)Ptr.X) * NX) / (INT32)((drawW > 0) ? drawW : 1));
    INT32 gy = (INT32)((((INT64)Ptr.Y) * NY) / (INT32)((drawH > 0) ? drawH : 1));
    gx = ClampI32(gx, 0, NX-1);
    gy = ClampI32(gy, 0, NY-1);

    if (pressed) {
      StampDisk(A, NX, NY, gx, gy, brushRad, brushTemp);
      dirty = TRUE;
    } else if (ptrEvent) {
      dirty = TRUE; // cursor moved
    }

    // ---- Simulation ----
    if (!Paused) {
      // Re-stamp base heat each step
      StampDisk(A, NX, NY, baseCx, baseCy, baseRad, baseTemp);

      // Explicit diffusion step
      for (INT32 j = 1; j < NY-1; j++) {
        for (INT32 i = 1; i < NX-1; i++) {
          FLOAT32 t = A[j*NX + i];
          FLOAT32 lap =
            A[j*NX + (i+1)] + A[j*NX + (i-1)] +
            A[(j+1)*NX + i] + A[(j-1)*NX + i] - 4.0f*t;
          B[j*NX + i] = t + r * lap;
        }
      }

      ApplyBoundary(B, NX, NY, bc);

      // Swap buffers
      FLOAT32 *Tmp = A; A = B; B = Tmp;
      dirty = TRUE;
    }

    // ---- Render ----
    if (dirty) {
      for (INT32 j = 0; j < NY; j += (INT32)drawSkip) {
        for (INT32 i = 0; i < NX; i += (INT32)drawSkip) {
          FLOAT32 t = A[j*NX + i];

          UINT8 rr, gg, bb;
          TempToRGB(t, &rr, &gg, &bb);

          UINT32 px = PackPixel(&Packer, rr, gg, bb);

          UINTN x0 = (UINTN)i * cellW;
          UINTN y0 = (UINTN)j * cellH;

          UINTN cw = cellW * drawSkip;
          UINTN ch = cellH * drawSkip;
          if (cw < 1) cw = 1;
          if (ch < 1) ch = 1;

          if (x0 >= drawW || y0 >= drawH) continue;

          DrawRect(Fb, Width, Height, Ppsl, x0, y0, cw, ch, px);
        }
      }

      DrawLegend(Fb, Width, Height, Ppsl, &Packer);
      DrawCursor(Fb, Width, Height, Ppsl, (UINTN)Ptr.X, (UINTN)Ptr.Y, &Packer);

      dirty = FALSE;
    }

    // Throttle (microseconds)
    gBS->Stall(4000); // ~4ms
  }

done:
  FreePool(A);
  FreePool(B);
  Print(L"Exit.\n");
  return EFI_SUCCESS;
}
```

---

## Compile the UEFI application (AArch64)

From the `edk2/` directory:

```bash
export GCC5_AARCH64_PREFIX=aarch64-linux-gnu-
build -a AARCH64 -t GCC5 -p MdeModulePkg/MdeModulePkg.dsc -m Heat2D/Heat2D.inf
```

### Faster build output (RELEASE)

For smoother performance, build a RELEASE binary:

```bash
build -a AARCH64 -t GCC5 -b RELEASE -p MdeModulePkg/MdeModulePkg.dsc -m Heat2D/Heat2D.inf
```

### Where is the output?

Typically:

- DEBUG:
  `Build/MdeModule/DEBUG_GCC5/AARCH64/Heat2D.efi`

- RELEASE:
  `Build/MdeModule/RELEASE_GCC5/AARCH64/Heat2D.efi`

---

## Copy to the SD card (EFI partition)

### 1) Insert your UEFI SD card into the build machine

Find the EFI/FAT partition:

```bash
lsblk -f
```

Look for a small FAT partition (often 100–500MB) that mounts as something like:

- `/media/$USER/EFI`
- `/run/media/$USER/EFI`
- or similar

### 2) Copy the `.efi` to the default boot path

Make directories if needed:

```bash
mkdir -p /media/$USER/EFI/EFI/BOOT
```

Copy:

```bash
cp Build/MdeModule/RELEASE_GCC5/AARCH64/Heat2D.efi /media/$USER/EFI/EFI/BOOT/BOOTAA64.EFI
sync
```

> If you built DEBUG, adjust the path accordingly.

### 3) Eject safely and put SD back into the Pi 5

```bash
sudo umount /media/$USER/EFI
```

---

## Run on the Raspberry Pi 5

1. Connect HDMI monitor + USB keyboard (+ optional USB mouse)
2. Insert the UEFI SD card
3. Power on

### Auto-boot behavior

If the firmware follows standard removable-media fallback rules, it will load:

- `EFI/BOOT/BOOTAA64.EFI`

…and your program should start immediately.

### If it does not auto-boot

Use one of these options:

- Enter UEFI Boot Manager and choose the EFI app
- Use the UEFI Shell (if available) and run it manually:
  - `fs0:\EFI\BOOT\BOOTAA64.EFI`
  - or copy your app as `Heat2D.efi` and run `fs0:\Heat2D.efi`

### Secure Boot note

If Secure Boot is enabled, an unsigned `.efi` may be blocked.  
Disable Secure Boot in UEFI settings (firmware UI) for development.

---

## Troubleshooting

### 1) “PixelBltOnly not supported”
Your GOP mode does not allow direct framebuffer writes.

What it means:
- UEFI is asking you to render using `Gop->Blt()` instead of writing to `FrameBufferBase`.

Options:
- Try a different display mode / resolution in UEFI setup
- Use a different UEFI firmware build/config
- Or implement a BLT renderer (slower but universal). If you want, I can provide a BLT-based version.

### 2) Mouse doesn’t work
Pointer support depends on UEFI drivers.

This app supports:
- `EFI_SIMPLE_POINTER_PROTOCOL` (USB mouse)
- `EFI_ABSOLUTE_POINTER_PROTOCOL` (touch)

If your UEFI firmware exposes neither, pointer input won’t work and only keyboard controls will be available.

### 3) Black screen
Common causes:
- Wrong boot path (`BOOTAA64.EFI` not found)
- Secure Boot blocked the binary
- GOP mode issues (rare)
- The app is running but you’re on a different console output device

Try:
- Running from UEFI Shell so you can see error messages
- Confirm the file is in `EFI/BOOT/BOOTAA64.EFI`

### 4) Slow performance
Try:
- Build `RELEASE`
- Increase `drawSkip` to 2 or 3
- Reduce grid size `NX/NY` (e.g., 200)
- Increase stall slightly to reduce CPU load: `gBS->Stall(8000);`

---

## Tuning and customization

### Change grid size
In `UefiMain`:

```c
const INT32 NX = 240;
const INT32 NY = 240;
```

Typical:
- 200–300 is fine on Pi 5 UEFI depending on resolution.

### Change diffusion speed
Change:

```c
const FLOAT32 r = 0.20f;
```

Keep `r <= 0.25` for stability.

### Change palette
Modify `TempToRGB()` to use a different colormap.

### Add more heat sources
Call `StampDisk()` with additional positions.

---


---

## Develop & test on Ubuntu without rebooting (QEMU + UEFI firmware)

A very common UEFI development loop is:

1. Build your `.efi` with EDK2
2. Put it on a small FAT “EFI System Partition” (ESP) image
3. Boot a UEFI firmware in **QEMU** (e.g., **OVMF** for x86_64, or EDK2 “AAVMF/QEMU_EFI” for AArch64)
4. Run/test your app in the emulator — no rebooting real hardware

This is usually **much faster** than testing directly on a Raspberry Pi every edit.

### Important note about architecture

This project targets **AArch64** (ARM 64-bit), because Raspberry Pi 5 is AArch64.

- If you run **x86_64 OVMF**, you must compile an **x86_64** version of the app (`-a X64`) to run in that VM.
- If you want to test the **same AArch64 binary** that you’ll run on the Pi 5, use **qemu-system-aarch64** + an **AArch64 UEFI firmware image**.

Below are both workflows.

---

### Option A: x86_64 QEMU + OVMF (fast, common UEFI dev loop)

This is the most common UEFI dev setup overall (PC-style UEFI). It won’t emulate the Pi 5’s exact environment, but it’s great for iterating on UEFI logic, GOP rendering patterns, and input.

#### 1) Install packages

```bash
sudo apt update
sudo apt install -y qemu-system-x86 ovmf mtools dosfstools
```

#### 2) Build the X64 version of the app (in your EDK2 workspace)

From `edk2/`:

```bash
. edksetup.sh
build -a X64 -t GCC5 -b DEBUG -p MdeModulePkg/MdeModulePkg.dsc -m Heat2D/Heat2D.inf
```

Output example:

```
Build/MdeModule/DEBUG_GCC5/X64/Heat2D.efi
```

#### 3) Create a small ESP image and copy the app to the default boot path

```bash
mkdir -p /tmp/efi/EFI/BOOT
cp Build/MdeModule/DEBUG_GCC5/X64/Heat2D.efi /tmp/efi/EFI/BOOT/BOOTX64.EFI

dd if=/dev/zero of=/tmp/esp.img bs=1M count=64
mkfs.vfat /tmp/esp.img
mcopy -i /tmp/esp.img -s /tmp/efi/* ::
```

#### 4) Run QEMU with OVMF

Initialize a writable VARS file once:

```bash
cp /usr/share/OVMF/OVMF_VARS.fd /tmp/OVMF_VARS.fd
```

Then run:

```bash
qemu-system-x86_64 \
  -m 1024 \
  -drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE.fd \
  -drive if=pflash,format=raw,file=/tmp/OVMF_VARS.fd \
  -drive format=raw,file=/tmp/esp.img \
  -device usb-kbd -device usb-mouse \
  -display gtk
```

---

# Mark's Notes: I did this 
### Option B: AArch64 QEMU + AArch64 UEFI firmware (closest to Pi 5)

This runs the **same architecture** (AArch64) as the Pi 5, so it’s the best “no-reboot” test for your actual `BOOTAA64.EFI`.

#### 1) Install packages 

Package names vary by Ubuntu version. Try:

```bash
sudo apt update
sudo apt install -y qemu-system-arm qemu-system-aarch64 qemu-efi-aarch64 mtools dosfstools
```

If `qemu-efi-aarch64` isn’t available, search for firmware packages like `aavmf` or `edk2-uefi`, or build the firmware from EDK2.

#### 2) Build the AARCH64 version of the app

```bash
. edksetup.sh
export GCC5_AARCH64_PREFIX=aarch64-linux-gnu-
build -a AARCH64 -t GCC5 -b DEBUG -p MdeModulePkg/MdeModulePkg.dsc -m Heat2D/Heat2D.inf
```

#### 3) Create an ESP image containing BOOTAA64.EFI

```bash
mkdir -p /tmp/efi/EFI/BOOT
cp Build/MdeModule/DEBUG_GCC5/AARCH64/Heat2D.efi /tmp/efi/EFI/BOOT/BOOTAA64.EFI

dd if=/dev/zero of=/tmp/esp.img bs=1M count=64
mkfs.vfat /tmp/esp.img
mcopy -i /tmp/esp.img -s /tmp/efi/* ::
```

#### 4) Run QEMU AArch64 with UEFI firmware

Find firmware `.fd` files on your system (paths vary):

```bash
dpkg -L qemu-efi-aarch64 | grep -E '\.fd$|\.bin$' || true
ls /usr/share | grep -i -E 'aavmf|edk2|qemu' || true
```

Example using AAVMF (adjust paths):

```bash
cp /usr/share/AAVMF/AAVMF_VARS.fd /tmp/AAVMF_VARS.fd

qemu-system-aarch64 \
  -machine virt \
  -cpu cortex-a57 \
  -m 1024 \
  -drive if=pflash,format=raw,readonly=on,file=/usr/share/AAVMF/AAVMF_CODE.fd \
  -drive if=pflash,format=raw,file=/tmp/AAVMF_VARS.fd \
  -drive format=raw,file=/tmp/esp.img \
  -device usb-kbd -device usb-mouse \
  -display gtk
```

---

### Practical recommendation

- Use **OVMF x86_64** for quick iteration.
- Use **AArch64 QEMU** to validate ARM behavior.
- Use the **real Pi 5** for final HDMI + firmware input validation.

## Next improvements (optional)

If you want to go further, these are good next steps:

1. **On-screen text overlay**  
   Add a tiny bitmap font and draw mode/brush info directly into the framebuffer.

2. **Dirty-tile renderer**  
   Track which grid blocks change and only redraw those rectangles.

3. **BLT fallback renderer**  
   Use `Gop->Blt()` if pixel format is `PixelBltOnly`.

4. **Fixed-point solver**
   Avoid floating point entirely and use integer arithmetic.

If you want any of these upgrades, tell me which one matters most (performance, portability, or UI polish).
