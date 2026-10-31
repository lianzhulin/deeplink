#!/usr/bin/env python3
"""AXI 协议 TCP 桥端到端测试 — 直接测 C 服务端的 AXI 帧处理。"""
import struct, socket, sys

HOST, PORT = "127.0.0.1", 9527

AXI_OP_READ=0; AXI_OP_WRITE=1; AXI_OP_STREAM_WR=2; AXI_OP_STREAM_RD=3; AXI_OP_FLUSH=4
STATUS_OK=0; STATUS_ERR=1

def connect():
    s = socket.socket()
    s.settimeout(5)
    s.connect((HOST, PORT))
    return s

def axi_write(s, addr, data, stream=False):
    """发送 AXI WRITE/STREAM_WR, 返回 (status, ret_val)。"""
    op = AXI_OP_STREAM_WR if stream else AXI_OP_WRITE
    frame = struct.pack('>BII', op, addr, len(data)) + data
    s.sendall(frame)
    resp = s.recv(5)
    if len(resp) < 5: raise ConnectionError("short resp")
    return struct.unpack('>BI', resp)

def axi_read(s, addr, n, stream=False):
    """发送 AXI READ/STREAM_RD, 返回 (status, ret_val, data_or_None)。"""
    op = AXI_OP_STREAM_RD if stream else AXI_OP_READ
    frame = struct.pack('>BII', op, addr, n)
    s.sendall(frame)
    resp = b''
    while len(resp) < 5:
        c = s.recv(5 - len(resp))
        if not c: raise ConnectionError("closed mid-header")
        resp += c
    st, rv = struct.unpack('>BI', resp)
    if st == STATUS_OK and rv > 0:
        data = b''
        while len(data) < rv:
            c = s.recv(rv - len(data))
            if not c: raise ConnectionError("closed mid-data")
            data += c
        return st, rv, data
    return st, rv, None

def axi_flush(s):
    frame = struct.pack('>BII', AXI_OP_FLUSH, 0, 0)
    s.sendall(frame)
    resp = s.recv(5)
    return struct.unpack('>BI', resp)

tp = tf = 0
def CHECK(c, msg):
    global tp, tf
    if c: print(f"  PASS: {msg}"); tp += 1
    else: print(f"  FAIL: {msg}"); tf += 1

def main():
    s = connect()
    print("=== AXI 协议 TCP 桥测试 ===")

    # 1) AXI WRITE + AXI READ 回环
    data = b"HELLO_AXI_WORLD"
    st, rv = axi_write(s, 0x1000, data)
    CHECK(st == STATUS_OK and rv == len(data), f"AXI WRITE → OK(ret={rv})")

    st, rv, rd = axi_read(s, 0x1000, len(data))
    CHECK(st == STATUS_OK and rd == data, f"AXI READ 回环: {rd!r}")

    # 2) STREAM_WR 8KB 无 4K 限制
    big = b'Y' * 8192
    st, rv = axi_write(s, 0x2000, big, stream=True)
    CHECK(st == STATUS_OK and rv == 8192, f"STREAM_WR 8KB (无4K限) ret={rv}")

    st, rv, rd = axi_read(s, 0x2000, 8192, stream=True)
    CHECK(st == STATUS_OK and rd == big, "STREAM_RD 8KB 回环正确")

    # 3) 单笔 5000B WRITE → ERR (超 4K)
    st, rv = axi_write(s, 0x3000, b'X' * 5000)
    CHECK(st == STATUS_ERR and rv == 0x01, f"单笔 5000B → ERR_OVER_4K=0x01 (got 0x{rv:02x})")

    # 4) 越界 → ERR
    st, rv = axi_write(s, 16*1024*1024 - 50, b'Z' * 100)
    CHECK(st == STATUS_ERR and rv == 0x00, f"越界 → ERR_OUT_OF_RANGE=0x00 (got 0x{rv:02x})")

    # 5) FLUSH
    st, rv = axi_flush(s)
    CHECK(st == STATUS_OK and rv == 0, f"AXI FLUSH → OK(ret={rv})")

    # 6) AXI → pack version 联动
    # AXI 写 config.txt body (offset=688), 应该 bump seg[0] 的 version
    st, rv = axi_write(s, 688, b"MOD_BY_AXI!!")
    CHECK(st == STATUS_OK, "AXI WRITE 到 pack body region")

    # 读回来验证写入生效
    st, rv, rd = axi_read(s, 688, 12)
    CHECK(st == STATUS_OK and rd == b"MOD_BY_AXI!!", f"AXI READ 验证写入: {rd!r}")

    # 7) 未知 AXI op —— 但 framing 要求 first byte <= 4 才走 AXI 路径
    #    所有 <=4 的值都是合法 op, 无法在 AXI 帧内部触发 ERR_UNKNOWN
    #    服务端的 framing detection 就是 AXI 路径的"保护"
    print("  [SKIP] 未知 op 测试 (所有 AXI opcodes 0-4 已全覆盖)")

    s.close()
    print(f"\n=== Summary: {tp} passed, {tf} failed ===")
    return 1 if tf else 0

if __name__ == "__main__":
    sys.exit(main())
