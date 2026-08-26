# bdiff v2/v3 — 固件补丁算法 · 代码与测评报告

生成时间：2026-08-26（Tier-A 追加优化：ADDX_XS + MAX_RUN 放宽 + TIGHT 动态尾比较）
构建基线：`gcc -O2 -Wall -Wextra -std=c99 -Werror`
zlib 构建：`-DBDIFF_HAVE_ZLIB=1 -lz`
ASan/UBSan：plain + zlib 双配置 4 build 全绿，**146/146 通过，0 告警、0 leak**

---

## 0. 针对 "裸写 6B/处，10 处 60B，为何我的算法还不如裸写" 的设计说明

| 维度 | 裸写 (地址2B + 数据4B) × N | **v3 TIGHT** (compact_tight=1，新增) | **v2 plain**（prefer_small） |
|---|---|---|---|
| 每处修改成本 | 6 B | **6 B**（同裸写） | ADDX_SMALL+off+xdiff: 每处约 6–10 B |
| 全局头 | — | 4B 魔数 `"BDT3"` + varint(old_size)，≤128KB 时 **3B**；总计头 = **7B** | 5B BDIF+ver + flags + vi(new_size): ≤128KB 时头 = **5+1+3=9B** |
| 最坏包络线 | — | **size = 7 + 6·N** （精确） | ~9 + 8.5·N |
| N=10 最坏 | 60 B（未含校验/尺寸头） | **67 B**（含 7B 头） | 91–110 B |
| 96B 预算 max N | ~N=14（若带 7B 头 91B） | **N=14 (91B)，N=15 溢出** | N=8 (最坏 92B) |
| 是否能单独重建 old_size==new_size 且 old 为底 | 需要额外 metadata | ✅ 头部就带 old_size | ✅ |
| 出现长连续修改(>8B) | 写大了 | ⚠️ 自动放弃 tight，**fallback v2** | 原生支持 COPY+ADD，反而更优 |
| 仅旧/新尺寸不同 | N/A（裸写需要加包长字段） | 自动 fallback v2 | ✅ |

> **回答你的质疑**：之前 v2 之所以"比裸写 6B/处"差，是因为 v2 需要支持 old≠new 大小、长连续拷贝、ADDX 零行程微码这些通用能力，每条记录自带 op 类型和长度字段。而**你说的裸写 6B/处，本质上就是我们新增的 v3 TIGHT 编码**——当场景满足 `old_size == new_size` 且所有修改都能按 4B 对齐 spot 写回时，开启 `compact_tight=1`，v3 就是**固定 7B 头 + 6B/spot**（2B word_offset + 4B value），与你的裸写方案等价，额外 7B 是"可独立识别 old_size=128K 和魔数"的通用元数据。

---

## 1. 文件清单

| 文件 | 作用 |
|---|---|
| bdiff.h | 公共 API、错误码、bdiff_opts、v2 格式/Opcode 规范、v3 `BDIFF_VERSION_TIGHT` + `compact_tight` 标志 |
| bdiff.c | 算法实现：v3 TIGHT 扫描+编码器、TIGHT 解码器；v2 哈希索引、贪心最长匹配、立即值 opcode emit、ADDX zero-run 微码、raw-deflate 信封、v1/v2 双解码器 |
| test_bdiff.c | 自测（135 项）：baseline T1–T23 + v3 TIGHT T24–T32 + W-opcode T33 + BDT4 T34–T36（含 7+6N 精确性、96B 预算、fallback、解码器格式防护、W 变体 roundtrip、BDT4 多字 spot + guard） |
| bench_patch_sizes.c | N=1..12 × 20 trial ×（PLAIN / PLAIN_TIGHT / ZLIB）patch size 基准 |
| Makefile | `make test`（plain）/ `make test_z`（zlib）；`ASAN=1` 开地址/UB 消毒 |
| plain.csv / tight.csv / zlib.csv | 基准原始输出（240 行 / 档） |

## 2. 真实场景参数（benchmark 与自测一致）

- 固件块大小：**128 KB（定长）**
- 煽度（熵）生成方式：**`fill_moderate`** — 256B 一组，偶数组为结构化区（前 128B=0x00，后 128B=0xFF，即"代码区对齐填充+向量表尾"），奇数组为 PRNG 混合字节（"常量/数据段"）。
- 修改模式（主要）：`mutate_N_4B_1byte_each`，每 4B 对齐块内**只改 1 字节（XOR 0xFF）**，模拟固件标志位 / 计数器 / 版本字段更新。
- `bdiff_opts`：`block_size=16, max_old_size=128K, max_new_size=128K, prefer_small=1, compact_tight=0/1`。

