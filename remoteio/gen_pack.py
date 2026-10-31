#!/usr/bin/env python3
"""gen_pack.py — 自描述 pack 生成 + AXI 预填充桥

两种用法:
  1) 纯离线生成:  python3 gen_pack.py            → 写 rio_root/pack.bin + 本地 shm
  2) 远程预填充:  python3 gen_pack.py --host 10.0.0.1 --port 9527
                                                  → 通过 AXI 协议把 pack 写进远程 server 的 shm

核心流程 (两种模式共享):
  a) 用 axi_mem.AxiMemory 作为 pack 容器 (16MB 连续内存)
  b) 构造 rio_pack_hdr_t (256B) + rio_pack_entry_t[] + body
  c) 用 axi_mem 的 STREAM_WR (无 4K 限制) 一次性写进目标 shm

格式 (服务端 / 客户端 共享):
  offset 0:    rio_pack_hdr_t   (256B)
  offset 256:  rio_pack_entry_t (144B each, N 项)
  offset 256 + N*144: pack body  (各虚拟文件内容)

每个 entry 的 offset 是 shm 内**绝对偏移**。
"""

import struct, sys, os, socket, argparse

# ---- 引入 AXI 内核 ----
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from axi_mem import (
    AxiMemory, OP_STREAM_WR, OP_STREAM_RD, OP_FLUSH, OP_WRITE, OP_READ,
    STATUS_OK, STATUS_ERR, MAX_SINGLE, DEFAULT_SIZE,
    build_write_frame, build_read_frame, build_flush_frame,
    build_write_frame as build_stream_wr,  # 复用 (stream=False 用 4K 限)
    parse_response,
)

# ---- pack 格式常量 (和 rio_pack.h 精确对齐) ----
HDR_SIZE   = 256
ENTRY_SIZE = 144
MAGIC      = b"RIOPACK!"
VERSION    = 1
MAX_ENTRIES = 1024


def pack_hdr(n_entries: int, version: int = VERSION) -> bytes:
    """rio_pack_hdr_t (256B): magic[8] + version[4] + n_entries[4] + reserved[8] + pad[232]"""
    buf = bytearray(HDR_SIZE)
    buf[0:8] = MAGIC
    struct.pack_into('<I', buf, 8,  version)
    struct.pack_into('<I', buf, 12, n_entries)
    struct.pack_into('<Q', buf, 16, 0)
    return bytes(buf)


def pack_entry(vpath: str, offset: int, size: int) -> bytes:
    """rio_pack_entry_t (144B): vpath[128] + offset[8] + size[8] (全小端)"""
    buf = bytearray(ENTRY_SIZE)
    encoded = vpath.encode('utf-8')[:127]
    buf[0:len(encoded)] = encoded
    buf[len(encoded)] = 0
    struct.pack_into('<Q', buf, 128, offset)
    struct.pack_into('<Q', buf, 136, size)
    return bytes(buf)


def build_pack(segments: list) -> bytes:
    """把 segments [(vpath, size, content), ...] 打包成一个连续 bytes。"""
    n = len(segments)
    body_start = HDR_SIZE + n * ENTRY_SIZE

    # 1) header
    result = bytearray()
    result += pack_hdr(n)

    # 2) entries + body 交错构建
    body_off = body_start
    for vpath, size, content in segments:
        pad_len = size - len(content)
        if pad_len < 0:
            raise ValueError(f"{vpath}: content({len(content)}) > size({size})")
        result += pack_entry(vpath, body_off, size)
        body_off += size

    # 3) body — 现在 header + entries 已经写好了, 追加 body
    body_data = bytearray()
    for vpath, size, content in segments:
        pad_len = size - len(content)
        body_data += content + b'X' * pad_len

    # 确保 result 够大 (包含 header+entries), 然后填入 body
    expected_len = body_start + len(body_data)
    if len(result) < body_start:
        raise RuntimeError(f"bug: result={len(result)} < body_start={body_start}")
    result += body_data

    print(f"[pack] header @ 0       size={HDR_SIZE}")
    print(f"[pack] entries @ {HDR_SIZE:<6} size={n * ENTRY_SIZE}  ({n}x{ENTRY_SIZE})")
    print(f"[pack] body    @ {body_start:<6} size={len(body_data)}")
    for i, (vpath, size, content) in enumerate(segments):
        print(f"  [{i}] {vpath:<28} offset={body_start + sum(s[1] for s in segments[:i]):>6}  size={size:>4}  "
              f"content={len(content)} + pad={size - len(content)}")
    print(f"[pack] total = {len(result)} bytes")

    return bytes(result)


