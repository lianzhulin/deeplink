# remoteio — 基于 LD_PRELOAD 的远程文件 I/O 劫持库（共享内存 pack 模式）
#
# 架构：
#   服务端启动时创建 16MB POSIX 共享内存段 (RIO_PACK_SHM_NAME)，
#   写入自描述 pack 格式头部 + entries + 测试 body。
#   业务代码零配置，LD_PRELOAD=libremoteio.so ./app 即可。
#   所有虚拟文件 I/O 都是客户端对本地 mmap 的 memcpy —— 零网络、零磁盘。
#
# 劫持清单：open/openat/open64/openat64, read/write/close/lseek/pread/pwrite,
#          fopen/fread/fwrite/fclose/fseek/ftell/rewind/fflush

CC       ?= gcc
CFLAGS   ?= -Wall -Wextra -O2 -fPIC -D_GNU_SOURCE
LDFLAGS  ?=
LIBS_DL   = -ldl -lpthread -lrt   # -lrt for shm_open

# 劫持库
RIO_HDR  = remoteio.h
RIO_OBJS = rio_fdmap.o rio_proto.o rio_pack.o libremoteio.o

# 可执行文件
SRV_BIN  = remote_server
SRV_OBJ  = remote_server.o

PACK_BIN = test_pack_client
PACK_OBJ = test_pack_client.o

.PHONY: all clean srv test_shm help

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
$(SRV_BIN): $(SRV_OBJ)
	$(CC) -o $@ $^ $(LIBS_DL) $(LDFLAGS)

remote_server.o: remote_server.c $(RIO_HDR) rio_pack.h
	$(CC) $(CFLAGS) -c -o $@ $<

# --- pack 多虚拟文件测试 ---
# test_pack_client 动态链接 libremoteio.so —— 这样 open/read 等劫持自动生效
# 运行时仍用 LD_PRELOAD 双重保险（对 glibc 内部版本化符号 __openat_2 等）
$(PACK_BIN): $(PACK_OBJ) libremoteio.so
	$(CC) -o $@ $(PACK_OBJ) -L. -lremoteio $(LDFLAGS) -Wl,-rpath,$(shell pwd)

test_pack_client.o: test_pack_client.c $(RIO_HDR) rio_pack.h
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f *.o libremoteio.so $(SRV_BIN) $(PACK_BIN)

# --- 便捷目标 ---
srv: $(SRV_BIN)
	./$(SRV_BIN)

test_shm: $(SRV_BIN) $(PACK_BIN) libremoteio.so
	@echo "==> [test_shm] shm pack 多虚拟文件端到端测试"
	@rm -f /dev/shm/rio_packshm
	@./$(SRV_BIN) &
	@SRV_PID=$$!; \
	sleep 0.5; \
	echo "==> LD_PRELOAD=$$(pwd)/libremoteio.so ./$(PACK_BIN)"; \
	RIO_DEBUG=1 LD_PRELOAD=$$(pwd)/libremoteio.so ./$(PACK_BIN); \
	RC=$$?; \
	kill $$SRV_PID 2>/dev/null; \
	wait $$SRV_PID 2>/dev/null; \
	echo "==> test_shm exit: $$RC"; \
	exit $$RC

strings_check: libremoteio.so
	@echo "==> 检查 libremoteio.so 是否有硬编码业务路径（应该 0 hits）:"
	@strings libremoteio.so | grep -E '/virtual/|config\.txt' || echo "  OK: 0 hits"; \
	echo "  strings check done"

help:
	@echo "make all        编译 .so + 服务端 + pack 测试程序"
	@echo "make test_shm   shm 模式端到端测试 (默认 16MB shm)"
	@echo "make srv        前台运行 remote_server"
	@echo "make clean      清理所有构建产物"
	@echo "make strings_check  验证 .so 无硬编码路径"
	@echo ""
	@echo "使用方式："
	@echo "  1. 编译:  make all"
	@echo "  2. 服务端: ./remote_server &"
	@echo "  3. 业务:   LD_PRELOAD=\$\$(pwd)/libremoteio.so ./closed_source"
	@echo ""
	@echo "核心设计："
	@echo "  - 服务端创建 16MB POSIX shm (name=\"/rio_packshm\")"
	@echo "  - 写入 self-describing pack 头部 (256B hdr + 144B each entry + body)"
	@echo "  - 客户端 mmap 同一段 shm，所有虚拟文件 I/O = 本地 memcpy"
	@echo "  - libremoteio.so 零业务路径硬编码（用 strings_check 验证）"