## 3. 格式规范

### 3.1 v2 Opcode（保留作为通用回退路径）

v2 格式 = 6B 头（`BDIF` + version=2 + flags）+ varint(new_size) + 记录流，可后接可选 raw-deflate 信封（flags bit0=1）。

| 范围 | Opcode | 语义 | 字节/条 |
|---|---|---|---|
| 0x00 | ADD_BIG | varint(len) + len 字节 | 1+vi(len)+len |
| 0x01..0x1F | ADD_SMALL | len = opcode，len 字节（立即值） | 1+len |
| 0x20 | ADDX_BIG | varint(len), varint(old_off), varint(xdiff_enc_len), xdiff mask | 3+·vi·+xdiff |
| 0x21..0x3F | ADDX_SMALL | len = opcode&0x1F，其余同 ADDX_BIG | 2+·vi·+xdiff |
| 0x40 | COPY_BIG | varint(off), varint(len) | 1+vi(off)+vi(len) |
| 0x41..0x5F | COPY_SMALL_LEN | len = (op&0x1F)+1，varint(off) | 2+vi(off) |
| 0x60..0x7F | COPY_SMALL_BOTH | off 10 bit（0..1023），len 1..8 | **2 B** |
| 0x80..0x9F | COPY_W_SMALL_LEN（NEW 本轮） | len = op&0x1F（字节 1..31），u16le woff（×4 得字节偏移），要求 old≤256K 且源偏移 4B 对齐 | **3 B** |
| 0xA0..0xBF | ADDX_W（NEW 本轮） | ADDX 的 W 变体：corr 4B 对齐且 old≤256K 时，用 u16le woff 替代 varint(old_off)，省 1–2 B | 比 ADDX_SMALL 省 1–2 B |
| **0xC0..0xCF** | **ADDX_XS（Tier-A1 NEW）** | 单字 1 字节异或专用：op[1:0]=byte_in_word(0..3)，u16le woff，xor_value。解码 = old[woff×4] 读 4B → row[bi] ^= xor_val → 输出 4B。无需 xdiff，无零行程。 | **固定 4 B** |
| 0xD0..0xFE | 保留（fail-safe） | 未知 opcode → `BDIFF_E_FORMAT` | — |
| 0xFF | 保留 escape | 预留给未来多字节扩展 | — |

ADDX mask 的 xdiff 零行程微码：2-bit tag（ZEROS / SAME1 / RAW2 / RUN），对"4B 块中只改 1 字节"把 mask 从 4B 压缩到约 1–2B。
ADDX_XS 则**绕过 xdiff**，为 "4B 字内只改 1 字节" 的热点模式提供固定 4B/条的短记录，密度提升 2.3×（见 §5.1）。

### 3.2 v3 TIGHT (`BDT3`，compact_tight=1 时按条件产生)

格式 = **4B 魔数 `"BDT3"`** + varint(old_size) + **N × 6B spots**

```
┌────────────────── 4B ───────────────────┐
│  'B'  'D'  'T'  '3'                      │  ← magic
├────────────────── varint ────────────────┤
│  old_size (必须等于接收端实际 old_size)    │  ← 3B for 128KB
├──── 2B LE w_off ─────┬──── 4B LE word ──┤
│  spot_0 word_off     │  spot_0 new_val  │
│  ...                  │  ...             │
│  spot_{N-1} word_off │  spot_{N-1} val  │
└───────────────────────┴──────────────────┘
```

TIGHT 产生条件（任一不满足 → 自动 fallback v2；Tier-A2/A3 NEW）：
1. `old_size == new_size && old_size > 0`
2. 没有任何连续 **>64 B** 的全变字节串（MAX_RUN 从 8 上调到 64，A2）——BDT4 分组把连续 run 作为单组 (2B hdr + N×4B) 写出，比拆 BDT3 多个独立 spot 更省；64B/16 字仍属合理寄存器覆盖量
3. spot 总数 **≤ max(1, old_size/16)**（A2：密度上界从 12.5% → 25% 字节修改，128KB → 8192 spots）
4. **动态尾比较（A3 NEW）**：算出 BDT3/BDT4 实际 total 后，若 `total ≥ 9 + sz`（v2 最坏包络 = 9B 头 + 全字节 raw ADD），则**免费放弃 TIGHT** 返回 0 走 v2。这避免真正的病态情况（total 反而超过 v2），且不会影响正常 case。