# ---- 远程 AXI 桥 ----

class AxiRemoteBridge:
    """通过 TCP AXI 帧访问远程 server 的 shm。"""

    def __init__(self, host: str, port: int):
        self.host = host
        self.port = port
        self.sock = socket.socket()
        self.sock.settimeout(10)
        self.sock.connect((host, port))

    def close(self):
        self.sock.close()

    # ---- 底层: 先收 5B 响应头 ----
    def _recv_hdr(self):
        hdr = b''
        while len(hdr) < 5:
            c = self.sock.recv(5 - len(hdr))
            if not c: raise ConnectionError("connection closed mid-header")
            hdr += c
        return struct.unpack('>BI', hdr)  # (status, ret_val)

    # ---- WRITE / STREAM_WR: 发送帧+data, 只收 5B 响应 (无额外 data) ----
    def _write_op(self, op: int, addr: int, data: bytes):
        frame = struct.pack('>BII', op, addr, len(data)) + data
        self.sock.sendall(frame)
        return self._recv_hdr()   # (status, ret_val)

    # ---- READ / STREAM_RD: 发送帧, 收 5B 响应 + 可能的 data ----
    def _read_op(self, op: int, addr: int, n: int) -> bytes:
        frame = struct.pack('>BII', op, addr, n)
        self.sock.sendall(frame)
        st, rv = self._recv_hdr()
        if st != STATUS_OK:
            raise RuntimeError(f"AXI op={op} fail: status={st} ret=0x{rv:02x}")
        if rv == 0:
            return b''
        data = b''
        while len(data) < rv:
            c = self.sock.recv(rv - len(data))
            if not c: raise ConnectionError("connection closed mid-data")
            data += c
        return data

    def write_block(self, addr: int, data: bytes) -> int:
        """用 STREAM_WR 写 (无 4K 限制)。自动分块, 每块 64KB。"""
        CHUNK = 64 * 1024
        total = 0
        for i in range(0, len(data), CHUNK):
            chunk = data[i:i + CHUNK]
            st, rv = self._write_op(OP_STREAM_WR, addr + i, chunk)
            if st != STATUS_OK:
                raise RuntimeError(f"AXI STREAM_WR fail: status={st} ret=0x{rv:02x}")
            total += rv
        return total

    def read_block(self, addr: int, n: int) -> bytes:
        return self._read_op(OP_STREAM_RD, addr, n)

    def flush(self) -> bool:
        self.sock.sendall(struct.pack('>BII', OP_FLUSH, 0, 0))
        st, _ = self._recv_hdr()
        return st == STATUS_OK


# ================================================================
# 主程序
# ================================================================

