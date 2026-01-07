CC = aarch64-linux-gnu-gcc
LD = aarch64-linux-gnu-ld
OBJCOPY = aarch64-linux-gnu-objcopy

CFLAGS = -Wall -O3 -ffreestanding -nostdlib -mcpu=cortex-a72
LDFLAGS = -T link.ld -nostdlib

all: kernel8.img

kernel8.img: start.o kernel.o
	$(LD) $(LDFLAGS) start.o kernel.o -o kernel8.elf
	$(OBJCOPY) -O binary kernel8.elf kernel8.img

start.o: start.S
	$(CC) $(CFLAGS) -c start.S -o start.o

kernel.o: kernel.c
	$(CC) $(CFLAGS) -c kernel.c -o kernel.o

clean:
	rm -f *.o *.elf *.img

run: kernel8.img
	qemu-system-aarch64 -M raspi4b -cpu cortex-a72 -m 2G -serial stdio -kernel kernel8.img