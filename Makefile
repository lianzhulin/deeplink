CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra -std=c99 -Werror
TARGET  = bdiff_test
TARGET_Z = bdiff_test_z

.PHONY: all test test_z clean

all: $(TARGET)

$(TARGET): bdiff.c bdiff.h test_bdiff.c
	$(CC) $(CFLAGS) -o $(TARGET) bdiff.c test_bdiff.c

$(TARGET_Z): bdiff.c bdiff.h test_bdiff.c
	$(CC) $(CFLAGS) -DBDIFF_HAVE_ZLIB=1 -o $(TARGET_Z) bdiff.c test_bdiff.c -lz

test: $(TARGET)
	./$(TARGET)

test_z: $(TARGET_Z)
	./$(TARGET_Z)

clean:
	rm -f $(TARGET) $(TARGET_Z)
