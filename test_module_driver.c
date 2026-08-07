/**
 * @file    test_module_driver.c
 * @brief   ModuleController 驱动单元测试
 *
 * 方案：在用户态用两个 uint32_t 数组模拟 CTRL_REG/STATUS_REG，
 *       注入自定义的读写钩子，验证驱动的读-改-写正确性。
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include "module_driver.h"

/* -------------------------------------------------------------------------
 *                           测试基础设施
 * ------------------------------------------------------------------------- */
static int g_pass_cnt = 0;
static int g_fail_cnt = 0;

#define TEST_CASE(name) static void test_##name(void)
#define RUN_TEST(name)                                                  \
    do {                                                                \
        printf("  [RUN ] %s\n", #name);                                 \
        test_##name();                                                  \
    } while (0)

#define ASSERT_TRUE(cond, fmt, ...)                                     \
    do {                                                                \
        if (cond) {                                                     \
            g_pass_cnt++;                                               \
        } else {                                                        \
            g_fail_cnt++;                                               \
            printf("  [FAIL] %s:%d: " fmt "\n",                         \
                   __FILE__, __LINE__, ##__VA_ARGS__);                  \
        }                                                               \
    } while (0)

#define ASSERT_EQ(actual, expected, fmt, ...)                           \
    ASSERT_TRUE((actual) == (expected),                                 \
                "expected=" fmt " actual=" fmt, ##__VA_ARGS__,          \
                (expected), (actual))

/* -------------------------------------------------------------------------
 *                         寄存器模拟层
 * ------------------------------------------------------------------------- */
/* 用用户态内存模拟两个寄存器：[0]=CTRL_REG, [1]=STATUS_REG */
static uint32_t g_sim_regs[2];

static inline int reg_index(uint32_t addr)
{
    if (addr == CTRL_REG_ADDR)   return 0;
    if (addr == STATUS_REG_ADDR) return 1;
    return -1;
}

static uint32_t sim_reg_read(uint32_t addr)
{
    int idx = reg_index(addr);
    if (idx < 0) {
        printf("sim_reg_read: 非法地址 0x%08X\n", addr);
        return 0xFFFFFFFFU;
    }
    return g_sim_regs[idx];
}

static void sim_reg_write(uint32_t addr, uint32_t value)
{
    int idx = reg_index(addr);
    if (idx < 0) {
        printf("sim_reg_write: 非法地址 0x%08X\n", addr);
        return;
    }
    /* STATUS_REG 在真实硬件中是 RO，这里也拒绝写入，便于发现驱动 bug */
    if (idx == 1) {
        printf("sim_reg_write: 尝试写只读 STATUS_REG (value=0x%08X)\n", value);
        return;
    }
    g_sim_regs[idx] = value;
}

static void sim_reset(void)
{
    g_sim_regs[0] = CTRL_REG_RESET_VALUE;
    g_sim_regs[1] = STATUS_REG_RESET_VALUE;
    module_driver_register_io(sim_reg_read, sim_reg_write);
    module_driver_init();
}

/* -------------------------------------------------------------------------
 *                            测试用例
 * ------------------------------------------------------------------------- */

/* 1. 初始化：CTRL_REG=0, STATUS_REG=FIFO_EMPTY=1 */
TEST_CASE(init_reset_values)
{
    sim_reset();
    ASSERT_EQ(g_sim_regs[0], 0x00000000U, "0x%08X"); /* CTRL */
    ASSERT_EQ(g_sim_regs[1], STATUS_REG_RESET_VALUE, "0x%08X");                /* STATUS */
    ASSERT_EQ(module_is_enabled(), 0U, "%u");
    ASSERT_EQ(module_fifo_is_empty(), 1U, "%u");
    ASSERT_EQ(module_fifo_is_full(),  0U, "%u");
    ASSERT_EQ(module_has_error(),     0U, "%u");
    ASSERT_EQ(module_get_state(),     MODULE_STATE_IDLE, "%d");
}

/* 2. 模块使能/关闭：验证 MODULE_EN 读-改-写，不影响其他位 */
TEST_CASE(enable_disable)
{
    sim_reset();

    /* 预置其他位（非 MODULE_EN 位），验证读-改-写不破坏它们 */
    g_sim_regs[0] = CTRL_REG_SOFT_RESET_Msk; /* bit1 = 1 */

    module_enable();
    ASSERT_EQ(module_is_enabled(), 1U, "%u");
    /* bit1(SOFT_RESET) 仍应保持 1 */
    ASSERT_TRUE((g_sim_regs[0] & CTRL_REG_SOFT_RESET_Msk) != 0U,
                "enable 读-改-写时破坏了 SOFT_RESET 位, reg=0x%08X",
                g_sim_regs[0]);

    module_disable();
    ASSERT_EQ(module_is_enabled(), 0U, "%u");
    /* bit1 仍应保持 1 */
    ASSERT_TRUE((g_sim_regs[0] & CTRL_REG_SOFT_RESET_Msk) != 0U,
                "disable 读-改-写时破坏了 SOFT_RESET 位, reg=0x%08X",
                g_sim_regs[0]);
}

/* 3. 软件复位：SOFT_RESET 位置 1，不影响 MODULE_EN */
TEST_CASE(soft_reset)
{
    sim_reset();
    module_enable();
    uint32_t before = g_sim_regs[0];

    module_soft_reset();
    ASSERT_EQ(g_sim_regs[0], before | CTRL_REG_SOFT_RESET_Msk, "0x%08X");

    /* module_is_enabled 仍为 1 */
    ASSERT_EQ(module_is_enabled(), 1U, "%u");
}

/* 4. MODULE_STATE 四种状态读取 */
TEST_CASE(module_state_read)
{
    sim_reset();

    const module_state_t table[] = {
        MODULE_STATE_IDLE, MODULE_STATE_RUNNING,
        MODULE_STATE_BUSY, MODULE_STATE_ERROR
    };
    for (size_t i = 0; i < sizeof(table)/sizeof(table[0]); i++) {
        /* 直接构造 STATUS_REG 的 MODULE_STATE 字段 */
        uint32_t base = g_sim_regs[1] & ~STATUS_REG_MODULE_STATE_Msk;
        g_sim_regs[1] = base | ((uint32_t)table[i] << STATUS_REG_MODULE_STATE_Pos);
        ASSERT_EQ(module_get_state(), table[i], "%d");
    }
}

/* 5. FIFO 标志读写 */
TEST_CASE(fifo_flags)
{
    sim_reset();

    /* 默认：FIFO_EMPTY=1, FIFO_FULL=0 */
    ASSERT_EQ(module_fifo_is_empty(), 1U, "%u");
    ASSERT_EQ(module_fifo_is_full(),  0U, "%u");

    /* 构造 FIFO 非空但未满 */
    g_sim_regs[1] &= ~(STATUS_REG_FIFO_EMPTY_Msk | STATUS_REG_FIFO_FULL_Msk);
    ASSERT_EQ(module_fifo_is_empty(), 0U, "%u");
    ASSERT_EQ(module_fifo_is_full(),  0U, "%u");

    /* 构造 FIFO 满 */
    g_sim_regs[1] |= STATUS_REG_FIFO_FULL_Msk;
    ASSERT_EQ(module_fifo_is_full(), 1U, "%u");
}

/* 6. 错误标志与错误代码 */
TEST_CASE(error_flag_and_code)
{
    sim_reset();

    /* 默认无错误 */
    ASSERT_EQ(module_has_error(), 0U, "%u");
    ASSERT_EQ(module_get_err_code(), 0U, "%u");

    /* 注入 ERR_FLAG=1, ERR_CODE=0xAB */
    g_sim_regs[1] |= STATUS_REG_ERR_FLAG_Msk;
    g_sim_regs[1] &= ~STATUS_REG_ERR_CODE_Msk;
    g_sim_regs[1] |= (0xABU << STATUS_REG_ERR_CODE_Pos);

    ASSERT_EQ(module_has_error(), 1U, "%u");
    ASSERT_EQ(module_get_err_code(), 0xABU, "0x%02X");
}

/* 7. 驱动不得写只读 STATUS_REG */
TEST_CASE(status_reg_never_written)
{
    sim_reset();
    uint32_t backup = g_sim_regs[1];

    /* 执行所有可能写寄存器的接口 */
    module_driver_init();   /* 不写 STATUS */
    module_enable();
    module_disable();
    module_soft_reset();

    ASSERT_EQ(g_sim_regs[1], backup, "0x%08X");
}

/* -------------------------------------------------------------------------
 *                              主函数
 * ------------------------------------------------------------------------- */
int main(void)
{
    printf("=== ModuleController 驱动单元测试 ===\n\n");

    RUN_TEST(init_reset_values);
    RUN_TEST(enable_disable);
    RUN_TEST(soft_reset);
    RUN_TEST(module_state_read);
    RUN_TEST(fifo_flags);
    RUN_TEST(error_flag_and_code);
    RUN_TEST(status_reg_never_written);

    printf("\n=== 汇总: PASS=%d, FAIL=%d ===\n", g_pass_cnt, g_fail_cnt);
    return g_fail_cnt == 0 ? 0 : 1;
}
