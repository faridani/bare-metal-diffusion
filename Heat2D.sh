# ====== 0) Go to your edk2 workspace ======
cd ~/Desktop/edk2

# ====== 1) Set up the EDK2 build environment ======
. edksetup.sh
export GCC5_AARCH64_PREFIX=aarch64-linux-gnu-

# (If you haven't installed the cross toolchain yet)
# sudo apt update
# sudo apt install gcc-aarch64-linux-gnu g++-aarch64-linux-gnu make uuid-dev python3-distutils

# ====== 2) Build your UEFI app (DEBUG) ======
build -a AARCH64 -t GCC5 -b DEBUG -p Heat2D/Heat2D.dsc

# Your output EFI will be here:
# Build/Heat2DPlatform/DEBUG_GCC5/AARCH64/Heat2D.efi


# ====== 3) Put the EFI into your QEMU "directory FAT" and set it to autoboot ======
mkdir -p runfs/EFI/BOOT
cp Build/Heat2DPlatform/DEBUG_GCC5/AARCH64/Heat2D.efi runfs/EFI/BOOT/BOOTAA64.EFI

# Auto-run script (UEFI shell runs this at boot)
cat > runfs/startup.nsh <<'EOF'
fs0:
\EFI\BOOT\BOOTAA64.EFI
EOF
sed -i 's/\r$//' runfs/startup.nsh


# ====== 4) One-time: ensure you have a writable VARS file for AAVMF ======
# (Do this once; it is safe to re-run)
if [ ! -f AAVMF_VARS.fd ]; then
  cp /usr/share/AAVMF/AAVMF_VARS.fd ./AAVMF_VARS.fd
fi


# ====== 5) Run in QEMU (graphics framebuffer + good mouse) ======
# -device ramfb gives direct framebuffer GOP (works with your demo)
# -device usb-tablet fixes mouse clicking/position
qemu-system-aarch64 \
  -display sdl \
  -machine virt \
  -cpu cortex-a72 \
  -m 1024 \
  -device ramfb \
  -device qemu-xhci \
  -device usb-kbd \
  -device usb-tablet \
  -drive if=pflash,format=raw,readonly=on,file=/usr/share/AAVMF/AAVMF_CODE.fd \
  -drive if=pflash,format=raw,file=AAVMF_VARS.fd \
  -drive file=fat:rw:runfs,format=raw

