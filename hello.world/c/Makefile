CROSS ?= aarch64-linux-gnu
CC    := $(CROSS)-gcc
LD    := $(CROSS)-ld
OBJCOPY := $(CROSS)-objcopy

CFLAGS := -O2 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
          -Wall -Wextra -mcpu=cortex-a76
LDFLAGS := -T linker.ld -nostdlib

all: kernel.elf kernel.bin

kernel.elf: start.o main.o
	$(CC) $(CFLAGS) -Wl,$(LDFLAGS) -o $@ $^


start.o: start.S
	$(CC) $(CFLAGS) -c $< -o $@

main.o: main.c uart_pl011.h
	$(CC) $(CFLAGS) -c main.c -o main.o

kernel.bin: kernel.elf
	$(OBJCOPY) -O binary $< $@

clean:
	rm -f *.o kernel.elf kernel.bin
