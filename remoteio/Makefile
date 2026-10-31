# remoteio — 基于 LD_PRELOAD 的远程文件 I/O 劫持库
#
# target: libremoteio.so  (注入闭源客户端的共享库)
#         remote_server   (处理远程文件请求的 TCP 服务端)
#         test_client     (端到端测试程序)

CC       ?= gcc
CFLAGS   ?= -Wall -Wextra -O2 -fPIC -D_GNU_SOURCE
LDFLAGS  ?=
LIBS_DL   = -ldl -lpthread

SRC_DIR  = .
BUILD    = .

# 劫持库
RIO_HDR  = remoteio.h
RIO_OBJS = rio_fdmap.o rio_proto.o rio_pack.o libremoteio.o

# 可执行文件
SRV_BIN  = remote_server
SRV_OBJ  = remote_server.o

CLI_BIN  = test_client
CLI_OBJ  = test_client.o

.PHONY: all clean run srv test help

all: libremoteio.so $(SRV_BIN) $(CLI_BIN)

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

# --- test_client ---
$(CLI_BIN): $(CLI_OBJ)
	$(CC) -o $@ $^ $(LDFLAGS)

test_client.o: test_client.c $(RIO_HDR)
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f *.o libremoteio.so $(SRV_BIN) $(CLI_BIN)

# --- 便捷目标 ---
# 启动服务端（前台，ctrl+c 退出）
srv: $(SRV_BIN)
	./$(SRV_BIN)

# 跑一次端到端测试：先后台起服务端，等启动，再跑 LD_PRELOAD=... ./test_client
test: $(SRV_BIN) $(CLI_BIN) libremoteio.so
	@echo "==> 启动服务端..."
	./$(SRV_BIN) &
	@SRV_PID=$$!; \
	sleep 0.5; \
	echo "==> 运行 test_client (LD_PRELOAD=./libremoteio.so)..."; \
	LD_PRELOAD=$$(pwd)/libremoteio.so ./$(CLI_BIN); \
	RC=$$?; \
	echo "==> 停止服务端 (pid $$SRV_PID)"; \
	kill $$SRV_PID 2>/dev/null; \
	wait $$SRV_PID 2>/dev/null; \
	exit $$RC

help:
	@echo "make all     编译 .so + 服务端 + 测试程序"
	@echo "make srv     前台运行 remote_server"
	@echo "make test    自动起服务端 + 跑 test_client 端到端测试"
	@echo "make clean   清理构建产物"
	@echo ""
	@echo "使用方式："
	@echo "  1. 在服务器上:  ./remote_server &"
	@echo "  2. 启动闭源客户端: LD_PRELOAD=/path/to/libremoteio.so ./closed_source"
	@echo "  3. 客户端访问 /remote/xxx 路径时 → 转发到远程服务器"