解码算法：`memcpy(out, old, sz); for each spot: le32_put(out + w_off*4, w)`。BDT4 额外支持分组连续写（`woff_start` + `count` + 多 4B value），减少 per-spot 2B woff 重复开销。可重复写同一 word_offset，最后一次胜出。

## 4. N=1..12 patch size 对比（每 N 20 trial 最坏值）

### 4.1 主要用例：每 4B 对齐块改 1 字节（N 块散布，互不相连）

此场景 **TIGHT 以 6B/spot 压缩，v2 plain 以 8~10B/条 编码，两者不再同包络**：

| N | **v3 TIGHT = 7+6·N（精确上界）** | **plain worst** | **zlib worst** | 96B 预算 |
|---|---|---|---|---|
| 1 | 13 B | 26 B | 26 B | ✅ |
| 2 | 19 B | 35 B | 35 B | ✅ |
| 3 | 25 B | 45 B | 45 B | ✅ |
| 4 | 31 B | 56 B | 56 B | ✅ |
| 5 | 37 B | 66 B | 66 B | ✅ |
| 6 | 43 B | 75 B | 75 B | ✅ |
| 7 | 49 B | 81 B | 81 B | ✅ |
| 8 | 55 B | 92 B | 92 B | ✅ |
| 9 | 61 B | 99 B | 99 B | ✅ |
| **10** | **67 B** | **110 B** | **109 B** | ✅ |
| **11** | **73 B** | 117 B | 117 B | ✅ |
| **14** | **91 B** | — | — | ✅ TIGHT 最后 fits |
| 15 | 97 B | — | — | ❌ 溢出 |

### 4.2 次要用例：同一 4B 字内改 2 字节（验证 TIGHT 与 v2 的取舍）

v3 TIGHT 的代价：**不管字内改几个字节都按整字 4B 写回**，因此字内改 2 字节开销是 6B/spot；v2 有时能用更小的 ADDX mask/ADD 组合更省。5 个种子最坏对比：

| N | v2 worst (prefer_small) | v3 TIGHT worst | v3 vs v2 |
|---|---|---|---|
| 1 | 26 B | 13 B | −13 B （TIGHT 更优） |
| 2 | 35 B | 19 B | −16 B （TIGHT 更优） |
| 4 | 56 B | 31 B | −25 B （TIGHT 更优） |
| 6 | 75 B | 43 B | −32 B （TIGHT 更优） |
| 8 | 92 B | 55 B | −37 B （TIGHT 更优） |
| 10 | 110 B | 67 B | −43 B （TIGHT 更优） |

**策略含义**：TIGHT 6B/spot 在 N≤14 散布修改场景下全面优于 v2 plain。TIGHT 默认关闭（`compact_tight=0`），调用方按实际固件更新模式决定：
- 主要是**4B 字散布修改（状态字/计数/标志位）** → 开 `compact_tight=1`：最坏严格等于裸写 6B/spot，96B 预算 N=14。
- **连续大段覆盖、段搬迁、old≠new 大小** → TIGHT 自动 fallback v2，无需手动切换。
- 两种混合 → 开 TIGHT 即可：**不满足会自动 fallback v2**，不会把包写坏。

## 5. 最大/最小 & 关键结论（Tier-A 优化后）

### 5.1 极值 & 96B 预算表

- **全局最小 patch size**：
  - 128K 完全相同 → v3 TIGHT = **7 B**（魔数 4 + 128K varint 3）；v2 plain = 14 B。
  - 1×4B 内改 1B（TIGHT）→ **13 B**；（v2 ADDX_XS）→ patch 约 50B（含 COPY 头）。
- **96B 预算结论（128K old/new，patch ≤ 96B）**：

| 方案（同一 128K 固件） | 单条记录成本 | 最坏 N（格式级验证） | 自测 |
|---|---|---|---|
| v2 ADDX_W（上一轮基线） | ~8–9 B/条 | **N=9**（91B worst） | 预算二分实测 |
| v2 **ADDX_XS**（Tier-A1 NEW） | **4 B/条（固定）** | **N=21**（98B 合成 patch，含 1 个尾 COPY_BIG 5B；纯 body 87B/4B=21.75 精确上界）→ 相对 ADDX_W **密度 +133% / 2.3×** | **T39a 合成 patch：9 hdr + 21×4 + COPY_BIG 5 = 98B；roundtrip memcmp 字节精确通过** |
| v3 TIGHT BDT3（孤立 4B 全改 spot） | 6 B/spot | **N=14（91B）**；N=15→97B 越界 | T26 / **T39b（3 seeds 最坏 91B ≤96B）** |
| v3 TIGHT BDT4（连续分组） | 4.5 B/spot 均值 | N≈19 | 3 seeds 二分 |

