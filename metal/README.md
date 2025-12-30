# Heat2D (bare metal, Raspberry Pi 5)

A Circle-based, bare-metal version of the Heat2D demo for the Raspberry Pi 5 (BCM2712). It renders directly to HDMI, uses Circle's higher-resolution `Font12x22` for a more elegant HUD, and lets you switch between multiple color palettes at runtime.

## Features
- Explicit 2D heat diffusion solver with a persistent center heat source.
- Double-buffered rendering via Circle's `C2DGraphics`.
- Elegant text: Circle's `Font12x22` (larger, smoother than the legacy 8x16 font).
- Runtime palette switching (`C`), reset (`Space`), and exit (`Esc`).

## Build (cross-compile)

### Prerequisites (Ubuntu/Debian)
- AArch64 bare-metal toolchain (`aarch64-none-elf-gcc`/`g++`).
- Circle (Pi 5 capable, current `master` has BCM2712 support).

Example setup on Ubuntu:
```bash
sudo apt update
sudo apt install -y gcc-aarch64-none-elf g++-aarch64-none-elf make git

# Grab Circle next to this repository
cd ..
git clone --recursive https://github.com/rsta2/circle.git
# Build Circle once (from its repo root)
cd circle
./makeall RASPPI=5
```

### Build the demo
```bash
# from this repo root
git submodule update --init --recursive  # only needed if you keep Circle as a submodule
cd metal
make CIRCLEHOME=../circle RASPPI=5
```
This produces `kernel8.img` (or `kernel8-Heat2D.img` depending on your Circle version) in `metal/`.

### Deploy to an SD card (real Pi 5)
1. Format a FAT32 boot partition.
2. Copy the produced `kernel8.img` to the partition root. On Pi 5 firmware you can also name it `kernel_2712.img`; match the name you set in `config.txt`.
3. Minimal `config.txt` snippet:
   ```ini
   arm_64bit=1
   enable_uart=1
   kernel=kernel8.img
   disable_commandline_tags=1
   ```
4. Insert the card into the Pi 5, attach HDMI, and connect to the UART header (GPIO 14/15) or a USB‑serial adapter for input.

Controls:
- `Space` — reset the simulation (send over the serial console or QEMU `-serial stdio`).
- `C` — cycle color palettes (Fiery → Ocean → Magenta).
- `Esc` — halt.
- Palettes also auto-cycle every ~20 seconds if no input arrives, so the demo remains dynamic in kiosk setups.

## Emulation on a host
A recent QEMU (8.2 or newer) includes a `raspi5b` machine model. You need a BCM2712 device tree (shipped with QEMU or the Raspberry Pi firmware repo).

```bash
qemu-system-aarch64 \
  -M raspi5b -cpu cortex-a76 -m 2048 \
  -serial stdio -display sdl \
  -kernel kernel8.img \
  -dtb /usr/share/qemu-efi-bcm2712/bcm2712-rpi-5-b.dtb \
  -append "console=ttyAMA0" \
  -no-reboot
```

Tips:
- On Windows, install QEMU via WSL or MSYS2; the command line is identical. Ensure the DTB path matches your install (`where bcm2712-rpi-5-b.dtb`).
- If your QEMU build lacks `raspi5b`, build QEMU from source with `--target-list=aarch64-softmmu`.

## Cross-compiling on Windows
- Use [MSYS2](https://www.msys2.org/) and install `aarch64-elf-gcc` with `pacman -S mingw-w64-x86_64-aarch64-none-elf-gcc`.
- Clone Circle and this repo side by side (e.g., `C:\work\circle` and `C:\work\bare-metal-diffusion`).
- Build with: `make -C C:/work/bare-metal-diffusion/metal CIRCLEHOME=C:/work/circle RASPPI=5` from an MSYS2 shell.

## Notes
- The simulation uses Dirichlet cold boundaries and a small cooling term to keep temperatures bounded.
- Fonts and rendering use Circle primitives only; no UEFI dependencies remain.
- The `.c` file is compiled as C++17 by Circle; keep the extension to mirror the original filename while benefiting from Circle's class-based APIs.
