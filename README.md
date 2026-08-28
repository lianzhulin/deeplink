# IntcController — AXI 中断控制器驱动

基于寄存器接口的 **AXI4-Lite 中断控制器** 驱动，可在用户态运行单元测试并产出双 100% 覆盖率报告。

## 功能特性

- **AXI4-Lite 总线接口**：32-bit 数据、4-byte 对齐、单拍 R/W；驱动把 AXI 事务抽象为可注入的 `reg_read_fn_t` / `reg_write_fn_t` 钩子，平台负责接到真实 AXI master。
- **32 路中断请求**：每路独立 **使能** (`IRQ_ENABLE`) 与 **屏蔽** (`IRQ_MASK`)；支持 **触发类型** (`IRQ_TRIGGER`：电平/边沿) 与 **极性** (`IRQ_POLARITY`：高/低有效)。
- **local socket 访问控制**：`LOCAL_EN=1` 时，敏感寄存器 (`IRQ_ENABLE`/`IRQ_MASK`/`IRQ_TRIGGER`/`IRQ_POLARITY`/`IRQ_ACK`/`SOCKET_ID`) 仅 `owner socket` 可写；非 owner 写入被拒（API 返回 `-1`）。
- **pending/ack**：`IRQ_PENDING = IRQ_RAW & IRQ_ENABLE & ~IRQ_MASK`（只读）；`IRQ_ACK` 为 W1C 清 pending。
- **RO 寄存器写保护**：`IRQ_RAW` / `IRQ_PENDING` / `SOCKET_STATUS` 写忽略。

## 寄存器映射（基址 `0x40000000`）

| 偏移 | 寄存器 | 访问 | 复位值 | 说明 |
|------|--------|------|--------|------|
| 0x00 | `INTC_CTRL`     | RW | 0x00000000 | GLOBAL_EN / SOFT_RESET / LOCAL_EN |
| 0x04 | `IRQ_ENABLE`    | RW | 0x00000000 | 32 路独立使能（bit i = IRQ i 使能） |
| 0x08 | `IRQ_MASK`      | RW | 0xFFFFFFFF | 32 路独立屏蔽（bit i = 1 屏蔽，默认全屏蔽） |
| 0x0C | `IRQ_RAW`       | RO | 0x00000000 | 原始输入 |
| 0x10 | `IRQ_PENDING`   | RO | 0x00000000 | = RAW & ENABLE & ~MASK |
| 0x14 | `IRQ_ACK`       | RW | 0x00000000 | W1C 清 pending |
| 0x18 | `IRQ_TRIGGER`   | RW | 0x00000000 | 0=电平, 1=边沿 |
| 0x1C | `IRQ_POLARITY`  | RW | 0xFFFFFFFF | 0=低有效, 1=高有效 |
| 0x20 | `SOCKET_ID`     | RW | 0x00000000 | owner socket ID (0~255) |
| 0x24 | `SOCKET_STATUS` | RO | 0x00000000 | LOCKED / OWNER_ID / ACCESS_DENIED |

## 文件说明

| 文件 | 作用 |
|------|------|
| `controller_interface.json` | 控制器接口定义（10 个寄存器、32 路 IRQ、AXI4-Lite 总线） |
| `controller_interface.h`    | 寄存器地址 / 位掩码 / 复位值 / 枚举 C 头文件 |
| `intc_driver.h`            | 驱动对外 API + AXI 读写钩子注入 + mirror 接口 |
| `intc_driver.c`            | 驱动实现（默认 volatile 指针访问 + 镜像模式 + local socket 访问控制） |
| `test_intc_driver.c`       | 单元测试（用户态模拟寄存器 + mmap MAP_FIXED 覆盖真实 MMIO 分支） |
| `run_ut.sh`                | UT + 覆盖率一键脚本（产出 coverage_html，语句/分支分开显示） |

## 编译测试程序（用户态）

```bash
cd /path/to/intc
gcc -std=c99 -Wall -Wextra -Werror -I. \
    -o test_intc_driver \
    test_intc_driver.c intc_driver.c
```

## 运行单元测试

```bash
./test_intc_driver
```

预期输出（13 个用例、79 个断言）：

```
=== IntcController (AXI 中断控制器) 驱动单元测试 ===

  [RUN ] init_reset_values
  [RUN ] global_enable_disable
  [RUN ] soft_reset
  [RUN ] local_access_enable_disable
  [RUN ] check_access_and_state
  [RUN ] claim_socket
  [RUN ] irq_enable_disable_access
  [RUN ] irq_mask_unmask_access
  [RUN ] irq_trigger_polarity
  [RUN ] irq_ack_access
  [RUN ] default_io_mirror_mode
  [RUN ] default_io_probe_all_branches
  [RUN ] ro_write_protection_via_sim

=== 汇总: PASS=79, FAIL=0 ===
```

