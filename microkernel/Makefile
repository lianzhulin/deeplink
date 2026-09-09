# ---- microkernel prototype Makefile ----
CC      = gcc
CFLAGS  = -std=c11 -Wall -Wextra -Wpedantic -O2 -g \
          -Iinclude \
          -Wno-unused-parameter -Wno-unused-function
LDFLAGS =

SRC_DIR = src
OBJ_DIR = build
BIN_DIR = bin

SOURCES = $(wildcard $(SRC_DIR)/*.c)
OBJECTS = $(patsubst $(SRC_DIR)/%.c,$(OBJ_DIR)/%.o,$(SOURCES))
TARGET  = $(BIN_DIR)/mk

.PHONY: all clean run

all: $(TARGET)

$(TARGET): $(OBJECTS) | $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c | $(OBJ_DIR)
	$(CC) $(CFLAGS) -c -o $@ $<

$(OBJ_DIR):
	mkdir -p $(OBJ_DIR)

$(BIN_DIR):
	mkdir -p $(BIN_DIR)

run: $(TARGET)
	$(TARGET)

clean:
	rm -rf $(OBJ_DIR) $(BIN_DIR)
