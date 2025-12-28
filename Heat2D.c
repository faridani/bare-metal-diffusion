#include <Uefi.h>
#include <Base.h>

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
  BC_DIRICHLET_COLD = 0,
  BC_NEUMANN_INSULATED,
  BC_MIXED,
  BC_COUNT
} BOUNDARY_MODE;

typedef struct {
  EFI_GRAPHICS_PIXEL_FORMAT Fmt;
  EFI_PIXEL_BITMASK         Masks;
} PIXEL_PACKER;

typedef struct {
  UINT8 r, g, b;
} RGB8;

STATIC RGB8 gColorLut[256];

// ==================== Fixed-point formats ====================
// Temperature T: Q16.16 signed (INT32), range [0..1] -> [0..65536]
#define Q16_SHIFT 16
#define Q16_ONE   (1 << Q16_SHIFT)
#define Q16_HALF  (1 << (Q16_SHIFT-1))

// Conductivity k: Q0.16 unsigned (UINT16), range [0..1] -> [0..65535]
#define K_SHIFT 16
#define K_ONE   65535u

STATIC inline INT32 ClampI32(INT32 v, INT32 lo, INT32 hi) {
  if (v < lo) return lo;
  if (v > hi) return hi;
  return v;
}

STATIC inline UINT32 Scale8ToMask(UINT8 c, UINT32 mask) {
  if (mask == 0) return 0;

  UINT32 shift = 0;
  UINT32 tmp = mask;
  while ((tmp & 1u) == 0u) {
    tmp >>= 1;
    shift++;
    if (shift >= 32) return 0;
  }

  UINT32 bits = 0;
  while ((tmp & 1u) == 1u) {
    tmp >>= 1;
    bits++;
    if (bits >= 32) break;
  }
  if (bits == 0) return 0;

  UINT32 maxVal = (bits >= 32) ? 0xFFFFFFFFu : ((1u << bits) - 1u);
  UINT32 scaled = (((UINT32)c) * maxVal + 127u) / 255u;
  return (scaled << shift) & mask;
}

STATIC inline UINT32 PackPixel(const PIXEL_PACKER *P, UINT8 r, UINT8 g, UINT8 b) {
  switch (P->Fmt) {
    case PixelRedGreenBlueReserved8BitPerColor:
      return ((UINT32)r) | ((UINT32)g << 8) | ((UINT32)b << 16) | (0xFFu << 24);
    case PixelBlueGreenRedReserved8BitPerColor:
      return ((UINT32)b) | ((UINT32)g << 8) | ((UINT32)r << 16) | (0xFFu << 24);
    case PixelBitMask: {
      UINT32 out = 0;
      out |= Scale8ToMask(r, P->Masks.RedMask);
      out |= Scale8ToMask(g, P->Masks.GreenMask);
      out |= Scale8ToMask(b, P->Masks.BlueMask);
      if (P->Masks.ReservedMask != 0) out |= P->Masks.ReservedMask;
      return out;
    }
    default:
      return ((UINT32)b) | ((UINT32)g << 8) | ((UINT32)r << 16) | (0xFFu << 24);
  }
}

STATIC BOOLEAN TryReadKey(EFI_SYSTEM_TABLE *SystemTable, EFI_INPUT_KEY *OutKey) {
  EFI_STATUS st = SystemTable->ConIn->ReadKeyStroke(SystemTable->ConIn, OutKey);
  return !EFI_ERROR(st);
}

// -------------------- Viridis-like LUT --------------------
STATIC UINT8 LerpU8(UINT8 a, UINT8 b, float t) {
  float x = (1.0f - t) * (float)a + t * (float)b;
  if (x < 0) x = 0;
  if (x > 255) x = 255;
  return (UINT8)(x + 0.5f);
}

STATIC VOID BuildViridisLikeLut(VOID) {
  typedef struct { float t; UINT8 r,g,b; } Stop;
  STATIC CONST Stop stops[] = {
    {0.00f,  68,  1,  84},
    {0.25f,  59, 82, 139},
    {0.50f,  33,145, 140},
    {0.75f,  94,201,  98},
    {1.00f, 253,231,  37},
  };

  for (INT32 i = 0; i < 256; i++) {
    float t = (float)i / 255.0f;
    INT32 k = 0;
    while (k < (INT32)(sizeof(stops)/sizeof(stops[0])) - 2 && t > stops[k+1].t) k++;
    float t0 = stops[k].t;
    float t1 = stops[k+1].t;
    float u = (t1 > t0) ? ((t - t0) / (t1 - t0)) : 0.0f;
    if (u < 0) u = 0; if (u > 1) u = 1;

    gColorLut[i].r = LerpU8(stops[k].r, stops[k+1].r, u);
    gColorLut[i].g = LerpU8(stops[k].g, stops[k+1].g, u);
    gColorLut[i].b = LerpU8(stops[k].b, stops[k+1].b, u);
  }
}

