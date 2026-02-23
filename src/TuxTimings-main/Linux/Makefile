CC       = gcc
PKG      = gtk4
CFLAGS   = -Wall -Wextra -O2 $(shell pkg-config --cflags $(PKG)) \
           -fPIE -fstack-protector-strong -D_FORTIFY_SOURCE=2
LDFLAGS  = $(shell pkg-config --libs $(PKG)) -pie
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
