# Heat2D Bare-Metal (Raspberry Pi 5)

This folder contains a Circle-based Raspberry Pi 5 port of the Heat2D demo (`Heat2D.cpp` is the bare-metal rewrite of the original `Heat2D.c`). The simulation now:

- Models airflow and convection around the heat sink.
- Runs more iterations per frame for faster convergence.
- Includes a working USB mouse for heat placement/removal.
- Uses a proportional FreeSans font for clearer UI text.
- Lets you cycle through multiple color palettes at runtime.

Controls:

- **Left mouse button:** inject heat at the cursor.
- **Right mouse button:** cool the current cell.
- **C key:** switch to the next color palette.
- **R key:** reset the grid to ambient/sink defaults.

## Building (Ubuntu)

1. Install build dependencies:
   ```bash
   sudo apt-get update
   sudo apt-get install -y git cmake gcc-aarch64-linux-gnu g++-aarch64-linux-gnu make
   ```
2. Clone Circle next to this repository (expected layout: `../circle` relative to `metal/`):
   ```bash
   git clone https://github.com/rsta2/circle.git ../circle
   ```
3. Build Circle for Raspberry Pi 5:
   ```bash
   cd ../circle
   ./configure --board=rpi5
   make -j$(nproc)
   ```
4. Build the demo:
   ```bash
   cd ../bare-metal-diffusion/metal
   make
   ```
   The output `kernel8.img` can be copied to a FAT-formatted boot partition on a microSD card for the Pi 5.

## Building (Windows)

- Use **WSL2** for the smoothest experience and follow the Ubuntu steps above.
- Alternatively, install the ARM GNU toolchain from Arm Developer, ensure `aarch64-none-elf-gcc` is in `%PATH%`, and build Circle with a MinGW or MSYS2 shell:
  ```bash
  git clone https://github.com/rsta2/circle.git ..\\circle
  cd ..\\circle
  ./configure --board=rpi5
  make -j%NUMBER_OF_PROCESSORS%
  cd ..\\bare-metal-diffusion\\metal
  make
  ```

## Emulation (Windows or Ubuntu)

You can validate the build without hardware using QEMU's Raspberry Pi 5 model:

```bash
qemu-system-aarch64 \
  -M raspi5 \
  -kernel kernel8.img \
  -m 2G \
  -serial stdio \
  -display sdl \
  -usb -device usb-kbd -device usb-mouse
```

Notes:
- The SDL window shows the framebuffer; `-usb` enables keyboard/mouse for interaction.
- QEMU's Pi 5 support is evolving. If you see missing device warnings, try the latest QEMU build from source.

## Cross-compilation Details

- Target triple: **aarch64-none-elf** (no Linux).
- CPU tuning: `-mcpu=cortex-a76 -march=armv8.2-a+crypto` matches the Pi 5's cores.
- The provided `Makefile` relies on Circle's `Rules.mk`; adjust `CIRCLEHOME` if your clone lives elsewhere.

## Preparing the SD Card

1. Format a microSD card with a small FAT partition.
2. Copy Circle's firmware files (`config.txt`, `start4.elf`, `fixup4.dat`) plus the built `kernel8.img` to the partition.
3. Insert the card into the Raspberry Pi 5 and power on. The simulation should launch directly to the framebuffer.

## Troubleshooting

- **Mouse not moving:** ensure `-device usb-mouse` is present in QEMU; on hardware, try a different USB port or hub.
- **Blank display:** double-check `config.txt` points to `kernel8.img` and that the firmware files match Circle's build.
- **Slow emulation:** reduce QEMU resolution or drop `-display sdl` to `-display gtk` if SDL is slow on your host.
