CC      = gcc
CFLAGS  = -Wall -Wextra -std=c99 -pedantic \
          -D_DEFAULT_SOURCE -D_BSD_SOURCE -D_POSIX_C_SOURCE=200809L
LDFLAGS = -lX11
PREFIX  = /usr/local

SRC    = trinitywm.c
OBJ    = $(SRC:.c=.o)
TARGET = twm

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) $(OBJ) -o $(TARGET) $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

install: $(TARGET)
	install -Dm755 $(TARGET) $(PREFIX)/bin/$(TARGET)

clean:
	rm -f $(OBJ) $(TARGET)

.PHONY: all clean install
