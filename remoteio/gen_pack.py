#!/usr/bin/env python3
"""生成自描述 pack.bin

格式：
  offset 0:   rio_pack_hdr_t   (256 bytes, magic="RIOPACK!", n_entries=N)
  offset 256: rio_pack_entry_t (144 bytes each, N 项)
  offset 256 + N*144: pack body (各虚拟文件内容)

每个 entry 的 offset 是 pack.bin 内的**绝对偏移**。
"""
import struct
import sys

# ---- 虚拟段定义 ----
# (虚拟路径, 大小, 初始内容)
# body 偏移会根据头部+entries 总大小自动计算
segments = [
    ("/virtual/config.txt",   64,   b"=== config.txt: hello pack ===\n"),
    ("/virtual/header.bin",   128,  b"HEADER_BIN_MAGIC\x00\x01\x02\x03\xde\xad\xbe\xef"),
    ("/virtual/records.log",  256,  b"records line 1\nrecords line 2\nrecords line 3\n"),
]

# ---- 头部格式 ----
HDR_SIZE   = 256
ENTRY_SIZE = 144
MAGIC      = b"RIOPACK!"
VERSION    = 1

def pack_hdr(n_entries: int) -> bytes:
    """rio_pack_hdr_t (256B)"""
    buf = bytearray(HDR_SIZE)
    buf[0:8]  = MAGIC
    struct.pack_into('<I', buf, 8,  VERSION)
    struct.pack_into('<I', buf, 12, n_entries)
    struct.pack_into('<Q', buf, 16, 0)    # reserved
    # pad[224] — 全零，已由 bytearray 初始化为 0
    return bytes(buf)

def pack_entry(vpath: str, offset: int, size: int) -> bytes:
    """rio_pack_entry_t (144B): vpath[128] + offset[8] + size[8]"""
    buf = bytearray(ENTRY_SIZE)
    # vpath, 限制 127 字节 + \0
    encoded = vpath.encode('utf-8')[:127]
    buf[0:len(encoded)] = encoded
    buf[len(encoded)]    = 0  # \0 结尾
    struct.pack_into('<Q', buf, 128, offset)
    struct.pack_into('<Q', buf, 136, size)
    return bytes(buf)

# ---- 计算 body 起始偏移 ----
body_start = HDR_SIZE + len(segments) * ENTRY_SIZE

# ---- 生成 entries + body ----
entries_bytes = b''
body_bytes    = b''
cur_offset    = body_start

print(f"pack.bin layout:")
print(f"  header  @ 0       size={HDR_SIZE}  bytes")
print(f"  entries @ {HDR_SIZE}    size={len(segments) * ENTRY_SIZE}  bytes ({len(segments)} x {ENTRY_SIZE})")
print(f"  body    @ {body_start}")

for vpath, size, content in segments:
    pad_len = size - len(content)
    if pad_len < 0:
        sys.exit(f"ERROR: {vpath} content {len(content)} > size {size}")

    # 生成 entry
    entries_bytes += pack_entry(vpath, cur_offset, size)

    # 生成 body
    body_bytes += content + b'X' * pad_len

    print(f"  {vpath}: offset={cur_offset}  size={size}  "
          f"content={len(content)} + pad={pad_len}  -> [{(content + b'X' * pad_len)[:20]!r}...]")
    cur_offset += size

pack_bin = pack_hdr(len(segments)) + entries_bytes + body_bytes

print(f"\npack.bin total = {len(pack_bin)} bytes")

# ---- 写文件 ----
import os
os.makedirs("rio_root", exist_ok=True)
with open("rio_root/pack.bin", "wb") as f:
    f.write(pack_bin)

# ---- 验证 ----
print("\nVerifying...")

# 1) 头部校验
with open("rio_root/pack.bin", "rb") as f:
    hdr = f.read(HDR_SIZE)
    assert hdr[0:8] == MAGIC, "BAD MAGIC"
    assert struct.unpack_from('<I', hdr, 8)[0] == VERSION, "BAD VERSION"
    n = struct.unpack_from('<I', hdr, 12)[0]
    assert n == len(segments), f"n_entries mismatch {n} vs {len(segments)}"
    print(f"  HEADER OK: magic={hdr[0:8]!r} version={struct.unpack_from('<I', hdr, 8)[0]} n_entries={n}")

# 2) entries 校验 + 独立 lseek/read 各段
with open("rio_root/pack.bin", "rb") as f:
    for i, (vpath, exp_size, exp_content) in enumerate(segments):
        # 读 entry —— 每次都从固定位置读
        f.seek(HDR_SIZE + i * ENTRY_SIZE)
        ent_raw = f.read(ENTRY_SIZE)
        e_vpath   = ent_raw[0:128].split(b'\0', 1)[0].decode('utf-8')
        e_offset  = struct.unpack_from('<Q', ent_raw, 128)[0]
        e_size    = struct.unpack_from('<Q', ent_raw, 136)[0]

        assert e_vpath == vpath, f"entry[{i}].vpath: {e_vpath} != {vpath}"
        assert e_size  == exp_size, f"entry[{i}].size: {e_size} != {exp_size}"

        # 按 entry 自己描述的 offset/size 去 seek+read
        f.seek(e_offset)
        actual = f.read(e_size)
        expected = exp_content + b'X' * (exp_size - len(exp_content))
        assert actual == expected, f"MISMATCH @ offset={e_offset} ({vpath})"
        print(f"  ENTRY[{i}] OK: {vpath}  offset={e_offset}  size={e_size}  [read verified]")

print("\nAll segments verified!")