STATIC inline void TempQ16_ToRGB(INT32 tQ16, UINT8 *r, UINT8 *g, UINT8 *b) {
  if (tQ16 < 0) tQ16 = 0;
  if (tQ16 > Q16_ONE) tQ16 = Q16_ONE;

  // idx = round(t*255)
  // tQ16 in [0..65536]
  INT32 idx = (INT32)((((INT64)tQ16) * 255 + Q16_HALF) >> Q16_SHIFT);
  idx = ClampI32(idx, 0, 255);

  *r = gColorLut[idx].r;
  *g = gColorLut[idx].g;
  *b = gColorLut[idx].b;
}

// -------------------- PDE boundary conditions --------------------
STATIC VOID ApplyBoundaryQ16(INT32 *T, INT32 NX, INT32 NY, BOUNDARY_MODE Mode) {
  if (Mode == BC_DIRICHLET_COLD) {
    for (INT32 i = 0; i < NX; i++) {
      T[i] = 0;
      T[(NY-1)*NX + i] = 0;
    }
    for (INT32 j = 0; j < NY; j++) {
      T[j*NX] = 0;
      T[j*NX + (NX-1)] = 0;
    }
    return;
  }

  if (Mode == BC_NEUMANN_INSULATED) {
    for (INT32 i = 1; i < NX-1; i++) {
      T[0*NX + i]      = T[1*NX + i];
      T[(NY-1)*NX + i] = T[(NY-2)*NX + i];
    }
    for (INT32 j = 1; j < NY-1; j++) {
      T[j*NX + 0]      = T[j*NX + 1];
      T[j*NX + (NX-1)] = T[j*NX + (NX-2)];
    }
    T[0] = T[1*NX + 1];
    T[NX-1] = T[1*NX + (NX-2)];
    T[(NY-1)*NX] = T[(NY-2)*NX + 1];
    T[(NY-1)*NX + (NX-1)] = T[(NY-2)*NX + (NX-2)];
    return;
  }

  // BC_MIXED
  for (INT32 j = 0; j < NY; j++) {
    T[j*NX + 0] = 0;
    T[j*NX + (NX-1)] = 0;
  }
  for (INT32 i = 1; i < NX-1; i++) {
    T[0*NX + i]      = T[1*NX + i];
    T[(NY-1)*NX + i] = T[(NY-2)*NX + i];
  }
  T[0] = 0;
  T[NX-1] = 0;
  T[(NY-1)*NX] = 0;
  T[(NY-1)*NX + (NX-1)] = 0;
}

// -------------------- Heat stamping (fixed-point) --------------------
STATIC VOID StampDiskQ16(INT32 *T, INT32 NX, INT32 NY, INT32 cx, INT32 cy, INT32 rad, INT32 valQ16) {
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
        INT32 *p = &T[j*NX + i];
        if (valQ16 > *p) *p = valQ16;
      }
    }
  }
}

STATIC VOID StampRectMaxQ16(INT32 *T, INT32 NX, INT32 NY, INT32 x0, INT32 y0, INT32 w, INT32 h, INT32 valQ16) {
  INT32 x1 = x0 + w - 1;
  INT32 y1 = y0 + h - 1;

  x0 = ClampI32(x0, 0, NX-1);
  y0 = ClampI32(y0, 0, NY-1);
  x1 = ClampI32(x1, 0, NX-1);
  y1 = ClampI32(y1, 0, NY-1);

  for (INT32 y = y0; y <= y1; y++) {
    INT32 *row = &T[y*NX];
    for (INT32 x = x0; x <= x1; x++) {
      if (valQ16 > row[x]) row[x] = valQ16;
    }
  }
}

// -------------------- Framebuffer drawing --------------------
STATIC VOID DrawRect(UINT32 *Fb, UINTN Width, UINTN Height, UINTN Ppsl,
                     UINTN x0, UINTN y0, UINTN w, UINTN h, UINT32 px) {
  if (x0 >= Width || y0 >= Height) return;

  UINTN x1 = x0 + w; if (x1 > Width) x1 = Width;
  UINTN y1 = y0 + h; if (y1 > Height) y1 = Height;

  for (UINTN y = y0; y < y1; y++) {
    UINT32 *row = Fb + y * Ppsl;
    for (UINTN x = x0; x < x1; x++) row[x] = px;
  }
}

