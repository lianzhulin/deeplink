#
# ============================================================================
#   Makefile — 微内核原型的构建系统
# ============================================================================
#
#   【实现要点】单 Makefile，顶层 build/ 和 bin/ 自动创建。
#              $(wildcard src/*.c src/*.S) 自动发现源文件；
#              $(patsubst) 把 src/foo.c → build/foo.o、src/foo.S → build/foo.o；
#              最终链接成 bin/mk。gcc 同时编译 .c 和 .S（gas 汇编）。
#   【硬化实现】CFLAGS 严格：-std=c11 -Wall -Wextra -Wpedantic —— 所有警告必须清零；
#              用 -Wno-unused-parameter -Wno-unused-function 屏蔽 demo 里显式
#              标注 (void)arg 的参数和 irq.c 里保留的 helper 函数告警；
#              .PHONY: all clean run 避免与同名字典文件冲突；
#              clean 只 rm -rf build/ bin/，不会误删其他东西。
#   【热路径】无热路径。构建只在开发期跑。
#   【错误处理】任何 CFLAGS 告警都应该视为失败（-Wall 级别）。
#              Makefile 里没有 @ 静默执行，出错会直接显示 shell 原命令。

# ============================================================================
#

CC      = gcc
CFLAGS  = -std=c11 -Wall -Wextra -Wpedantic -O2 -g \
          -Iinclude \
          -Wno-unused-parameter -Wno-unused-function
LDFLAGS =

SRC_DIR = src
OBJ_DIR = build
BIN_DIR = bin

SOURCES = $(wildcard $(SRC_DIR)/*.c)
ASM     = $(wildcard $(SRC_DIR)/*.S)
OBJECTS = $(patsubst $(SRC_DIR)/%.c,$(OBJ_DIR)/%.o,$(SOURCES)) \
          $(patsubst $(SRC_DIR)/%.S,$(OBJ_DIR)/%.o,$(ASM))
TARGET  = $(BIN_DIR)/mk

.PHONY: all clean run

all: $(TARGET)

$(TARGET): $(OBJECTS) | $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c | $(OBJ_DIR)
	$(CC) $(CFLAGS) -c -o $@ $<

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.S | $(OBJ_DIR)
	$(CC) $(CFLAGS) -c -o $@ $<

$(OBJ_DIR):
	mkdir -p $(OBJ_DIR)

$(BIN_DIR):
	mkdir -p $(BIN_DIR)

run: $(TARGET)
	$(TARGET)

clean:
	rm -rf $(OBJ_DIR) $(BIN_DIR)
