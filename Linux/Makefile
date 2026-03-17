CC       = gcc
PKG      = gtk4
CFLAGS   = -Wall -Wextra -O2 -march=native $(shell pkg-config --cflags $(PKG)) \
           -fPIE -fstack-protector-strong -D_FORTIFY_SOURCE=2
LDFLAGS  = $(shell pkg-config --libs $(PKG)) -pie -lpthread -lgmp
SRCS     = $(wildcard src/*.c)
OBJS     = $(SRCS:.c=.o)
TARGET   = tuxtimings

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) -o $@ $^ $(LDFLAGS)

src/%.o: src/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

# Build the tuxbench kernel module (optional — app works without it)
kmod:
	$(MAKE) -C src/tuxbench

kmod-install: kmod
	$(MAKE) -C src/tuxbench install

kmod-clean:
	$(MAKE) -C src/tuxbench clean

clean: kmod-clean
	rm -f src/*.o $(TARGET)

.PHONY: all clean kmod kmod-install kmod-clean