STATIC VOID DrawCursor(UINT32 *Fb, UINTN Width, UINTN Height, UINTN Ppsl,
                       UINTN x, UINTN y, const PIXEL_PACKER *Packer) {
  UINT32 w = PackPixel(Packer, 255, 255, 255);
  DrawRect(Fb, Width, Height, Ppsl, (x > 2 ? x - 2 : 0), y, 5, 1, w);
  DrawRect(Fb, Width, Height, Ppsl, x, (y > 2 ? y - 2 : 0), 1, 5, w);
}

// -------------------- Minimal 8x8 font (footer + legend labels) --------------------
typedef struct { CHAR8 Ch; UINT8 Row[8]; } GLYPH8;

STATIC CONST GLYPH8 gFont8[] = {
  {' ', {0,0,0,0,0,0,0,0}},
  {'-', {0,0,0,0x7E,0,0,0,0}},
  {',', {0,0,0,0,0,0x18,0x18,0x30}},
  {'.', {0,0,0,0,0,0x18,0x18,0}},
  {':', {0,0x18,0x18,0,0,0x18,0x18,0}},

  {'0', {0x3C,0x66,0x6E,0x76,0x66,0x66,0x3C,0}},
  {'1', {0x18,0x38,0x18,0x18,0x18,0x18,0x3C,0}},
  {'2', {0x3C,0x66,0x06,0x0C,0x18,0x30,0x7E,0}},
  {'3', {0x3C,0x66,0x06,0x1C,0x06,0x66,0x3C,0}},
  {'4', {0x0C,0x1C,0x3C,0x6C,0x7E,0x0C,0x0C,0}},
  {'5', {0x7E,0x60,0x7C,0x06,0x06,0x66,0x3C,0}},
  {'6', {0x1C,0x30,0x60,0x7C,0x66,0x66,0x3C,0}},
  {'7', {0x7E,0x06,0x0C,0x18,0x30,0x30,0x30,0}},
  {'8', {0x3C,0x66,0x66,0x3C,0x66,0x66,0x3C,0}},
  {'9', {0x3C,0x66,0x66,0x3E,0x06,0x0C,0x38,0}},

  {'A', {0x18,0x3C,0x66,0x66,0x7E,0x66,0x66,0}},
  {'C', {0x3C,0x66,0x60,0x60,0x60,0x66,0x3C,0}},
  {'D', {0x78,0x6C,0x66,0x66,0x66,0x6C,0x78,0}},
  {'H', {0x66,0x66,0x66,0x7E,0x66,0x66,0x66,0}},
  {'I', {0x3C,0x18,0x18,0x18,0x18,0x18,0x3C,0}},
  {'L', {0x60,0x60,0x60,0x60,0x60,0x60,0x7E,0}},
  {'M', {0x63,0x77,0x7F,0x6B,0x63,0x63,0x63,0}},
  {'O', {0x3C,0x66,0x66,0x66,0x66,0x66,0x3C,0}},
  {'P', {0x7C,0x66,0x66,0x7C,0x60,0x60,0x60,0}},
  {'R', {0x7C,0x66,0x66,0x7C,0x78,0x6C,0x66,0}},
  {'S', {0x3C,0x66,0x60,0x3C,0x06,0x66,0x3C,0}},
  {'T', {0x7E,0x18,0x18,0x18,0x18,0x18,0x18,0}},
  {'V', {0x66,0x66,0x66,0x66,0x66,0x3C,0x18,0}},
  {'E', {0x7E,0x60,0x60,0x7C,0x60,0x60,0x7E,0}},
  {'B', {0x7C,0x66,0x66,0x7C,0x66,0x66,0x7C,0}},
  {'N', {0x66,0x76,0x7E,0x7E,0x6E,0x66,0x66,0}},
  {'Y', {0x66,0x66,0x66,0x3C,0x18,0x18,0x18,0}},
};

STATIC const UINT8* FindGlyph8(CHAR8 ch) {
  if (ch >= 'a' && ch <= 'z') ch = (CHAR8)(ch - 'a' + 'A');
  for (UINTN i = 0; i < (sizeof(gFont8)/sizeof(gFont8[0])); i++) {
    if (gFont8[i].Ch == ch) return gFont8[i].Row;
  }
  // unknown -> blank
  return gFont8[0].Row;
}