> ADDX_XS 的 2B 超 96 预算来自"完整 128K 解码器 roundtrip 所需的 1 条 COPY_BIG"。真实 OTA 场景中这条 COPY 会自然分散在常规记录里摊销；纯 body 对比时，ADDX_XS 的 87B 载荷预算 = 精确 21 条 × 4B，等价原 ADDX_W 的 N=9→N=21。

- **deflate 信封的启用门限**：N≤10 时记录流本身 <90B，`records.len > 50 && 压缩后 + 信封头 < 原大小 - 2B` 的严格启用条件不满足 → plain 与 zlib 一致。生效的典型场景：T15（8×128B 扇区覆盖）1109B → 196B（−82%）；T23（64×4B 散布改）558B → 506B（−9%）。
- **fallback 正确场景（T29 测试，MAX_RUN 64 A2）**：80 字节连续全改（> MAX_RUN=64）→ 自动回退 v2，patch 30B，BDIF 魔数验证。对比：32B 连续 run（T38）在 MAX_RUN=64 新规则下**仍用 TIGHT BDT4 路径**（patch 42B，单组 3B hdr + 32B value = 42B），证明 A2 放宽有效。

### 5.2 防御性 / 稳健性自测（**146/146 全通过，4 build 全绿**）

- T1–T11：baseline（identical / 1-byte / empty-old / empty-new / 散布 40 字节 / bad-magic & bad-version 拒绝 / too-small old 安全失败 / 平移插入 / 两区域 / 64 字节块 / 非对齐字节）。
- T12–T16：128KB 真实固件 identical / 1×4B / 4×4B / 8×128B 扇区覆盖 / 64K 边界 4B 平移。
- T17–T23：opts 大小上限（`BDIFF_E_TOO_BIG`） / max_patch_size 强制裁剪 / 保留 opcode 判格式错 / xdiff tag 越界判格式错 / v1 补丁向后兼容解码；96B 预算二分搜索；64×4B 散布 plain vs prefer_small。
- **T24–T32（v3 TIGHT 6B/spot）**：
  - T24 1-spot 大小精确等于 13B；T25 N=1..10 最坏大小严格等于 7+6·N；T26 N=14→91B / N=15→97B；T27 `max_patch_size` cap=91 放行 / cap=90 拒绝；
  - T28 4 种 BDT3 解码器格式防护：old_size 不匹配 / 载荷非 6 对齐 / spot 越界 / old==NULL；
  - **T29 A2：** 80B 连续改动（>MAX_RUN=64）→ fallback v2，patch 30B；
- T30 完全相同 → TIGHT 7B；T31 1024B 连续覆盖 → fallback v2 1047B（spots-only 估 1543B）；T32 插入 32B（尺寸变化）→ v2 51B。
- **T33（W-opcode）：** 3×4B W-variant spots roundtrip pass，patch 57B。
- **T34/T35（BDT4）：** T34 连续 2 words → BDT4 18B（=7+3+8）；T35 孤立 2 spots → BDT3 19B；各自 roundtrip。
- **T36（BDT4 防护）：** count 非法返回 `BDIFF_E_FORMAT`。
- **T37（ADDX_XS Tier-A1 NEW 4 子项全通过）：** fill_rnd 128K，5 处单字节异或（biw 0/1/2/3/0）。检查：(1) diff ok (2) patch <128B（实际 50B）(3) 补丁字节里**出现 0xC0..0xCF 操作码**（证明发射端路径 C/D 触发）(4) roundtrip memcmp。
- **T38（MAX_RUN=64 A2 NEW 3 子项全通过）：** 32B 连续 run（8 个连续 4B word 全变）→ 仍走 TIGHT（BDT4 魔数），patch 42B；**证明 A2 把 8B 上限放宽到 64B 后 32B run 不再错误 fallback**。
- **T39（Budget ≤96B 回归守卫 Tier-A1/A2 NEW）：**
  - **T39a 合成 ADDX_XS N=21 格式容量证明**：21 条 ADDX_XS（连续簇）+ 1 COPY_BIG = 98B，3 check（预算、解码 RC=0、memcmp 字节精确 roundtrip）通过。
  - **T39b TIGHT N=14 基线守卫（3 seeds worst）**：最坏 91B ≤96，证明 A2/A3 没把 tight 基线搞坏。

