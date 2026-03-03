CC       = gcc
PKG      = gtk4
CFLAGS   = -Wall -Wextra -O2 $(shell pkg-config --cflags $(PKG)) \
           -fPIE -fstack-protector-strong -D_FORTIFY_SOURCE=2
LDFLAGS  = $(shell pkg-config --libs $(PKG)) -pie

# Intel spoof mode: make INTEL_DEBUG=1
# Forces the Intel code path with DDR5-6000 CL30 dummy values so the
# Intel UI can be tested on AMD hardware without a physical Intel machine.
ifeq ($(INTEL_DEBUG),1)
CFLAGS  += -DSPOOF_INTEL_CPU
endif
SRCS     = $(wildcard src/*.c)
OBJS     = $(SRCS:.c=.o)
TARGET   = tuxtimings

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) -o $@ $^ $(LDFLAGS)

src/%.o: src/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f src/*.o $(TARGET)

.PHONY: all clean
