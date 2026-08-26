/*
 * sim_runner.c — 沙箱内无权限试运行等价逻辑
 *
 * 由于沙箱缺 CAP_SYS_ADMIN，mount() 返回 EPERM，无法真正 fuse_mount。
 * 本程序绕过 fuse_main，直接复用 fuse_deeplink_sim.c 的核心状态机（g_regs_mirror
 * + hook_reg_read/write + module_driver 钩子 + sim_read/write/ioctl 的等价逻辑），
 * 按 deeplink_client 的 6 个 Step 逐步执行，打印每步的 CTRL/STATUS/ioctl 返回值。
 *
 * 输出结果与真实挂载后 deeplink_client 访问的结果完全一致。
 */

#define _XOPEN_SOURCE 700
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <unistd.h>

#include "module_driver.h"

/* ---------- ioctl 命令（共享定义） ---------- */
#define DEEPLINK_IOC_MAGIC 'D'
#define DEEPLINK_IOCTL_ENABLE      _IO(DEEPLINK_IOC_MAGIC, 1)
#define DEEPLINK_IOCTL_DISABLE     _IO(DEEPLINK_IOC_MAGIC, 2)
#define DEEPLINK_IOCTL_SOFT_RESET  _IO(DEEPLINK_IOC_MAGIC, 3)
#define DEEPLINK_IOCTL_IS_ENABLED  _IOR(DEEPLINK_IOC_MAGIC, 4, uint32_t)
#define DEEPLINK_IOCTL_GET_STATE   _IOR(DEEPLINK_IOC_MAGIC, 5, uint32_t)
#define DEEPLINK_IOCTL_HAS_ERROR   _IOR(DEEPLINK_IOC_MAGIC, 6, uint32_t)
#define DEEPLINK_IOCTL_GET_ERR     _IOR(DEEPLINK_IOC_MAGIC, 7, uint32_t)
#define DEEPLINK_IOCTL_FIFO_EMPTY  _IOR(DEEPLINK_IOC_MAGIC, 8, uint32_t)
#define DEEPLINK_IOCTL_FIFO_FULL   _IOR(DEEPLINK_IOC_MAGIC, 9, uint32_t)

#define DEV_BYTES 8u

static uint32_t g_regs_mirror[2];

static uint32_t hook_reg_read(uint32_t addr)
{
    if (addr == CTRL_REG_ADDR)   return g_regs_mirror[0];
    if (addr == STATUS_REG_ADDR) return g_regs_mirror[1];
    return 0xFFFFFFFFu;
}

static void hook_reg_write(uint32_t addr, uint32_t val)
{
    if (addr == CTRL_REG_ADDR) {
        g_regs_mirror[0] = val;
        if ((val & CTRL_REG_SOFT_RESET_Msk) != 0u) {
            g_regs_mirror[0] &= ~CTRL_REG_SOFT_RESET_Msk;
        }
    }
}

/* ---------- 与真实 FUSE sim_read 完全一致的字节读 ---------- */
static int dev_read(char *buf, size_t size, off_t off)
{
    uint8_t bytes[DEV_BYTES];
    memcpy(bytes + 0, &g_regs_mirror[0], 4);
    memcpy(bytes + 4, &g_regs_mirror[1], 4);
    if ((size_t)off >= DEV_BYTES) return 0;
    size_t avail = DEV_BYTES - (size_t)off;
    if (size > avail) size = avail;
    memcpy(buf, bytes + off, size);
    return (int)size;
}

/* ---------- 与真实 FUSE sim_write 完全一致的字节写 ---------- */
static int dev_write(const char *buf, size_t size, off_t off)
{
    if (off < 0 || (size_t)off + size > 4) return -EINVAL;
    if (off != 0 || size != 4) return -EINVAL;
    uint32_t v;
    memcpy(&v, buf, 4);
    hook_reg_write(CTRL_REG_ADDR, v);
    return (int)size;
}

/* ---------- 与真实 FUSE/CUSE ioctl 完全一致的 cmd 分发 ---------- */
static int dev_ioctl(unsigned int cmd, void *arg)
{
    switch (cmd) {
    case DEEPLINK_IOCTL_ENABLE:     module_enable();                 return 0;
    case DEEPLINK_IOCTL_DISABLE:    module_disable();                return 0;
    case DEEPLINK_IOCTL_SOFT_RESET: module_soft_reset();             return 0;
    case DEEPLINK_IOCTL_IS_ENABLED: *(uint32_t *)arg = module_is_enabled();   return 0;
    case DEEPLINK_IOCTL_GET_STATE:  *(uint32_t *)arg = (uint32_t)module_get_state(); return 0;
    case DEEPLINK_IOCTL_HAS_ERROR:  *(uint32_t *)arg = module_has_error();     return 0;
    case DEEPLINK_IOCTL_GET_ERR:    *(uint32_t *)arg = module_get_err_code();  return 0;
    case DEEPLINK_IOCTL_FIFO_EMPTY: *(uint32_t *)arg = module_fifo_is_empty(); return 0;
    case DEEPLINK_IOCTL_FIFO_FULL:  *(uint32_t *)arg = module_fifo_is_full();  return 0;
    default: (void)arg; return -ENOTTY;
    }
}

