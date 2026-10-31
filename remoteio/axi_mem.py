#!/usr/bin/env python3
"""
axi_mem — 极简 AXI 内存功能模型 (内核, 自包含)
================================================

一块连续内存的事务级读写, 无突发/无时序/无波形。
  * 连续内存, 默认最大 16MB (bytearray 存储), 事务级读写, 自带边界检查与并发锁。
  * 单笔请求最大 4096 字节 (READ/WRITE 各 <=4096B)。
  * 大块读写 (STREAM_WR/STREAM_RD) 不受单笔 4096B 限制, 仅受内存边界约束。

二进制帧约定 (大端序):
  请求头: ">BII"  op(1) + addr(4) + length(4)  共 9 字节
  响应头: ">BI"   status(1) + ret_val(4)      共 5 字节

  op:     0=READ  1=WRITE  2=STREAM_WR  3=STREAM_RD  4=FLUSH
  status: 0=OK    1=ERR (越界 / 超 4096B / 未知 op)

本内核不发起任何访问 — 只提供:
  process_frame(req_bytes) -> resp_bytes     处理单帧
  read(addr, n) / write(addr, data)          直接 API
  read_block(addr, n) / write_block(addr, data)  无单 4K 限制

外部桥 (gen_pack.py 等) 负责把帧送进来。
"""

import struct
import threading

# ================================================================
# 协议常量
# ================================================================

OP_READ      = 0
OP_WRITE     = 1
OP_STREAM_WR = 2
OP_STREAM_RD = 3
OP_FLUSH     = 4

STATUS_OK = 0
STATUS_ERR = 1

MAX_SINGLE = 4096            # READ/WRITE 单笔上限
DEFAULT_SIZE = 16 * 1024 * 1024   # 16MB

# 帧头大小
REQ_HDR_SIZE  = struct.calcsize('>BII')   # 9
RESP_HDR_SIZE = struct.calcsize('>BI')    # 5


# ================================================================
# 异常
# ================================================================

class AxiMemError(Exception):
    pass


# ================================================================
# AXI 内存内核
# ================================================================

