CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra -std=c99 -Werror
TARGET = bdiff_test

all: $(TARGET)

$(TARGET): bdiff.c bdiff.h test_bdiff.c
	$(CC) $(CFLAGS) -o $(TARGET) bdiff.c test_bdiff.c

test: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(TARGET)

.PHONY: all test clean
