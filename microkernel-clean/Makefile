# ---- microkernel core Makefile ----
CC      = gcc
AR      = ar
CFLAGS  = -std=c11 -Wall -Wextra -Wpedantic -O2 -g \
          -Iinclude \
          -Wno-unused-parameter -Wno-unused-function

SRC_DIR = src
OBJ_DIR = build

SOURCES = $(wildcard $(SRC_DIR)/*.c)
ASM     = $(wildcard $(SRC_DIR)/*.S)
OBJECTS = $(patsubst $(SRC_DIR)/%.c,$(OBJ_DIR)/%.o,$(SOURCES)) \
          $(patsubst $(SRC_DIR)/%.S,$(OBJ_DIR)/%.o,$(ASM))

LIB     = $(OBJ_DIR)/libmicrokernel.a

.PHONY: all clean

all: $(LIB)

$(LIB): $(OBJECTS) | $(OBJ_DIR)
	$(AR) rcs $@ $^

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c | $(OBJ_DIR)
	$(CC) $(CFLAGS) -c -o $@ $<

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.S | $(OBJ_DIR)
	$(CC) $(CFLAGS) -c -o $@ $<

$(OBJ_DIR):
	mkdir -p $(OBJ_DIR)

clean:
	rm -rf $(OBJ_DIR)