STATIC VOID DrawChar8(UINT32 *Fb, UINTN Width, UINTN Height, UINTN Ppsl,
                      UINTN x, UINTN y, CHAR8 ch, UINT32 fg) {
  const UINT8 *rows = FindGlyph8(ch);
  for (UINTN ry = 0; ry < 8; ry++) {
    UINTN py = y + ry;
    if (py >= Height) break;
    UINT8 bits = rows[ry];
    UINT32 *row = Fb + py * Ppsl;
    for (UINTN rx = 0; rx < 8; rx++) {
      UINTN px = x + rx;
      if (px >= Width) break;
      if (bits & (0x80u >> rx)) row[px] = fg;
    }
  }
}

STATIC VOID DrawString8(UINT32 *Fb, UINTN Width, UINTN Height, UINTN Ppsl,
                        UINTN x, UINTN y, const CHAR8 *s, UINT32 fg) {
  UINTN cx = x;
  for (; *s; s++) {
    if (*s == '\n') { y += 10; cx = x; continue; }
    DrawChar8(Fb, Width, Height, Ppsl, cx, y, *s, fg);
    cx += 8;
  }
}

STATIC VOID DrawFooter(UINT32 *Fb, UINTN Width, UINTN Height, UINTN Ppsl, const PIXEL_PACKER *Packer) {
  const CHAR8 *msg = "Dec 27, 2025 - Bare Metal Parabolic PDE Solver";
  UINTN padX = 12, padY = 6, textH = 8;
  UINTN boxH = textH + padY * 2;
  if (Height < boxH + 2) return;

  UINTN y0 = Height - boxH;
  UINT32 bg = PackPixel(Packer, 10, 10, 10);
  UINT32 fg = PackPixel(Packer, 240, 240, 240);

  DrawRect(Fb, Width, Height, Ppsl, 0, y0, Width, boxH, bg);
  DrawString8(Fb, Width, Height, Ppsl, padX, y0 + padY, msg, fg);
}

STATIC VOID DrawLegendWithLabels(UINT32 *Fb, UINTN Width, UINTN Height, UINTN Ppsl, const PIXEL_PACKER *Packer) {
  UINTN barW = (Width > 200) ? 24 : 16;
  UINTN barH = (Height > 240) ? (Height / 2) : (Height * 2 / 3);

  UINTN footerH = 8 + 6*2;
  if (Height > footerH + 24 && barH > Height - footerH - 24) barH = Height - footerH - 24;
  if (barH < 40) barH = 40;

  UINTN labelW = 72;
  UINTN x0 = (Width > (barW + 12 + labelW + 12)) ? (Width - barW - 12 - labelW - 12) : 0;
  UINTN y0 = 12;

  UINT32 panel  = PackPixel(Packer, 20, 20, 20);
  UINT32 border = PackPixel(Packer, 220, 220, 220);
  UINT32 text   = PackPixel(Packer, 240, 240, 240);

  DrawRect(Fb, Width, Height, Ppsl, x0-6, y0-6, barW + 12 + labelW + 12, barH + 12, panel);

  for (UINTN y = 0; y < barH; y++) {
    // t in [0..1] -> idx
    INT32 tQ16 = (INT32)(((INT64)(barH - 1 - (INT32)y) * Q16_ONE) / (INT32)((barH > 1) ? (barH - 1) : 1));
    UINT8 r,g,b;
    TempQ16_ToRGB(tQ16, &r, &g, &b);
    UINT32 px = PackPixel(Packer, r, g, b);
    DrawRect(Fb, Width, Height, Ppsl, x0, y0 + y, barW, 1, px);
  }

  DrawRect(Fb, Width, Height, Ppsl, x0-1, y0-1, barW+2, 1, border);
  DrawRect(Fb, Width, Height, Ppsl, x0-1, y0+barH, barW+2, 1, border);
  DrawRect(Fb, Width, Height, Ppsl, x0-1, y0-1, 1, barH+2, border);
  DrawRect(Fb, Width, Height, Ppsl, x0+barW, y0-1, 1, barH+2, border);

  UINTN lx = x0 + barW + 10;
  DrawString8(Fb, Width, Height, Ppsl, lx, y0 + 0,          "HOT  1.0", text);
  DrawString8(Fb, Width, Height, Ppsl, lx, y0 + barH/2 - 4, "MID  0.5", text);
  DrawString8(Fb, Width, Height, Ppsl, lx, y0 + barH - 8,   "COLD 0.0", text);
}

// -------------------- Pointer handling --------------------
typedef struct {
  BOOLEAN HasAbs;
  BOOLEAN HasRel;
  EFI_ABSOLUTE_POINTER_PROTOCOL *Abs;
  EFI_SIMPLE_POINTER_PROTOCOL   *Rel;
  INT32 X;
  INT32 Y;
  INT32 AbsMinX, AbsMaxX;
  INT32 AbsMinY, AbsMaxY;
  BOOLEAN LastAbsValid;
  INT32   LastAbsX;
  INT32   LastAbsY;
  INT32 RelScale;
} POINTER_STATE;