def main():
    # ---- 定义虚拟段 (和 test_pack_client.c 对齐) ----
    segments = [
        ("/virtual/config.txt",   64,  b"=== config.txt: hello pack ===\n"),
        ("/virtual/header.bin",   128, b"HEADER_BIN_MAGIC" + b"\x00\x01\x02\x03\xde\xad\xbe\xef"),
        ("/virtual/records.log",  256, b"records line 1\nrecords line 2\nrecords line 3\n"),
    ]

    # ---- 生成 pack 二进制 ----
    pack_bin = build_pack(segments)

    parser = argparse.ArgumentParser(description="gen_pack: pack generate + AXI bridge")
    parser.add_argument("--host", help="远程 server 地址 (通过 AXI 帧预填充)")
    parser.add_argument("--port", type=int, default=9527, help="远程 port (default 9527)")
    parser.add_argument("--no-local-shm", action="store_true", help="不写本地 /dev/shm")
    parser.add_argument("--bin-dir", default="rio_root", help="pack.bin 输出目录")
    args = parser.parse_args()

    # ---- 离线: 写 pack.bin ----
    os.makedirs(args.bin_dir, exist_ok=True)
    bin_path = os.path.join(args.bin_dir, "pack.bin")
    with open(bin_path, "wb") as f:
        f.write(pack_bin)
    print(f"[pack.bin] 写入 {bin_path} ({len(pack_bin)} bytes)")

    # ---- 本地 shm (可选) ----
    if not args.no_local_shm:
        try:
            import mmap
            shm_path = "/dev/shm/rio_packshm_srv"
            fd = os.open(shm_path, os.O_RDWR | os.O_CREAT | os.O_TRUNC, 0o644)
            try:
                os.ftruncate(fd, DEFAULT_SIZE)
                mm = mmap.mmap(fd, DEFAULT_SIZE)
                mm.seek(0)
                mm.write(pack_bin)
                # 剩余 16MB - len(pack_bin) 填 0
                mm.write(b'\x00' * (DEFAULT_SIZE - len(pack_bin)))
                mm.flush()
                mm.close()
                print(f"[local shm] {shm_path} written ({len(pack_bin)} bytes pack + "
                      f"{DEFAULT_SIZE - len(pack_bin)} zero)")
            finally:
                os.close(fd)
        except ImportError:
            print("[local shm] mmap not available, skip")
        except Exception as e:
            print(f"[local shm] 跳过: {e}")

    # ---- 远程预填充 (如果指定 --host) ----
    if args.host:
        print(f"\n[remote] 连接 {args.host}:{args.port} ...")
        bridge = AxiRemoteBridge(args.host, args.port)
        try:
            wrote = bridge.write_block(0, pack_bin)
            print(f"[remote] AXI STREAM_WR @ addr=0 → {wrote} bytes pack body")

            # 验证: 回读 header 确认写对了
            hdr_back = bridge.read_block(0, HDR_SIZE)
            assert hdr_back[:8] == MAGIC, "remote magic mismatch!"
            n_entries = struct.unpack_from('<I', hdr_back, 12)[0]
            assert n_entries == len(segments), f"remote n_entries mismatch: {n_entries}"
            print(f"[remote] 回读验证 OK: magic={hdr_back[:8]!r} n_entries={n_entries}")

            bridge.flush()
            print("[remote] FLUSH OK")
        finally:
            bridge.close()
        print("[remote] 完成 — 远程 shm 已预填充 pack 数据")

    # ---- 验证 ----
    print("\n=== 验证 pack.bin ===")
    with open(bin_path, "rb") as f:
        # header
        hdr = f.read(HDR_SIZE)
        assert hdr[:8] == MAGIC, "BAD MAGIC"
        assert struct.unpack_from('<I', hdr, 8)[0] == VERSION, "BAD VERSION"
        n = struct.unpack_from('<I', hdr, 12)[0]
        assert n == len(segments), f"n mismatch: {n} vs {len(segments)}"
        print(f"  HEADER OK: magic={hdr[:8]!r} version={struct.unpack_from('<I', hdr, 8)[0]} n={n}")

        # entries + body
        for i, (vpath, exp_size, exp_content) in enumerate(segments):
            f.seek(HDR_SIZE + i * ENTRY_SIZE)
            ent = f.read(ENTRY_SIZE)
            e_vpath = ent[:128].split(b'\0', 1)[0].decode('utf-8')
            e_offset = struct.unpack_from('<Q', ent, 128)[0]
            e_size   = struct.unpack_from('<Q', ent, 136)[0]
            assert e_vpath == vpath, f"entry[{i}].vpath mismatch"
            assert e_size == exp_size, f"entry[{i}].size mismatch"

            f.seek(e_offset)
            actual = f.read(e_size)
            expected = exp_content + b'X' * (exp_size - len(exp_content))
            assert actual == expected, f"BODY MISMATCH @ {vpath}"
            print(f"  ENTRY[{i}] OK: {vpath}  offset={e_offset}  size={e_size}")

    print("\nAll entries verified!")


if __name__ == "__main__":
    main()
