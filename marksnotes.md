I installed an Ubuntu on a virtual box on a windows 

Then inside Ubuntu 

```bash
sudo apt update
sudo apt install -y build-essential git python3 \
  gcc-aarch64-linux-gnu g++-aarch64-linux-gnu \
  nasm iasl uuid-dev
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

```bash 
cd ..
git clone https://github.com/faridani/uefi-2D-heat-diffusion.git
mv ./uefi-2D-heat-diffusion ./edk2/Heat2D


### 2) Build BaseTools and set up the environment

```bash
make -C BaseTools
. edksetup.sh
```

# Now build 

```bash
sudo apt update
sudo apt install gcc-aarch64-linux-gnu g++-aarch64-linux-gnu make uuid-dev 
sudo apt install -y qemu-system-arm qemu-system-aarch64 qemu-efi-aarch64 mtools dosfstools
```



NOW BUILD 

# ====== 2) Build your UEFI app (DEBUG) ======
build -a AARCH64 -t GCC5 -b DEBUG -p Heat2D/Heat2D.dsc

# Your output EFI will be here:
# Build/Heat2DPlatform/DEBUG_GCC5/AARCH64/Heat2D.efi

From this point on you should be able to run the rest of 
Head2D.sh and see the code 