class AxiMemory:
    """极简 AXI 内存功能模型。"""

    def __init__(self, size: int = DEFAULT_SIZE):
        if size <= 0:
            raise AxiMemError(f"size must be > 0, got {size}")
        self._size = size
        self._mem = bytearray(size)
        self._lock = threading.Lock()

    # ---- 直接 API (无帧) ----

    @property
    def size(self) -> int:
        return self._size

    def read(self, addr: int, n: int) -> bytes:
        """单笔读 (<=4096B), 越界/超限抛 AxiMemError。"""
        if n > MAX_SINGLE:
            raise AxiMemError(f"单笔 READ {n} > MAX_SINGLE {MAX_SINGLE}")
        if addr < 0 or n < 0 or addr + n > self._size:
            raise AxiMemError(f"READ 越界: addr={addr} n={n} mem_size={self._size}")
        with self._lock:
            return bytes(self._mem[addr:addr + n])

    def write(self, addr: int, data: bytes) -> int:
        """单笔写 (<=4096B), 越界/超限抛 AxiMemError。返回实际写入字节数。"""
        n = len(data)
        if n > MAX_SINGLE:
            raise AxiMemError(f"单笔 WRITE {n} > MAX_SINGLE {MAX_SINGLE}")
        if addr < 0 or addr + n > self._size:
            raise AxiMemError(f"WRITE 越界: addr={addr} n={n} mem_size={self._size}")
        with self._lock:
            self._mem[addr:addr + n] = data
        return n

    # ---- 大块 API (无单 4K 限制) ----

    def read_block(self, addr: int, n: int) -> bytes:
        """无单笔上限的大块读, 仅受内存边界约束。"""
        if addr < 0 or n < 0 or addr + n > self._size:
            raise AxiMemError(f"read_block 越界: addr={addr} n={n} mem_size={self._size}")
        with self._lock:
            return bytes(self._mem[addr:addr + n])

    def write_block(self, addr: int, data: bytes) -> int:
        """无单笔上限的大块写, 仅受内存边界约束。"""
        n = len(data)
        if addr < 0 or addr + n > self._size:
            raise AxiMemError(f"write_block 越界: addr={addr} n={n} mem_size={self._size}")
        with self._lock:
            self._mem[addr:addr + n] = data
        return n

    def flush(self) -> None:
        """FLUSH —— 本模型无 cache, 空操作, 仅为协议完整性。"""
        pass

    # ---- 帧处理 ----

    def process_frame(self, req: bytes) -> bytes:
        """
        处理一帧请求, 返回完整响应帧。

        请求帧:
          READ/STREAM_RD:  9B ">BII"  op, addr, length
          WRITE/STREAM_WR: 9B ">BII"  + length 字节 data
          FLUSH:           9B ">BII"  (addr/length 忽略)

        响应帧:
          错误:    5B ">BI"  status=1, ret_val=error_code
          成功:
            READ/STREAM_RD: 5B header + ret_val 字节 data
            其他:           5B header, ret_val=实际字节数或 0
        """
        try:
            if len(req) < REQ_HDR_SIZE:
                return self._err(0x02)   # 帧头过短

            op, addr, length = struct.unpack_from('>BII', req, 0)

            if op == OP_READ:
                if length > MAX_SINGLE:
                    return self._err(0x01)   # 超 4096B
                return self._do_read(addr, length)

            elif op == OP_WRITE:
                if length > MAX_SINGLE:
                    return self._err(0x01)
                if len(req) < REQ_HDR_SIZE + length:
                    return self._err(0x02)
                data = req[REQ_HDR_SIZE:REQ_HDR_SIZE + length]
                return self._do_write(addr, data)

            elif op == OP_STREAM_RD:
                return self._do_read_block(addr, length)

            elif op == OP_STREAM_WR:
                if len(req) < REQ_HDR_SIZE + length:
                    return self._err(0x02)
                data = req[REQ_HDR_SIZE:REQ_HDR_SIZE + length]
                return self._do_write_block(addr, data)

            elif op == OP_FLUSH:
                self.flush()
                return self._ok(0)

            else:
                return self._err(0x03)   # 未知 op

        except Exception as e:
            return self._err(0xFF)

    # ---- 帧内部 helper ----

    def _ok(self, ret_val: int) -> bytes:
        return struct.pack('>BI', STATUS_OK, ret_val)

    def _err(self, code: int) -> bytes:
        return struct.pack('>BI', STATUS_ERR, code)

    def _do_read(self, addr: int, n: int) -> bytes:
        try:
            data = self.read(addr, n)
            return self._ok(len(data)) + data
        except AxiMemError:
            return self._err(0x00)   # 越界

    def _do_write(self, addr: int, data: bytes) -> bytes:
        try:
            wrote = self.write(addr, data)
            return self._ok(wrote)
        except AxiMemError:
            return self._err(0x00)

    def _do_read_block(self, addr: int, n: int) -> bytes:
        try:
            data = self.read_block(addr, n)
            return self._ok(len(data)) + data
        except AxiMemError:
            return self._err(0x00)

    def _do_write_block(self, addr: int, data: bytes) -> bytes:
        try:
            wrote = self.write_block(addr, data)
            return self._ok(wrote)
        except AxiMemError:
            return self._err(0x00)


# ================================================================
# 便捷 TCP 客户端桥 (gen_pack.py 等外部工具使用)
# ================================================================

def build_read_frame(addr: int, length: int, stream: bool = False) -> bytes:
    """构建 READ 或 STREAM_RD 请求帧。"""
    op = OP_STREAM_RD if stream else OP_READ
    return struct.pack('>BII', op, addr, length)


def build_write_frame(addr: int, data: bytes, stream: bool = False) -> bytes:
    """构建 WRITE 或 STREAM_WR 请求帧。"""
    op = OP_STREAM_WR if stream else OP_WRITE
    return struct.pack('>BII', op, addr, len(data)) + data


