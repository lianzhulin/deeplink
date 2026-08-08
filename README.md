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
| `test_module_driver.c`    | 单元测试（在用户态模拟寄存器，不需真实硬件） |

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

预期输出（7 个用例、27 个断言）：

```
=== ModuleController 驱动单元测试 ===

  [RUN ] init_reset_values
  [RUN ] enable_disable
  [RUN ] soft_reset
  [RUN ] module_state_read
  [RUN ] fifo_flags
  [RUN ] error_flag_and_code
  [RUN ] status_reg_never_written

=== 汇总: PASS=27, FAIL=0 ===
```

### 编译产物清理

二进制 `test_module_driver` 已在 `.gitignore` 中忽略，不会被提交；如需手动清理：

```bash
rm -f test_module_driver && find . -maxdepth 1 -type f \( -name '*.o' -o -name '*.out' \) -delete
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