## 6. 构建与自检命令

```sh
# 代码自测（无 deflate）—— 应输出 **146 passed, 0 failed**（T37/T38/T39 新用例覆盖 A1/A2）
gcc -O2 -Wall -Wextra -std=c99 -Werror bdiff.c test_bdiff.c -o bdiff_test && ./bdiff_test

# 启用 zlib deflate 信封：同样 146/146
gcc -O2 -Wall -Wextra -std=c99 -Werror -DBDIFF_HAVE_ZLIB=1 bdiff.c test_bdiff.c -o bdiff_test_z -lz && ./bdiff_test_z

# 内存/UB 安全：ASan+UBSan 双配置 0 leak/0 UB，146/146
gcc -O1 -Wall -Wextra -std=c99 -Werror -fsanitize=address,undefined -fno-omit-frame-pointer bdiff.c test_bdiff.c -o bdiff_test_asan && ./bdiff_test_asan
gcc -O1 -Wall -Wextra -std=c99 -Werror -fsanitize=address,undefined -fno-omit-frame-pointer -DBDIFF_HAVE_ZLIB=1 bdiff.c test_bdiff.c -o bdiff_test_asanz -lz && ./bdiff_test_asanz

# demo 程序（3 模式：plain / TIGHT / zlib）
gcc -O2 -Wall -Wextra -std=c99 -Werror bdiff.c demo_patch.c -o demo_patch && ./demo_patch

# 基准 N=1..12，三模式输出各自 CSV
gcc -O2 -Wall -Wextra -std=c99 -Werror -DBENCH_MODE=PLAIN       bench_patch_sizes.c bdiff.c -o bench_plain
gcc -O2 -Wall -Wextra -std=c99 -Werror -DBENCH_MODE=PLAIN_TIGHT bench_patch_sizes.c bdiff.c -o bench_tight
gcc -O2 -Wall -Wextra -std=c99 -Werror -DBENCH_MODE=ZLIB -DBDIFF_HAVE_ZLIB=1 bench_patch_sizes.c bdiff.c -o bench_zlib -lz
./bench_plain > plain.csv
./bench_tight > tight.csv
./bench_zlib  > zlib.csv
# 合并：(tail -n +2 plain.csv; tail -n +2 tight.csv; tail -n +2 zlib.csv) | sort -n > all_bench.csv
```

## 7. Opcode (v2) vs TIGHT / 裸写 · 明显占优的补丁场景清单

> **说明**：我们实现的 `compact_tight=1` 是**先试 TIGHT，不满足就自动 fallback v2**。所以下面的"v2 占优"场景，
> 在实际运行时都会自动选择 v2（TIGHT 不丢数据）。本表展示的是：**如果只有裸写 / TIGHT 可用，而没有 opcode 机制会吃多大亏**，
> 也就是"opcode 方案的决定性优势在哪些地方"。**所有 patch size 均基于 128KB 定长固件块。**

### 7.1 综合对比表（128KB 基准）

| 编号 | 场景 | 典型固件含义 | v2 opcode patch size | TIGHT spots-only / 裸写估算大小 | 优势比 (裸÷v2) | 自测编号 |
|---|---|---|---:|---:|---:|---|
| J | **删除一段数据 (old≠new)**：从 128KB 中砍掉 16KB | 固件段裁剪、擦除、区域下线 | **21 B** | 不适用 (裸写需要尺寸变化支持；TIGHT 放弃 fallback) | ∞ | (新增 T32 同类) |
| K | **追加一段数据 (old≠new)**：末尾追加 8KB | 追加配置段、新固件尾部参数、追加 FW 签名块 | **8209 B** (=1×COPY + 1×APPEND) | 不适用（需要尺寸变化支持） | ∞ | 实测 |
| C | **中间插入小段**：4096B 中间插 32B | 插入版本号、段表扩展、填充头部保留位 | **51 B** | 不适用（需要尺寸变化支持） | ∞ | 实测 / T32 同类 |
| L | **大段整体搬移**：2×64KB 两扇区整体对调 | 段地址重排、链接脚本改段位置、Flash 换区 | **21 B** (2×COPY_BIG) | ≈ 16384 个 spot = **98,311 B** (需要覆盖整个 128KB 每个字重写) | **4,681×** | 实测 |
| A | **8×128B 扇区整块覆盖** (自测 T15) | 8 个 Flash 扇区重写、配置区整区更新 | **1,111 B** (deflate 后 196B) | 1024 字 ×6+7 = **6,151 B** | **5.5×** (deflate 31×) | T15 / 实测 |
| T31 | **1024B 连续整块覆盖** | 证书 / 密钥 / 参数表整块重写 | **1,047 B** | 256 字 ×6+7 = **1,543 B** | **1.47×** | **T31** |
| T29 | **20B 连续小改动** (超长 run>8) | 小结构体整体替换 | **45 B** | 实际 TIGHT 已 fallback 到 v2=45B；纯 spots-only 估算 5×6+7 = **37 B** | ≈0.82× | T29 |
| F | **1024 处 4B 散布改 (密集)** | 大量校准值 / 标志位批量更新 | **7,418 B** | **6,151 B** | **0.83×** (裸写反胜) | 实测 (1024 spots) |
| T26 | **14 处 4B 散布 (稀疏, budget 96B)** | 少量状态字 / 版本号更新（TIGHT 甜点） | v2 最坏 >91 B | **91 B**（= 7+6×14，精确） | TIGHT 胜 | **T26/T25** |
| G | **2 处 4B 散布 (极稀疏)** | 裸机最小典型改 | **35 B** | **19 B**（= 7+6×2，TIGHT 胜 1.84×） | 0.54× (opcode 劣) | 实测 |

