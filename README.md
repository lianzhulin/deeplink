# deeplink
深度链接

## ModuleController 控制器驱动

本项目包含一个基于寄存器接口的模块控制器驱动，以及可在用户态运行的单元测试。

### 文件说明

| 文件 | 作用 |
|------|------|
| `controller_interface.json` | 控制器接口定义（CTRL_REG 可读写、STATUS_REG 只读） |
| `controller_interface.h`  | 寄存器地址 / 位掩码 / 复位值 / 枚举 C 头文件 |
| `module_driver.h`         | 驱动对外 API + 寄存器读写钩子注入接口 |
| `module_driver.c`         | 驱动实现（默认 volatile 指针访问寄存器） |
| `test_module_driver.c`    | 单元测试（用户态模拟寄存器 + mmap MAP_FIXED 覆盖真实 MMIO 分支） |
| `run_ut.sh`               | UT + 覆盖率一键脚本（产出 coverage_html，语句/分支分开显示） |

### 编译测试程序（用户态）

在 Linux 环境下使用 gcc：

```bash
cd /path/to/deeplink
gcc -std=c99 -Wall -Wextra -Werror -I. \
    -o test_module_driver \
    test_module_driver.c module_driver.c
```

编译参数说明：
- `-std=c99`：使用 C99 标准
- `-Wall -Wextra`：开启常用警告
- `-Werror`：将警告视为错误（推荐用于 CI）
- `-I.`：头文件搜索路径包含当前目录（`controller_interface.h`、`module_driver.h`）

### 运行单元测试

```bash
./test_module_driver
```

预期输出（12 个用例、54 个断言）：

```
=== ModuleController 驱动单元测试 ===

  [RUN ] init_reset_values
  [RUN ] enable_disable
  [RUN ] soft_reset
  [RUN ] module_state_read
  [RUN ] fifo_flags
  [RUN ] error_flag_and_code
  [RUN ] status_reg_never_written
  [RUN ] default_io_mirror_mode
  [RUN ] default_io_mirror_status_ro
  [RUN ] bad_addr_paths_and_all_ones_ctrl
  [RUN ] default_io_probe_all_branches
  [RUN ] default_io_mirror_illegal_addr_path

=== 汇总: PASS=54, FAIL=0 ===
```

### 代码覆盖率（语句覆盖率 + 分支覆盖率，分开显示）

目标：`module_driver.c` 目标文件的**语句覆盖率 100%** 且 **分支覆盖率 100%**，两者在 HTML 报告中分栏显示。

推荐方式：使用已提供的 `run_ut.sh` 一键完成编译、UT、覆盖率采集、HTML 报告生成、双 100% 校验：

```bash
cd /path/to/deeplink
bash run_ut.sh
```

脚本在终端会分别打印整体覆盖率、`module_driver.c` 单独覆盖率（Lines / Functions / Branches 分三行显示），并输出如下 HTML 报告入口：

```
✔ 达标：module_driver.c 语句覆盖率 100%，分支覆盖率 100%（两者分开显示于 HTML 报告）
```

> 生成的 `coverage_html/index.html` 中包含 **Line Coverage** 和 **Branch Coverage** 两个独立栏目；
> 如需快速只看驱动文件，打开 `coverage_html/driver_only/index.html` 即可。

#### 手动分步命令（与 run_ut.sh 等价）

```bash
# 1) 清理 + 编译（注意必须：-O0 + --coverage）
rm -f *.gcno *.gcda test_module_driver_cov
gcc -std=c99 -Wall -Wextra -Werror -I. -O0 --coverage \
    -o test_module_driver_cov test_module_driver.c module_driver.c

# 2) 运行 UT（必须执行一次才能产生 *.gcda）
./test_module_driver_cov

# 3) 采集覆盖率数据（开启 branch_coverage，语句+分支都采集）
lcov --capture --directory . --output-file coverage.info --rc branch_coverage=1

# 4) 生成 HTML 报告：--branch-coverage 让语句与分支覆盖率分开显示
genhtml coverage.info \
    --branch-coverage \
    --output-directory coverage_html \
    --show-details --legend --frames

# 5) （可选）只看 module_driver.c，去掉测试文件自身干扰
lcov --remove coverage.info "*test_module_driver.c" \
     --output-file /tmp/driver_only.info --rc branch_coverage=1
lcov --summary /tmp/driver_only.info --rc branch_coverage=1
```

依赖安装（若缺 lcov/genhtml）：

```bash
sudo apt-get update && sudo apt-get install -y build-essential lcov
```

最终覆盖率指标（目标文件 module_driver.c）：

| 类型 | 结果 | 命中/总数 |
|------|------|-----------|
| 语句覆盖率 (Line Coverage)   | **100.0%** | 103 / 103 |
| 函数覆盖率 (Func Coverage)   | **100.0%** | 18 / 18   |
| 分支覆盖率 (Branch Coverage) | **100.0%** | 39 / 39   |

### 编译产物清理

二进制 `test_module_driver` / `test_module_driver_cov`、覆盖率中间产物已在 `.gitignore` 中忽略，不会被提交；如需手动清理：

```bash
rm -f test_module_driver test_module_driver_cov test_module_driver_plain \
      *.o *.out *.gcno *.gcda *.gcov coverage.info && rm -rf coverage_html
```

### 硬件集成说明

默认实现通过 `volatile uint32_t*` 直接读写内存映射寄存器（基地址 `0x40000000`，定义于 `controller_interface.h`）。
若在自定义平台上运行（例如模拟、RTOS、寄存器访问必须走总线接口），可在初始化时注入自定义读写钩子：

```c
#include "module_driver.h"

static uint32_t my_reg_read(uint32_t addr)  { /* 平台实现 */ }
static void     my_reg_write(uint32_t addr, uint32_t value) { /* 平台实现 */ }

int main(void)
{
    module_driver_register_io(my_reg_read, my_reg_write);
    module_driver_init();
    module_enable();
    return 0;
}
```
