CC      = gcc
CFLAGS  = -Wall -Wextra -O2 -std=c11 -D_POSIX_C_SOURCE=200809L
LDFLAGS = -lm
SRCS    = main.c lexer.c parser.c value.c interp.c
TARGET  = kite

all: $(TARGET)

$(TARGET): $(SRCS) kite.h
	$(CC) $(CFLAGS) -o $(TARGET) $(SRCS) $(LDFLAGS)

clean:
	rm -f $(TARGET)

.PHONY: all clean