def build_flush_frame() -> bytes:
    return struct.pack('>BII', OP_FLUSH, 0, 0)


def parse_response(resp: bytes):
    """
    解析响应帧, 返回 (status, ret_val, data_or_None)。
      status=0 OK:   ret_val = 实际字节数, data = resp[5:5+ret_val] (仅 READ)
      status=1 ERR:  ret_val = 错误码,     data = None
    """
    if len(resp) < RESP_HDR_SIZE:
        return (STATUS_ERR, 0x02, None)
    status, ret = struct.unpack_from('>BI', resp, 0)
    if status == STATUS_OK and len(resp) > RESP_HDR_SIZE and ret > 0:
        data = resp[RESP_HDR_SIZE:RESP_HDR_SIZE + ret]
        return (STATUS_OK, ret, data)
    return (status, ret, None)


# ================================================================
# 自测试
# ================================================================

def _self_test():
    import io as _io

    print("=== axi_mem 内核自测试 ===")

    m = AxiMemory(size=DEFAULT_SIZE)   # 16MB 足够大块测试

    # 1) 基本 write/read
    data = b"HELLO_WORLD"
    wf = build_write_frame(0x100, data)
    resp = m.process_frame(wf)
    st, rv, _ = parse_response(resp)
    assert st == STATUS_OK and rv == len(data), f"write FAIL: {st} {rv}"

    rf = build_read_frame(0x100, len(data))
    resp = m.process_frame(rf)
    st, rv, rd = parse_response(resp)
    assert st == STATUS_OK and rd == data, f"read FAIL: {rd!r} != {data!r}"
    print("  [PASS] 基本 write/read 帧")

    # 2) 单笔超 4096B → ERR
    big = build_write_frame(0, b'X' * (MAX_SINGLE + 1))
    resp = m.process_frame(big)
    st, rv, _ = parse_response(resp)
    assert st == STATUS_ERR, f"超限应 ERR 实际 OK"
    print("  [PASS] 单笔超 4096B → ERR")

    # 3) STREAM 不受 4096 限制
    huge = b'Y' * 8192
    sf = build_write_frame(0x200, huge, stream=True)
    resp = m.process_frame(sf)
    st, rv, _ = parse_response(resp)
    assert st == STATUS_OK and rv == 8192, f"STREAM_WR FAIL"

    sr = build_read_frame(0x200, 8192, stream=True)
    resp = m.process_frame(sr)
    st, rv, rd = parse_response(resp)
    assert st == STATUS_OK and rd == huge, f"STREAM_RD FAIL"
    print("  [PASS] STREAM_WR/STREAM_RD 8KB 无 4K 限制")

    # 4) 越界
    bad = build_write_frame(m.size - 50, b'Z' * 100)
    resp = m.process_frame(bad)
    st, rv, _ = parse_response(resp)
    assert st == STATUS_ERR, f"越界应 ERR"
    print("  [PASS] 越界 → ERR")

    # 5) FLUSH
    resp = m.process_frame(build_flush_frame())
    st, rv, _ = parse_response(resp)
    assert st == STATUS_OK
    print("  [PASS] FLUSH → OK")

    # 6) 未知 op
    resp = m.process_frame(struct.pack('>BII', 99, 0, 0))
    st, rv, _ = parse_response(resp)
    assert st == STATUS_ERR
    print("  [PASS] 未知 op → ERR")

    # 7) 并发 (10 线程各写不同区域)
    import concurrent.futures
    def w(tid):
        frame = build_write_frame(tid * 100, f"T{tid}".encode())
        return parse_response(m.process_frame(frame))[0]
    with concurrent.futures.ThreadPoolExecutor(max_workers=10) as ex:
        results = list(ex.map(w, range(10)))
    assert all(r == STATUS_OK for r in results)
    print("  [PASS] 10 线程并发写 → 全 OK")

    print("=== axi_mem 内核自测试: ALL PASSED ===")


if __name__ == '__main__':
    _self_test()