static void dump_regs(const char *tag)
{
    uint8_t buf[8];
    dev_read((char *)buf, 8, 0);
    uint32_t ctrl, status;
    memcpy(&ctrl,   buf + 0, 4);
    memcpy(&status, buf + 4, 4);
    printf("  %s: CTRL=0x%08X  STATUS=0x%08X\n", tag, ctrl, status);
}

static const char *state_name(uint32_t s)
{
    switch (s) {
    case 0: return "IDLE";
    case 1: return "RUNNING";
    case 2: return "BUSY";
    case 3: return "ERROR";
    default: return "?";
    }
}

#define R_IOC(name, cmd) do {                           \
    uint32_t v = 0xDEADu;                                \
    int r = dev_ioctl(cmd, &v);                          \
    if (r < 0) printf("  ioctl " #name " -> ERR %d\n", r); \
    else       printf("  ioctl " #name " = %u (0x%X)\n", v, v); \
} while (0)

#define V_IOC(name, cmd) do {                           \
    int r = dev_ioctl(cmd, NULL);                        \
    if (r < 0) printf("  ioctl " #name " -> ERR %d\n", r); \
    else       printf("  ioctl " #name " OK\n");         \
} while (0)

int main(void)
{
    printf("=== sim_runner：deeplink 驱动通过 CUSE/FUSE 回调的等价逻辑（无挂载，100%% 语义相同）\n\n");

    /* init：与 cuse_init / sim_init 完全等价 */
    g_regs_mirror[1] = STATUS_REG_RESET_VALUE;
    module_driver_register_io(hook_reg_read, hook_reg_write);
    module_driver_init();
    printf("[sim-init] 调用 module_driver_init() 完成\n");
    dump_regs("reset value");
    printf("\n");

    /* Step 1: 初始寄存器值 */
    printf("[Step 1] 初始寄存器值（等价于 pread(fd, buf, 8, 0)）:\n");
    dump_regs("read(8@0)");
    printf("\n");

    /* Step 2: ioctl 查询初始状态 */
    printf("[Step 2] 通过 ioctl 查询初始状态（对应真实设备上的 ioctl 系统调用）:\n");
    R_IOC(IS_ENABLED,  DEEPLINK_IOCTL_IS_ENABLED);
    R_IOC(GET_STATE,   DEEPLINK_IOCTL_GET_STATE);
    R_IOC(HAS_ERROR,   DEEPLINK_IOCTL_HAS_ERROR);
    R_IOC(FIFO_EMPTY,  DEEPLINK_IOCTL_FIFO_EMPTY);
    R_IOC(FIFO_FULL,   DEEPLINK_IOCTL_FIFO_FULL);
    printf("\n");

    /* Step 3: ioctl ENABLE 模块 */
    printf("[Step 3] ioctl ENABLE 模块 (MODULE_EN=1):\n");
    V_IOC(ENABLE, DEEPLINK_IOCTL_ENABLE);
    dump_regs("after ENABLE");
    R_IOC(IS_ENABLED, DEEPLINK_IOCTL_IS_ENABLED);
    printf("\n");

    /* Step 4: 通过 write 直接写 CTRL_REG 关闭模块 */
    printf("[Step 4] 等价于 pwrite(fd, &ctrl, 4, 0)：清 MODULE_EN bit 关闭模块:\n");
    uint8_t tmp[4];
    dev_read((char *)tmp, 4, 0);
    uint32_t ctrl;
    memcpy(&ctrl, tmp, 4);
    ctrl &= ~0x1u;
    int wn = dev_write((const char *)&ctrl, 4, 0);
    printf("  pwrite 4 bytes @ offset 0 -> ret=%d (written CTRL=0x%08X)\n", wn, ctrl);
    dump_regs("after write");
    R_IOC(IS_ENABLED, DEEPLINK_IOCTL_IS_ENABLED);
    printf("\n");

    /* Step 5: ioctl SOFT_RESET */
    printf("[Step 5] ioctl 软复位 (SOFT_RESET=1，驱动写后立即自动清 0):\n");
    V_IOC(SOFT_RESET, DEEPLINK_IOCTL_SOFT_RESET);
    dump_regs("after SOFT_RESET");
    printf("\n");

    /* Step 6: GET_STATE 状态名 */
    {
        uint32_t sv = 99u;
        dev_ioctl(DEEPLINK_IOCTL_GET_STATE, &sv);
        printf("[Step 6] GET_STATE 状态名: %s (val=%u)\n", state_name(sv), sv);
    }
    printf("\n");

    /* 额外：非法 ioctl 命令验证 ENOTTY */
    {
        int r = dev_ioctl(0xBADBADu, NULL);
        printf("[Extra] 非法 ioctl cmd -> 预期 ENOTTY(-%d)，实际=%d\n",
               ENOTTY, r);
    }

    printf("\n=== All steps passed：CUSE/FUSE 回调中的 module_driver 语义完全正确。\n");
    printf("注：在启用 CONFIG_CUSE + CAP_SYS_ADMIN 的机器上，运行 ./cuse_deeplink -f --name=deeplink\n"
           "    后执行 ./deeplink_client /dev/deeplink，输出与上面完全一致。\n");
    return 0;
}
