
step 1 install wsl
step 2 install ubuntu

```bash
sudo apt update
sudo apt install -y \
  build-essential git python3 \
  gcc-aarch64-linux-gnu g++-aarch64-linux-gnu \
  nasm iasl uuid-dev \
  qemu-system-x86 qemu-system-aarch64 \
  ovmf qemu-efi-aarch64 \
  mtools dosfstools
```

```
Practical recommendation

Best workflow on Windows:

Windows 11 + WSL2 (Ubuntu)

Build with EDK2 inside WSL

Test in:

QEMU (fast, no reboot)

Final test on:

Raspberry Pi 5 hardware

This is a very common and well-supported UEFI development setup.
```


Cons
* All training wheels are removed. There is no debugger in this level
* If a virus gets in, it's game over. You cannot install an antivirus here 