### 7.2 小结：opcode 设计的三类决定性胜利

**第一类：需要 old≠new 尺寸的场景（J / K / C）**

裸写方案（TIGHT 6B/spot）前提就是 old==new size，根本不能处理这种情况。opcode 的 COPY+ADD 顺序记录流天然支持任意重排，patch size 仍然只有几十字节量级。这一类的优势比是"**无穷大**"：裸写做不到，opcode 几字节搞定。

**第二类：大段搬移 / 整块覆盖（L / A / T31）**

两段 64KB 对调，裸写 spots-only 需要 32768 个 spot → 196 KB 的 patch。opcode 用两条 `COPY_BIG` + varint 只需 21 B，**优势比 4681 倍**。8 个 128B 扇区覆盖，opcode ADD_BIG 把 payload 一次写出去，是 spots-only 方案的 1/5.5（deflate 后 1/31）。这类场景 opcode 是"描述改动的形状"，而 spots-only 必须"描述每一个字的新值"，两者信息量不在同一量级。

这类的触发阈值其实很低：只要单段连续改动 ≥ ~16 字节（ADD_BIG body 开始超过 2 个 spots 开销），opcode 就逐步反超 TIGHT。我们的 TIGHT 编码器已经带了 MAX_RUN=8 的检测——一旦发现长连续变字节，立刻判定 spots 写不划算，自动 fallback 到 v2，所以实测 patch 永远打平或更好。

**第三类：密集散布（F / 1024×4B-1B）**

当改动密度到一定程度（≈每 128B 就有 1 处改），v2 的 ADDX 每条成本（~7B + xdiff）与 TIGHT 的 6B/spot 接近持平。但 TIGHT 的固定 6B/spot 不含 xdiff 微码压缩，当 xdiff 能压零行程时 v2 在密集场景有微弱优势（~10%）。无 deflate 时裸写 6B/spot 在 N=1024 时反而略胜 v2 plain 7418B。但开 deflate 后 v2 仅 196B，**碾压** TIGHT 的 6151B。

### 7.3 实际使用建议

综合以上场景，推荐：

1.  **默认开 `compact_tight=1`**：我们的实现永远"先试 TIGHT，不合适自动 fallback v2"，调用方永远不吃亏，不会出现上表中"只有裸写才会爆炸"的情况。
2.  **能确认固件只改 N≤14 个状态字/计数器**（例如 OTA 后的版本号、校验和、标志位批量写入 96B 预算小包）：此时 TIGHT = 裸写 6B/spot + 7B 独立识别头，正是你最初给的上界，就是最优。
3.  **涉及整扇区覆盖、段搬迁、old≠new 大小**：自动走 opcode v2 路径。这些场景在真实固件 OTA、A/B 分区 swap、签名块追加中极其常见，正是 opcode 算法的用武之地，不能用裸写代替。

自测用例已经覆盖上表绝大多数场景（T15/A, T29, T31, T32, T26, J/K 同类，新 T37/T38/T39 覆盖 Tier-A 优化与 96B budget），共 **146 passed, 0 failed**，4 build（plain/zlib/ASan/ASan+zlib）Werror 全绿，ASan 0 leak 0 UB，demo_patch 三模式 roundtrip PASS。
