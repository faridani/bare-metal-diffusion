CIRCLEHOME ?= ../circle
ARMGNU ?= aarch64-none-elf

TARGET = kernel8.img
OBJS = Heat2D.o

CPPFLAGS += -I$(CIRCLEHOME)/include
CFLAGS += -mcpu=cortex-a76 -march=armv8.2-a+crypto -ffast-math
CXXFLAGS += -std=gnu++17
CXXFLAGS += $(CFLAGS)

LIBS = circle circleusb circlescreen circleinput circletime circleutil

include $(CIRCLEHOME)/sample/Rules.mk
