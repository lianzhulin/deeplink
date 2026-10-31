#!/usr/bin/env python3
"""精确生成 pack.bin —— 每个段的 offset/size 与 rio_pack.ini 一致"""

# 段定义（必须和 rio_pack.ini 完全匹配）
segments = [
    # (虚拟路径, offset, size, 填充内容)
    ("/virtual/config.txt",  0,    64,   b"=== config.txt: hello pack ===\n"),
    ("/virtual/header.bin",  64,   128,  b"HEADER_BIN_MAGIC\x00\x01\x02\x03\xde\xad\xbe\xef"),
    ("/virtual/records.log", 192,  256,  b"records line 1\nrecords line 2\nrecords line 3\n"),
]

total = 0
for vpath, off, size, content in segments:
    actual = len(content)
    pad_len = size - actual
    if pad_len < 0:
        raise SystemExit(f"ERROR: {vpath} content {actual} > size {size}")
    padded = content + b'X' * pad_len
    print(f"  {vpath}: offset={off:3d} size={size:3d}  content={actual:2d} + pad={pad_len:2d}  "
          f"-> [{padded[:20]}...]")
    assert off == total, f"Gap detected! total={total} but offset={off}"
    total += size

pack_bin = b''.join(c + b'X' * (s - len(c)) for _, _, s, c in segments)
print(f"\npack.bin total = {len(pack_bin)} bytes")

with open("rio_root/pack.bin", "wb") as f:
    f.write(pack_bin)

# 验证各段可以独立 lseek+read
for vpath, off, size, content in segments:
    with open("rio_root/pack.bin", "rb") as f:
        f.seek(off)
        data = f.read(size)
        expected = content + b'X' * (size - len(content))
        assert data == expected, f"MISMATCH at {vpath}!"
        print(f"  VERIFIED {vpath} @ offset={off}: OK")

print("\nAll segments verified!")