## 代码覆盖率（语句 + 分支，分开显示）

> 目标：`intc_driver.c` 目标文件的 **语句覆盖率 100%** 且 **分支覆盖率 100%**，两者在 HTML 报告中分栏显示。

一键完成编译、UT、覆盖率采集、HTML 报告生成、双 100% 校验：

```bash
cd /path/to/intc
bash run_ut.sh
```

脚本在终端分别打印整体覆盖率、`intc_driver.c` 单独覆盖率（Lines / Functions / Branches 分三行显示），并输出 HTML 报告入口：

```
✔ 达标：intc_driver.c 语句覆盖率 100%，分支覆盖率 100%（两者分开显示于 HTML 报告）
```

HTML 报告入口：

- 总览（含 Line + Branch 双栏）：`coverage_html/index.html`
- 纯 `intc_driver.c` 双百报告：`coverage_html/driver_only/index.html`

### 手动分步命令（与 run_ut.sh 等价）

```bash
# 1) 清理 + 编译（注意必须：-O0 + --coverage）
rm -f *.gcno *.gcda test_intc_driver_cov
gcc -std=c99 -Wall -Wextra -Werror -I. -O0 --coverage \
    -o test_intc_driver_cov test_intc_driver.c intc_driver.c

# 2) 运行 UT（必须执行一次才能产生 *.gcda）
./test_intc_driver_cov

# 3) 采集覆盖率数据（开启 branch_coverage，语句+分支都采集）
lcov --capture --directory . --output-file coverage.info --rc branch_coverage=1

# 4) 生成 HTML 报告：--branch-coverage 让语句与分支分开显示
genhtml coverage.info \
    --branch-coverage \
    --output-directory coverage_html \
    --show-details --legend --frames

# 5) （可选）只看 intc_driver.c，去掉测试文件自身干扰
lcov --remove coverage.info "*test_intc_driver.c" \
     --output-file /tmp/driver_only.info --rc branch_coverage=1
lcov --summary /tmp/driver_only.info --rc branch_coverage=1
```

依赖安装（若缺 lcov/genhtml）：

```bash
sudo apt-get update && sudo apt-get install -y build-essential lcov
```

### 最终覆盖率指标（目标文件 intc_driver.c）

| 类型 | 结果 | 命中/总数 |
|------|------|-----------|
| 语句覆盖率 (Line Coverage)   | **100.0%** | 190 / 190 |
| 函数覆盖率 (Func Coverage)   | **100.0%** | 35 / 35   |
| 分支覆盖率 (Branch Coverage) | **100.0%** | 62 / 62   |

## 编译产物清理

二进制与覆盖率中间产物已在 `.gitignore` 中忽略，不会被提交；如需手动清理：

```bash
rm -f test_intc_driver test_intc_driver_cov *.o *.out *.gcno *.gcda *.gcov coverage.info && rm -rf coverage_html
```

## 硬件集成说明

默认实现通过 `volatile uint32_t*` 直接读写内存映射寄存器（基址 `0x40000000`）。若在自定义平台运行（模拟器 / RTOS / 寄存器访问必须走 AXI 总线接口），可在初始化时注入自定义读写钩子，把驱动接到真实 AXI master：

```c
#include "intc_driver.h"

static uint32_t my_axi_read(uint32_t addr)  { /* 实现 AXI AR/R 单拍读 */ }
static void     my_axi_write(uint32_t addr, uint32_t value) { /* 实现 AXI AW/W/B 单拍写 */ }

int main(void)
{
    intc_driver_register_io(my_axi_read, my_axi_write);  /* 接 AXI master */
    intc_driver_init();

    /* 典型使用：开放访问下配置 32 路中断 */
    intc_global_enable();                  /* 全局使能 */
    intc_irq_unmask(0x0000FFFFu);          /* 解屏蔽低 16 路 */
    intc_irq_enable(0x0000FFFFu);         /* 使能低 16 路 */
    intc_irq_set_trigger(0x00FFu, INTC_TRIG_EDGE);   /* 低 8 路边沿触发 */
    intc_irq_set_polarity(0x00FFu, INTC_POL_HIGH);   /* 低 8 路高有效 */

    /* local socket 访问模式：仅 owner socket 可改敏感寄存器 */
    intc_set_local_socket(5);              /* 本机 socket = 5 */
    intc_claim_socket(5);                  /* 申请成为 owner */
    intc_local_access_enable();            /* 开启 local socket 访问控制 */
    intc_irq_enable(0x00010000u);          /* 由 owner 配置高 16 路 */

    /* 响应中断 */
    uint32_t pend = intc_irq_get_pending();
    if (pend) intc_irq_ack(pend);          /* W1C 清 pending */

    return 0;
}
```
