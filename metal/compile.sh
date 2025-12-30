rm -f *.o kernel.elf kernel8.img

aarch64-linux-gnu-gcc -c -O2 -ffreestanding -nostdlib -nostartfiles start.S -o start.o

aarch64-linux-gnu-g++ -c -O2 -std=gnu++17 \
  -ffreestanding -fno-exceptions -fno-rtti \
  -fno-stack-protector -fno-pic -fno-pie \
  -nostdlib -nostartfiles \
  Heat2D_ramfb.cpp -o Heat2D_ramfb.o

aarch64-linux-gnu-ld -T link.ld -o kernel.elf start.o Heat2D_ramfb.o

# optional
aarch64-linux-gnu-objcopy -O binary kernel.elf kernel8.img