STATIC VOID InitPointer(POINTER_STATE *P, EFI_SYSTEM_TABLE *SystemTable, UINTN Width, UINTN Height) {
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
  if (!EFI_ERROR(st) && P->Rel) P->HasRel = TRUE;

  SystemTable->ConIn->Reset(SystemTable->ConIn, FALSE);
}

STATIC BOOLEAN PollPointer(POINTER_STATE *P, UINTN Width, UINTN Height, BOOLEAN *OutPressed) {
  BOOLEAN absAvail = FALSE, absPressed = FALSE, absMoved = FALSE;
  INT32 absX = P->X, absY = P->Y;

  BOOLEAN relAvail = FALSE, relPressed = FALSE, relMoved = FALSE;
  INT32 dx = 0, dy = 0;

  if (P->HasAbs && P->Abs) {
    EFI_ABSOLUTE_POINTER_STATE st;
    EFI_STATUS e = P->Abs->GetState(P->Abs, &st);
    if (!EFI_ERROR(e)) {
      absAvail = TRUE;
      INT32 ax = (INT32)st.CurrentX;
      INT32 ay = (INT32)st.CurrentY;

      INT32 rx = P->AbsMaxX - P->AbsMinX; if (rx <= 0) rx = 1;
      INT32 ry = P->AbsMaxY - P->AbsMinY; if (ry <= 0) ry = 1;

      absX = (INT32)((((INT64)(ax - P->AbsMinX)) * (INT64)((Width  > 0) ? (Width  - 1) : 0)) / rx);
      absY = (INT32)((((INT64)(ay - P->AbsMinY)) * (INT64)((Height > 0) ? (Height - 1) : 0)) / ry);

      absX = ClampI32(absX, 0, (INT32)Width  - 1);
      absY = ClampI32(absY, 0, (INT32)Height - 1);

      absPressed = (st.ActiveButtons != 0);

      if (!P->LastAbsValid || absX != P->LastAbsX || absY != P->LastAbsY) absMoved = TRUE;
      P->LastAbsValid = TRUE;
      P->LastAbsX = absX;
      P->LastAbsY = absY;
    }
  }

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
    moved = (absX != P->X) || (absY != P->Y);
    P->X = absX; P->Y = absY;
  } else {
    if (relAvail && relMoved) {
      INT32 nx = ClampI32(P->X + dx, 0, (INT32)Width  - 1);
      INT32 ny = ClampI32(P->Y + dy, 0, (INT32)Height - 1);
      moved = (nx != P->X) || (ny != P->Y);
      P->X = nx; P->Y = ny;
    } else if (absAvail && absMoved) {
      moved = (absX != P->X) || (absY != P->Y);
      P->X = absX; P->Y = absY;
    }
  }

  *OutPressed = pressed;
  return moved || pressed;
}

// -------------------- Heatsink geometry + face K precompute --------------------
typedef struct {
  INT32 baseX0, baseX1;
  INT32 baseY0, baseY1;
} HEATSINK_GEOM;

