# remoteio — 基于 LD_PRELOAD 的远程文件 I/O 劫持库
#
# 两大功能：
#   1. 普通远程路径：访问 /remote/xxx → 转发到 remote_server
#   2. pack 合并模式：多个虚拟路径（/virtual/xxx）映射到服务端单个 pack.bin 的不同 offset
#      映射表在 rio_pack.c 的 g_entries[] 静态数组里声明，编译期决定
#
# target:
#   libremoteio.so   注入闭源客户端的共享库
#   remote_server    远程文件服务端
#   test_client      普通 /remote/xxx 端到端测试
#   test_pack_client pack 多虚拟文件合并端到端测试

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

PACK_BIN = test_pack_client
PACK_OBJ = test_pack_client.o

PACK_DATA = rio_root/pack.bin

.PHONY: all clean run srv test test_pack test_all help

all: libremoteio.so $(SRV_BIN) $(CLI_BIN) $(PACK_BIN)

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

# --- 普通远程路径测试 ---
$(CLI_BIN): $(CLI_OBJ)
	$(CC) -o $@ $^ $(LDFLAGS)

test_client.o: test_client.c $(RIO_HDR)
	$(CC) $(CFLAGS) -c -o $@ $<

# --- pack 多虚拟文件测试 ---
$(PACK_BIN): $(PACK_OBJ)
	$(CC) -o $@ $^ $(LDFLAGS)

test_pack_client.o: test_pack_client.c $(RIO_HDR) rio_pack.h
	$(CC) $(CFLAGS) -c -o $@ $<

# pack.bin 测试数据（gen_pack.py 按 rio_pack.c 里的映射精确生成）
$(PACK_DATA): rio_pack.c rio_pack.h gen_pack.py
	@mkdir -p rio_root
	python3 gen_pack.py

clean:
	rm -f *.o libremoteio.so $(SRV_BIN) $(CLI_BIN) $(PACK_BIN)
	rm -rf rio_root $(PACK_DATA)

# --- 便捷目标 ---
# 前台启动服务端（ctrl+c 退出）
srv: $(SRV_BIN)
	./$(SRV_BIN)

# 跑普通 /remote/xxx 端到端测试
test: $(SRV_BIN) $(CLI_BIN) libremoteio.so
	@echo "==> [test] 普通远程路径 /remote/xxx 端到端测试"
	@rm -rf rio_root && mkdir -p rio_root
	@./$(SRV_BIN) &
	@SRV_PID=$$!; \
	sleep 0.5; \
	echo "==> LD_PRELOAD=$$(pwd)/libremoteio.so ./$(CLI_BIN)"; \
	LD_PRELOAD=$$(pwd)/libremoteio.so ./$(CLI_BIN); \
	RC=$$?; \
	kill $$SRV_PID 2>/dev/null; \
	wait $$SRV_PID 2>/dev/null; \
	echo "==> test exit: $$RC"; \
	exit $$RC

# 跑 pack 多虚拟文件合并端到端测试（无需 RIO_PACK_CONFIG）
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

# 一次跑完两个测试
test_all: test test_pack

help:
	@echo "make all        编译 .so + 服务端 + 两套测试程序"
	@echo "make test       跑普通 /remote/xxx 端到端测试"
	@echo "make test_pack  跑 pack 多虚拟文件合并端到端测试（无需 INI / 无环境变量）"
	@echo "make test_all   连续跑 test + test_pack"
	@echo "make srv        前台运行 remote_server"
	@echo "make clean      清理所有构建产物 + rio_root 测试数据"
	@echo ""
	@echo "使用方式 —— 普通远程路径:"
	@echo "  1. ./remote_server -p 9527 -r ./rio_root &"
	@echo "  2. LD_PRELOAD=/path/to/libremoteio.so ./closed_source_app"
	@echo "  3. app 访问 /remote/xxx → 自动转发到服务器"
	@echo ""
	@echo "使用方式 —— pack 多虚拟文件合并:"
	@echo "  1. 服务端 rio_root/ 下放一个 pack.bin（gen_pack.py 生成或自行构造）"
	@echo "  2. 如需新增虚拟段，改 rio_pack.c 的 g_entries[] 数组，重编译 .so"
	@echo "  3. LD_PRELOAD=/path/to/libremoteio.so ./closed_source_app"
	@echo "  4. app 访问 /virtual/config.txt → 自动翻译成 pack.bin offset=0 的读写"
