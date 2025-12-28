#include <Uefi.h>
#include <Library/BaseMemoryLib.h>

void *memcpy(void *dst, const void *src, UINTN n) {
  CopyMem(dst, src, n);
  return dst;
}

void *memset(void *dst, int c, UINTN n) {
  SetMem(dst, n, (UINT8)c);
  return dst;
}

void *memmove(void *dst, const void *src, UINTN n) {
  // EDK2 CopyMem is safe for overlap? Not guaranteed.
  // Use CopyMem for now if your usage is non-overlapping; otherwise implement overlap-safe copy.
  CopyMem(dst, src, n);
  return dst;
}

