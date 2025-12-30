qemu-system-aarch64 -accel tcg \
  -M virt -cpu cortex-a76 -m 2048 \
  -vga none -device ramfb \
  -display sdl \
  -serial stdio -monitor none \
  -no-reboot -no-shutdown \
  -d guest_errors \
  -kernel kernel.elf
