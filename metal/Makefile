# Build the bare-metal Heat2D demo with Circle
# Set CIRCLEHOME to the root of your Circle checkout (default ../circle)

CIRCLEHOME ?= ../circle

OBJS = Heat2D.o
LIBS = $(CIRCLEHOME)/lib/libcircle.a

# Enable Pi 5 defaults; override on the make command if needed.
EXTRA_CFLAGS += -DRASPPI=5 -mcpu=cortex-a76
EXTRA_CXXFLAGS += -DRASPPI=5 -mcpu=cortex-a76

include $(CIRCLEHOME)/sample/Rules.mk

-include $(DEPS)