STATIC VOID BuildHeatsinkCombMask_U16(UINT16 *Kcell, UINT8 *Mat, INT32 NX, INT32 NY, HEATSINK_GEOM *G) {
  // V3 ratio: 1:100
  const UINT16 k_air = (UINT16)(K_ONE / 100); // ~655
  const UINT16 k_cu  = (UINT16)K_ONE;         // 65535

  for (INT32 j = 0; j < NY; j++) {
    for (INT32 i = 0; i < NX; i++) {
      Mat[j*NX + i] = 0;
      Kcell[j*NX + i] = k_air;
    }
  }

  INT32 marginX = NX / 8;
  INT32 baseW = NX - 2*marginX;
  INT32 baseH = NY / 10;
  INT32 baseX0 = marginX;
  INT32 baseX1 = baseX0 + baseW - 1;

  INT32 baseY1 = NY - 10;
  INT32 baseY0 = baseY1 - baseH + 1;
  if (baseY0 < 0) baseY0 = 0;

  if (G) {
    G->baseX0 = baseX0; G->baseX1 = baseX1;
    G->baseY0 = baseY0; G->baseY1 = baseY1;
  }

  // Copper base
  for (INT32 j = baseY0; j <= baseY1; j++) {
    for (INT32 i = baseX0; i <= baseX1; i++) {
      Mat[j*NX + i] = 1;
      Kcell[j*NX + i] = k_cu;
    }
  }

  // Fins
  INT32 finH  = NY / 3;
  INT32 finY0 = baseY0 - finH;
  INT32 finY1 = baseY0;
  if (finY0 < 2) finY0 = 2;

  INT32 finCount = 28; // more fins looks nicer at 2x grid
  INT32 gap = baseW / finCount;
  if (gap < 6) gap = 6;

  INT32 finW = gap / 2;
  if (finW < 3) finW = 3;

  for (INT32 f = 0; f < finCount; f++) {
    INT32 cx = baseX0 + f * gap + gap/2;
    INT32 x0 = cx - finW/2;
    INT32 x1 = x0 + finW - 1;
    x0 = ClampI32(x0, baseX0, baseX1);
    x1 = ClampI32(x1, baseX0, baseX1);

    for (INT32 j = finY0; j <= finY1; j++) {
      for (INT32 i = x0; i <= x1; i++) {
        Mat[j*NX + i] = 1;
        Kcell[j*NX + i] = k_cu;
      }
    }
  }

  // Die block under base
  INT32 dieW = baseW / 6;
  INT32 dieH = baseH / 2;
  INT32 dieX0 = (NX/2) - dieW/2;
  INT32 dieX1 = dieX0 + dieW - 1;
  INT32 dieY0 = baseY1 + 1;
  INT32 dieY1 = dieY0 + dieH - 1;
  if (dieY1 >= NY) dieY1 = NY - 1;

  for (INT32 j = dieY0; j <= dieY1; j++) {
    for (INT32 i = dieX0; i <= dieX1; i++) {
      Mat[j*NX + i] = 1;
      Kcell[j*NX + i] = k_cu;
    }
  }
}

// Precompute face K in Q0.16 (UINT16) using harmonic mean.
// Uses integer math: k_face = (2*k0*k1)/(k0+k1)
STATIC UINT16 KFaceHarmonic_U16(UINT16 k0, UINT16 k1) {
  UINT32 denom = (UINT32)k0 + (UINT32)k1;
  if (denom == 0) return 0;
  UINT32 num = 2u * (UINT32)k0 * (UINT32)k1;
  return (UINT16)(num / denom);
}

STATIC VOID PrecomputeFaceK_U16(const UINT16 *Kcell, UINT16 *Kx, UINT16 *Ky, INT32 NX, INT32 NY) {
  for (INT32 j = 0; j < NY; j++) {
    INT32 row = j*NX;
    for (INT32 i = 0; i < NX; i++) {
      INT32 idx = row + i;
      if (i < NX-1) Kx[idx] = KFaceHarmonic_U16(Kcell[idx], Kcell[idx+1]);
      else Kx[idx] = 0;
      if (j < NY-1) Ky[idx] = KFaceHarmonic_U16(Kcell[idx], Kcell[idx+NX]);
      else Ky[idx] = 0;
    }
  }
}

