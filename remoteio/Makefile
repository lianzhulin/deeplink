# remoteio — 基于 LD_PRELOAD 的远程文件 I/O 劫持库（pack 模式专用）
#
# 架构：
#   客户端业务代码通过 rio_pack_register(path, offset, size) 在运行时注入
#   虚拟路径 → pack.bin 内 offset/size 映射。libremoteio.so 本身不携带任何
#   具体业务路径或 offset 数值，完全解耦。
#
# 劫持清单：open/openat/open64/openat64, read/write/close/lseek/pread/pwrite,
#          fopen/fread/fwrite/fclose/fseek/ftell/rewind/fflush
#
# target:
#   libremoteio.so   注入闭源客户端的共享库
#   remote_server    远程文件服务端（pack.bin 对它只是普通文件）
#   test_pack_client pack 多虚拟文件合并端到端测试

CC       ?= gcc
CFLAGS   ?= -Wall -Wextra -O2 -fPIC -D_GNU_SOURCE
LDFLAGS  ?=
LIBS_DL   = -ldl -lpthread

# 劫持库
RIO_HDR  = remoteio.h
RIO_OBJS = rio_fdmap.o rio_proto.o rio_pack.o libremoteio.o

# 可执行文件
SRV_BIN  = remote_server
SRV_OBJ  = remote_server.o

PACK_BIN = test_pack_client
PACK_OBJ = test_pack_client.o

PACK_DATA = rio_root/pack.bin

.PHONY: all clean run srv test_pack help

all: libremoteio.so $(SRV_BIN) $(PACK_BIN)

# --- libremoteio.so ---
libremoteio.so: $(RIO_OBJS)
	$(CC) -shared -o $@ $^ $(LIBS_DL) $(LDFLAGS)

rio_fdmap.o: rio_fdmap.c $(RIO_HDR)
	$(CC) $(CFLAGS) -c -o $@ $<

rio_proto.o: rio_proto.c $(RIO_HDR)
	$(CC) $(CFLAGS) -c -o $@ $<

rio_pack.o: rio_pack.c rio_pack.h $(RIO_HDR)
	$(CC) $(CFLAGS) -c -o $@ $<

libremoteio.o: libremoteio.c $(RIO_HDR) rio_pack.h
	$(CC) $(CFLAGS) -c -o $@ $<

# --- remote_server ---
$(SRV_BIN): $(SRV_OBJ) rio_proto.o rio_fdmap.o
	$(CC) -o $@ $^ $(LIBS_DL) $(LDFLAGS)

remote_server.o: remote_server.c $(RIO_HDR)
	$(CC) $(CFLAGS) -c -o $@ $<

# --- pack 多虚拟文件测试 ---
# test_pack_client 动态链接 libremoteio.so —— 这样 rio_pack_register 等符号有明确来源，
# 且 open/read 等劫持也自动生效（因为 libremoteio.so 是第一个被加载的共享库之一）
# 运行时仍用 LD_PRELOAD 双重保险（对 glibc 内部版本化符号 __openat_2 等）
$(PACK_BIN): $(PACK_OBJ) libremoteio.so
	$(CC) -o $@ $(PACK_OBJ) -L. -lremoteio $(LDFLAGS) -Wl,-rpath,$(shell pwd)

test_pack_client.o: test_pack_client.c $(RIO_HDR) rio_pack.h
	$(CC) $(CFLAGS) -c -o $@ $<

# pack.bin 测试数据（gen_pack.py 按 test_pack_client.c 的段信息生成）
$(PACK_DATA): rio_pack.c rio_pack.h gen_pack.py test_pack_client.c
	@mkdir -p rio_root
	python3 gen_pack.py

clean:
	rm -f *.o libremoteio.so $(SRV_BIN) $(PACK_BIN)
	rm -rf rio_root

# --- 便捷目标 ---
srv: $(SRV_BIN)
	./$(SRV_BIN)

test_pack: $(SRV_BIN) $(PACK_BIN) libremoteio.so $(PACK_DATA)
	@echo "==> [test_pack] pack 多虚拟文件合并端到端测试"
	@rm -rf rio_root && mkdir -p rio_root
	python3 gen_pack.py
	@./$(SRV_BIN) &
	@SRV_PID=$$!; \
	sleep 0.5; \
	echo "==> LD_PRELOAD=$$(pwd)/libremoteio.so ./$(PACK_BIN)"; \
	LD_PRELOAD=$$(pwd)/libremoteio.so ./$(PACK_BIN); \
	RC=$$?; \
	kill $$SRV_PID 2>/dev/null; \
	wait $$SRV_PID 2>/dev/null; \
	echo "==> test_pack exit: $$RC"; \
	exit $$RC

help:
	@echo "make all        编译 .so + 服务端 + pack 测试程序"
	@echo "make test_pack  跑 pack 多虚拟文件合并端到端测试"
	@echo "make srv        前台运行 remote_server"
	@echo "make clean      清理所有构建产物 + rio_root 测试数据"
	@echo ""
	@echo "使用方式："
	@echo "  1. 编译:  make all"
	@echo "  2. 服务端: ./remote_server -p 9527 -r ./rio_root &"
	@echo "     (确保 rio_root/ 下有 pack.bin)"
	@echo "  3. 业务代码在任何 open() 前注入映射:"
	@echo "     rio_pack_set_pack_file(\"/remote/pack.bin\");"
	@echo "     rio_pack_register(\"/virtual/config.txt\", 0, 64);"
	@echo "  4. 启动闭源客户端: LD_PRELOAD=/path/to/libremoteio.so ./closed_source"
	@echo ""
	@echo "核心设计：libremoteio.so 本身不携带任何 /virtual/xxx 路径或 offset 数值，"
	@echo "          所有 pack 布局由业务代码通过 rio_pack_register() 运行时注入。"
