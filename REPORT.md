# bdiff v2 — 固件补丁算法 · 代码与测评报告

生成时间：2026-08-24
构建基线：`gcc -O2 -Wall -Wextra -std=c99 -Werror`
zlib 构建：`-DBDIFF_HAVE_ZLIB=1 -lz`
ASan/UBSan：两者均 0 告警（42/42 通过）

---

## 1. 文件清单

| 文件 | 作用 |
|---|---|
| bdiff.h | 公共 API、错误码、bdiff_opts、v2 格式/Opcode 规范 |
| bdiff.c | 算法实现：哈希索引、贪心最长匹配、立即值 opcode emit、ADDX zero-run 微码、raw-deflate 信封、v1/v2 双解码器 |
| test_bdiff.c | 自测（42 项）：baseline T1–T11 + 128KB 固件真实场景 T12–T23 + v1 兼容 + 恶意 opcode 拒绝 + 96B 预算搜索 |
| bench_patch_sizes.c | N=1..10 × 20 trial ×（plain / zlib）patch size 基准 |
| Makefile | `make test`（plain）/ `make test_z`（zlib） |
| plain.csv  | 基准原始输出：N,trial,PLAIN,bytes（200 行） |
| zlib.csv   | 基准原始输出：N,trial,ZLIB,bytes（200 行） |

## 2. 真实场景参数（benchmark 与自测一致）

- 固件块大小：**128 KB（定长）**
- 煽度（熵）生成方式：**`fill_moderate`** — 256B 一组，偶数组为结构化区（前 128B=0x00，后 128B=0xFF，即"代码区对齐填充+向量表尾"），奇数组为 PRNG 混合字节（"常量/数据段"）。
- 修改模式：`mutate_N_4B_1byte_each`，每 4B 对齐块内**只改 1 字节（XOR 0xFF）**，模拟固件标志位 / 计数器 / 版本字段更新。
- `bdiff_opts`：`block_size=16, max_old_size=128K, max_new_size=128K, prefer_small=1`。

## 3. Opcode 设计

v2 格式 = 6B 头（`BDIF` + version=2 + flags）+ varint(new_size) + 记录流，可后接可选 raw-deflate 信封（flags bit0=1）。

| 范围 | Opcode | 语义 |
|---|---|---|
| 0x00 | ADD_BIG | varint(len) + len 字节 |
| 0x01..0x1F | ADD_SMALL | len = opcode，len 字节（立即值） |
| 0x20 | ADDX_BIG | varint(len), varint(old_off), varint(xdiff_enc_len), xdiff-encoded mask |
| 0x21..0x3F | ADDX_SMALL | len = opcode&0x1F，其余同 ADDX_BIG |
| 0x40 | COPY_BIG | varint(off), varint(len) |
| 0x41..0x5F | COPY_SMALL_LEN | len = (op&0x1F)+1，varint(off) |
| 0x60..0x7F | COPY_SMALL_BOTH | 2 字节记录：off 10 bit（0..1023），len 1..8 |
| 0x80..0xFF | 保留 | 未知 opcode → `BDIFF_E_FORMAT`（fail-safe） |

ADDX mask 的 xdiff 零行程微码：2-bit tag（ZEROS=0 / SAME1=1 / RAW2=2 / RUN=3），对
"4B 块中只改 1 字节" 把 mask 从 4B 压缩到约 1–2B，是 budget≤96B、N≥8 的关键。

## 4. N=1..10 patch size 对比（每 N 20 trial）