// ==================== Main ====================
EFI_STATUS EFIAPI UefiMain(IN EFI_HANDLE ImageHandle, IN EFI_SYSTEM_TABLE *SystemTable) {
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

  PIXEL_PACKER Packer;
  Packer.Fmt = Info->PixelFormat;
  if (Packer.Fmt == PixelBltOnly) {
    Print(L"Error: GOP PixelFormat is PixelBltOnly.\n");
    Print(L"This demo requires direct framebuffer access.\n");
    return EFI_UNSUPPORTED;
  }
  if (Packer.Fmt == PixelBitMask) Packer.Masks = Info->PixelInformation;
  else SetMem(&Packer.Masks, sizeof(Packer.Masks), 0);

  BuildViridisLikeLut();

  UINT32 *Fb = (UINT32*)(UINTN)Gop->Mode->FrameBufferBase;

  // ======= 2x grid size =======
  const INT32 NX = 520;
  const INT32 NY = 440;

  // Temperatures in Q16.16
  INT32 *A = AllocateZeroPool(sizeof(INT32) * NX * NY);
  INT32 *B = AllocateZeroPool(sizeof(INT32) * NX * NY);

  // Conductivity in Q0.16 (cell + faces)
  UINT16 *Kcell = AllocateZeroPool(sizeof(UINT16) * NX * NY);
  UINT16 *Kx    = AllocateZeroPool(sizeof(UINT16) * NX * NY);
  UINT16 *Ky    = AllocateZeroPool(sizeof(UINT16) * NX * NY);

  UINT8  *Mat   = AllocateZeroPool(sizeof(UINT8)  * NX * NY);

  if (!A || !B || !Kcell || !Kx || !Ky || !Mat) {
    Print(L"Out of memory\n");
    if (A) FreePool(A); if (B) FreePool(B);
    if (Kcell) FreePool(Kcell); if (Kx) FreePool(Kx); if (Ky) FreePool(Ky);
    if (Mat) FreePool(Mat);
    return EFI_OUT_OF_RESOURCES;
  }

  HEATSINK_GEOM G;
  BuildHeatsinkCombMask_U16(Kcell, Mat, NX, NY, &G);
  PrecomputeFaceK_U16(Kcell, Kx, Ky, NX, NY);

  // baseR in Q16.16. 0.20 -> ~13107
  const INT32 baseR_Q16 = (INT32)(0.20f * (float)Q16_ONE + 0.5f);

  // Heat sources (Q16.16)
  const INT32 heatTempQ = Q16_ONE;

  INT32 baseW = G.baseX1 - G.baseX0 + 1;
  INT32 baseH = G.baseY1 - G.baseY0 + 1;

  INT32 srcH  = ClampI32(baseH / 2, 2, baseH);
  INT32 srcY0 = G.baseY1 - srcH + 1;
  INT32 srcW  = ClampI32(baseW / 8, 10, baseW / 3);
  INT32 gap   = ClampI32(baseW / 12, 6, baseW / 4);

  INT32 mid = (G.baseX0 + G.baseX1) / 2;
  INT32 s0x0 = mid - (srcW/2) - (srcW + gap);
  INT32 s1x0 = mid - (srcW/2);
  INT32 s2x0 = mid - (srcW/2) + (srcW + gap);

  if (s0x0 < G.baseX0) s0x0 = G.baseX0;
  if (s2x0 + srcW - 1 > G.baseX1) s2x0 = G.baseX1 - srcW + 1;

  // Brush
  INT32 brushRad = NX / 70;
  INT32 brushTempQ = Q16_ONE; // default 1.0

  BOUNDARY_MODE bc = BC_DIRICHLET_COLD;
  BOOLEAN Paused = FALSE;

  // ---- Rendering scaling ----
  UINTN cellW = Width / (UINTN)NX;
  UINTN cellH = Height / (UINTN)NY;
  if (cellW < 1) cellW = 1;
  if (cellH < 1) cellH = 1;

  UINTN drawW = (UINTN)NX * cellW;
  UINTN drawH = (UINTN)NY * cellH;
  if (drawW > Width)  drawW = Width;
  if (drawH > Height) drawH = Height;

  // Adaptive drawSkip so rendering doesn't explode with 2x grid
  UINTN drawSkip = 1;
  // If output is big, subsample more
  if (Width * Height > 1280u * 720u)  drawSkip = 2;
  if (Width * Height > 1920u * 1080u) drawSkip = 3;
  if (Width * Height > 2560u * 1440u) drawSkip = 4;

  POINTER_STATE Ptr;
  InitPointer(&Ptr, SystemTable, Width, Height);

  UINT32 bg = PackPixel(&Packer, 0, 0, 0);
  DrawRect(Fb, Width, Height, Ppsl, 0, 0, Width, Height, bg);

  BOOLEAN dirty = TRUE;

  while (TRUE) {
    EFI_INPUT_KEY Key;
    while (TryReadKey(SystemTable, &Key)) {
      if (Key.ScanCode == SCAN_ESC) goto done;

      if (Key.UnicodeChar == L' ') { Paused = !Paused; dirty = TRUE; }
      else if (Key.UnicodeChar == L'r' || Key.UnicodeChar == L'R') {
        SetMem(A, sizeof(INT32)*NX*NY, 0);
        SetMem(B, sizeof(INT32)*NX*NY, 0);
        dirty = TRUE;
      } else if (Key.UnicodeChar == L'c' || Key.UnicodeChar == L'C') {
        SetMem(A, sizeof(INT32)*NX*NY, 0);
        dirty = TRUE;
      } else if (Key.UnicodeChar == L'b' || Key.UnicodeChar == L'B') {
        bc = (BOUNDARY_MODE)((bc + 1) % BC_COUNT);
        dirty = TRUE;
      } else if (Key.UnicodeChar == L'+' || Key.UnicodeChar == L'=') {
        brushRad = ClampI32(brushRad + 2, 2, NX/8);
        dirty = TRUE;
      } else if (Key.UnicodeChar == L'-' || Key.UnicodeChar == L'_') {
        brushRad = ClampI32(brushRad - 2, 2, NX/8);
        dirty = TRUE;
      } else if (Key.UnicodeChar == L'1') { brushTempQ = (INT32)(0.5f * (float)Q16_ONE + 0.5f); dirty = TRUE; }
      else if (Key.UnicodeChar == L'2') { brushTempQ = (INT32)(0.8f * (float)Q16_ONE + 0.5f); dirty = TRUE; }
      else if (Key.UnicodeChar == L'3') { brushTempQ = Q16_ONE; dirty = TRUE; }
    }

    // Pointer
    BOOLEAN pressed = FALSE;
    BOOLEAN ptrEvent = PollPointer(&Ptr, Width, Height, &pressed);

    INT32 gx = (INT32)((((INT64)Ptr.X) * NX) / (INT32)((drawW > 0) ? drawW : 1));
    INT32 gy = (INT32)((((INT64)Ptr.Y) * NY) / (INT32)((drawH > 0) ? drawH : 1));
    gx = ClampI32(gx, 0, NX-1);
    gy = ClampI32(gy, 0, NY-1);

    if (pressed) {
      StampDiskQ16(A, NX, NY, gx, gy, brushRad, brushTempQ);
      dirty = TRUE;
    } else if (ptrEvent) {
      dirty = TRUE;
    }

    // Simulation
    if (!Paused) {
      // 3 rectangular sources on base bottom
      StampRectMaxQ16(A, NX, NY, s0x0, srcY0, srcW, srcH, heatTempQ);
      StampRectMaxQ16(A, NX, NY, s1x0, srcY0, srcW, srcH, heatTempQ);
      StampRectMaxQ16(A, NX, NY, s2x0, srcY0, srcW, srcH, heatTempQ);

      // Hot loop: all fixed-point, precomputed face k
      for (INT32 j = 1; j < NY-1; j++) {
        INT32 row = j*NX;
        for (INT32 i = 1; i < NX-1; i++) {
          INT32 idx = row + i;

          INT32 tC = A[idx];
          INT32 tR = A[idx + 1];
          INT32 tL = A[idx - 1];
          INT32 tD = A[idx + NX];
          INT32 tU = A[idx - NX];

          // flux = k_face * (tN - tC)
          // k_face is Q0.16, deltaT is Q16.16
          // flux becomes Q16.16 after shifting down 16
          INT64 flux_r = ((INT64)Kx[idx]     * (INT64)(tR - tC)) >> 16;
          INT64 flux_l = ((INT64)Kx[idx - 1] * (INT64)(tL - tC)) >> 16;
          INT64 flux_d = ((INT64)Ky[idx]     * (INT64)(tD - tC)) >> 16;
          INT64 flux_u = ((INT64)Ky[idx - NX]* (INT64)(tU - tC)) >> 16;

          INT64 flux_sum = flux_r + flux_l + flux_d + flux_u; // Q16.16

          // dt term: baseR_Q16 (Q16.16) * flux_sum (Q16.16) -> Q32.32 -> shift 16 -> Q16.16
          INT64 delta = ((INT64)baseR_Q16 * flux_sum) >> 16;

          INT64 out = (INT64)tC + delta;

          // clamp to [0..1]
          if (out < 0) out = 0;
          if (out > Q16_ONE) out = Q16_ONE;

          B[idx] = (INT32)out;
        }
      }

      ApplyBoundaryQ16(B, NX, NY, bc);

      INT32 *Tmp = A; A = B; B = Tmp;
      dirty = TRUE;
    }

    // Render
    if (dirty) {
      for (INT32 j = 0; j < NY; j += (INT32)drawSkip) {
        for (INT32 i = 0; i < NX; i += (INT32)drawSkip) {
          INT32 tQ = A[j*NX + i];

          UINT8 rr, gg, bb;
          TempQ16_ToRGB(tQ, &rr, &gg, &bb);

          // Copper tint (visual only)
          if (Mat[j*NX + i] == 1) {
            rr = (UINT8)ClampI32((INT32)rr + 10, 0, 255);
            gg = (UINT8)((UINT32)gg * 240u / 255u);
            bb = (UINT8)((UINT32)bb * 220u / 255u);
          } else {
            rr = (UINT8)((UINT32)rr * 230u / 255u);
            gg = (UINT8)((UINT32)gg * 230u / 255u);
            bb = (UINT8)((UINT32)bb * 230u / 255u);
          }

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

      DrawCursor(Fb, Width, Height, Ppsl, (UINTN)Ptr.X, (UINTN)Ptr.Y, &Packer);
      DrawLegendWithLabels(Fb, Width, Height, Ppsl, &Packer);
      DrawFooter(Fb, Width, Height, Ppsl, &Packer);

      dirty = FALSE;
    }

    // gBS->Stall(4000);
  }

done:
  FreePool(A); FreePool(B);
  FreePool(Kcell); FreePool(Kx); FreePool(Ky);
  FreePool(Mat);
  Print(L"Exit.\n");
  return EFI_SUCCESS;
}
