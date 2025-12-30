These instructions are for building the project on an Ubuntu-based system (including WSL or a VM).

## Prerequisites

Install the required packages:

```bash
sudo apt update
sudo apt install -y \
  build-essential git python3 \
  gcc-aarch64-linux-gnu g++-aarch64-linux-gnu \
  nasm iasl uuid-dev \
  qemu-system-aarch64 qemu-efi-aarch64 \
  mtools dosfstools
```

## Build instructions (EDK2)

### 1) Clone EDK2

```bash
mkdir UEFI-app
cd UEFI-app
git clone https://github.com/tianocore/edk2.git
cd edk2
git submodule update --init --recursive
```

### 2) Add the Heat2D application to the EDK2 workspace

```bash
cd ..
git clone https://github.com/faridani/uefi-2D-heat-diffusion.git
mv ./uefi-2D-heat-diffusion ./edk2/Heat2D
```

### 3) Build BaseTools and set up the environment

```bash
cd edk2
make -C BaseTools
. edksetup.sh
export GCC5_AARCH64_PREFIX=aarch64-linux-gnu-
```

### 4) Build the UEFI application (DEBUG)

```bash
build -a AARCH64 -t GCC5 -b DEBUG -p Heat2D/Heat2D.dsc
```

The output EFI will be located at `Build/Heat2DPlatform/DEBUG_GCC5/AARCH64/Heat2D.efi`.

> **Tip:** If you see `gcc: error: unrecognized command-line option '-mlittle-endian'` while building, it means the AArch64 cross-compiler is missing or `GCC5_AARCH64_PREFIX` was not set. Install `gcc-aarch64-linux-gnu` and re-run `export GCC5_AARCH64_PREFIX=aarch64-linux-gnu-` before invoking `build`.

From this point on you can run the remaining steps in `Heat2D.sh` to test the code.