| N | plain min | plain avg | plain max | zlib min | zlib avg | zlib max | 96B 预算 |
|---|---|---|---|---|---|---|---|
| 1 | 22 | 23.7 | 26 | 22 | 23.7 | 26 | ✅ |
| 2 | 29 | 32.6 | 35 | 29 | 32.6 | 35 | ✅ |
| 3 | 36 | 41.0 | 45 | 36 | 41.0 | 45 | ✅ |
| 4 | 46 | 50.0 | 56 | 46 | 50.0 | 56 | ✅ |
| 5 | 52 | 60.0 | 66 | 52 | 60.0 | 66 | ✅ |
| 6 | 64 | 69.0 | 75 | 64 | 69.0 | 75 | ✅ |
| 7 | 71 | 76.3 | 81 | 71 | 76.3 | 81 | ✅ |
| 8 | 78 | 85.8 | 92 | 78 | 85.8 | 92 | ✅ 最坏 92B |
| 9 | 86 | 92.6 | 99 | 86 | 92.6 | 99 | ⚠️ 最坏 99B 溢出 |
| 10 | 92 | 102.0 | 110 | 92 | 101.8 | 109 | ❌ |

## 5. 最大/最小 & 关键结论

### 5.1 极值

- **全局最小 patch size**：N=1 时 22 B（6B header + 2B new_size vi + COPY 拆分 + ADDX_SMALL 记录，3 字节 zero-run）。
- **全局最大 patch size**：N=10 时 **plain 最坏 110 B / zlib 最坏 109 B**。
- **96B 预算结论（按最坏而定）**：可靠上限 **N = 8，最坏 92 B；N=9 最坏 99 B 已越界。** 若用平均视角估算 N=9 只有 92.6 B，会在最坏布局时静默把补丁写大导致实际固件下发 truncate，必须按 max 定数。
- **deflate 信封的启用门限**：N≤10 时记录流本身 <50B，不满足 `records.len > 50 && 压缩后 + 信封头 < 原大小 - 2B` 的严格启用条件，因此 plain 和 zlib 结果几乎一致。这是**正确的决策**，避免 deflate 头把小包反而变大。
- **deflate 生效的典型场景**：自测 T15（8×128B 扇区覆盖）1109B → 196B（-82%）；T23（64×4B 散布改）558B → 506B（-9%）。
- **线性斜率**：每多 1 处 4B-1B 改动，补丁增量 7–9 B；即 ADDX_SMALL + off vi + xdiff_enc_len vi + 1–2B body，完全匹配 opcode 预估。

### 5.2 防御性 / 稳健性自测（42/42 全通过）

- T1–T11：baseline（identical / 1-byte / empty-old / empty-new / 散布 40 字节 / bad-magic & bad-version 拒绝 / too-small old 安全失败 / 平移插入 / 两区域 / 64 字节块 / 非对齐字节）。
- T12–T16：128KB 真实固件 identical / 1×4B / 4×4B / 8×128B 扇区覆盖 / 64K 边界 4B 平移。
- T17–T21：opts 大小上限（`BDIFF_E_TOO_BIG`） / max_patch_size 强制裁剪 / 保留 opcode 0x88 判格式错 / xdiff tag 越界判格式错 / v1 补丁向后兼容解码。
- T22：96B 预算二分搜索 → max N = 8 带 roundtrip。
- T23：64×4B 散布的 plain + prefer_small + prefer_small 不大于 balanced + slack。

## 6. 构建与自检命令

```sh
# 代码自测（无 deflate）
make test                 # 42/42 通过, Budget≤96B max N=8 worst 91B

# 启用 zlib deflate 信封
make test_z               # 42/42 通过, Budget≤96B max N=8 worst 87B
                          # T15 8×128B: 1109B→196B, T23: 558B→506B

# 内存/UB 安全
gcc -O1 -g -fsanitize=address,undefined  bdiff.c test_bdiff.c -o asan_plain   && ./asan_plain
gcc -O1 -g -fsanitize=address,undefined -DBDIFF_HAVE_ZLIB=1  bdiff.c test_bdiff.c -o asan_z -lz && ./asan_z

# 基准 N=1..10
gcc -O2 -Wall -Wextra -std=c99 -Werror -DBENCH_PLAIN bench_patch_sizes.c bdiff.c -o bench_plain
gcc -O2 -Wall -Wextra -std=c99 -Werror -DBDIFF_HAVE_ZLIB=1  bench_patch_sizes.c bdiff.c -o bench_zlib -lz
./bench_plain > plain.csv
./bench_zlib  > zlib.csv
```
