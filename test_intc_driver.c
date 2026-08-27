/**
 * @file    test_intc_driver.c
 * @brief   IntcController (AXI 中断控制器) 驱动单元测试
 *
 * 策略：
 *   1) sim 钩子模式 (g_read_fn/g_write_fn != NULL) —— 覆盖 hw_read/hw_write 的
 *      「钩子分支」与全部驱动逻辑（使能/屏蔽/触发/极性/ack/local socket 访问控制）；
 *   2) mirror 镜像模式 (钩子 NULL, g_mirror != NULL) —— 覆盖默认 I/O 的镜像分支；
 *   3) intc_driver_ut_probe_default_io(0/1) —— 覆盖 default_reg_index 的全部防御
 *      分支（低于基址/未对齐/越界/合法）与 default_reg_read/write 全部分支，
 *      mode=1 通过 mmap(MAP_FIXED) 把 INTC_BASE_ADDR 页映射到用户态，使
 *      `*(volatile uint32_t*)addr` 分支安全可达。
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include "intc_driver.h"

#if defined(__unix__) || defined(__linux__)
#include <sys/mman.h>
#include <unistd.h>
#endif

/* -------------------------------------------------------------------------
 *        用户态安全 MMIO 页管理（仅测试使用，用于覆盖 volatile 分支）
 * ------------------------------------------------------------------------- */
static void  *g_mmio_base = NULL;
static size_t g_page_size  = 0;

static int ut_map_mmio_page(void)
{
#if defined(__unix__) || defined(__linux__)
    long ps = sysconf(_SC_PAGESIZE);
    if (ps <= 0) return -1;
    g_page_size = (size_t)ps;
    /* MAP_FIXED 把匿名页映射到 INTC_BASE_ADDR(0x40000000)，让驱动的
     * `*(volatile uint32_t*)0x40000000` 在用户态也能安全访问。 */
    g_mmio_base = mmap((void *)(uintptr_t)INTC_BASE_ADDR,
                       g_page_size,
                       PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED,
                       -1, 0);
    if (g_mmio_base == MAP_FAILED) { g_mmio_base = NULL; return -1; }
    memset(g_mmio_base, 0, g_page_size);
    return 0;
#else
    return -1;
#endif
}

static void ut_unmap_mmio_page(void)
{
#if defined(__unix__) || defined(__linux__)
    if (g_mmio_base != NULL) {
        munmap(g_mmio_base, g_page_size);
        g_mmio_base = NULL;
    }
#else
    g_mmio_base = NULL;
#endif
}

/* -------------------------------------------------------------------------
 *                           测试基础设施
 * ------------------------------------------------------------------------- */
static int g_pass_cnt = 0;
static int g_fail_cnt = 0;

#define TEST_CASE(name) static void test_##name(void)
#define RUN_TEST(name)                                  \
    do {                                                \
        printf("  [RUN ] %s\n", #name);                 \
        test_##name();                                  \
    } while (0)

#define ASSERT_TRUE(cond, fmt, ...)                                     \
    do {                                                                \
        if (cond) { g_pass_cnt++; }                                     \
        else {                                                          \
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
 *                  模拟寄存器层：注入钩子模式
 * ------------------------------------------------------------------------- */
static uint32_t g_sim_regs[INTC_NUM_REGS];
static uint32_t g_sim_ro_writes;   /* 统计 sim 层尝试写 RO 寄存器的次数 */

static inline int sim_idx(uint32_t addr)
{
    if (addr < INTC_BASE_ADDR) return -1;
    uint32_t off = (uint32_t)(addr - INTC_BASE_ADDR);
    if ((off & 0x3u) != 0u) return -1;
    int idx = (int)(off >> 2);
    if (idx >= (int)INTC_NUM_REGS) return -1;
    return idx;
}

static const int sim_writable[INTC_NUM_REGS] = { 1, 1, 1, 0, 0, 1, 1, 1, 1, 0 };

static uint32_t sim_reg_read(uint32_t addr)
{
    int idx = sim_idx(addr);
    if (idx < 0) return 0xFFFFFFFFu;
    return g_sim_regs[idx];
}

static void sim_reg_write(uint32_t addr, uint32_t value)
{
    int idx = sim_idx(addr);
    if (idx < 0) return;
    if (!sim_writable[idx]) { g_sim_ro_writes++; return; }
    g_sim_regs[idx] = value;
}

static void sim_reset(void)
{
    g_sim_regs[0] = INTC_CTRL_RESET_VALUE;
    g_sim_regs[1] = IRQ_ENABLE_RESET_VALUE;
    g_sim_regs[2] = IRQ_MASK_RESET_VALUE;
    g_sim_regs[3] = IRQ_RAW_RESET_VALUE;
    g_sim_regs[4] = IRQ_PENDING_RESET_VALUE;
    g_sim_regs[5] = IRQ_ACK_RESET_VALUE;
    g_sim_regs[6] = IRQ_TRIGGER_RESET_VALUE;
    g_sim_regs[7] = IRQ_POLARITY_RESET_VALUE;
    g_sim_regs[8] = SOCKET_ID_RESET_VALUE;
    g_sim_regs[9] = SOCKET_STATUS_RESET_VALUE;
    g_sim_ro_writes = 0;
    intc_driver_register_io(sim_reg_read, sim_reg_write);
    intc_driver_set_mmio_mirror(NULL);   /* 强制走钩子路径 */
    intc_set_local_socket(0);
}

/* -------------------------------------------------------------------------
 *                  镜像寄存器层：默认 I/O 的镜像模式
 *                  （钩子为 NULL，覆盖 default_reg_read/write 的 mirror 分支）
 * ------------------------------------------------------------------------- */
static uint32_t g_mirror_regs[INTC_NUM_REGS];

static void mirror_reset(void)
{
    g_mirror_regs[0] = INTC_CTRL_RESET_VALUE;
    g_mirror_regs[1] = IRQ_ENABLE_RESET_VALUE;
    g_mirror_regs[2] = IRQ_MASK_RESET_VALUE;
    g_mirror_regs[3] = IRQ_RAW_RESET_VALUE;
    g_mirror_regs[4] = IRQ_PENDING_RESET_VALUE;
    g_mirror_regs[5] = IRQ_ACK_RESET_VALUE;
    g_mirror_regs[6] = IRQ_TRIGGER_RESET_VALUE;
    g_mirror_regs[7] = IRQ_POLARITY_RESET_VALUE;
    g_mirror_regs[8] = SOCKET_ID_RESET_VALUE;
    g_mirror_regs[9] = SOCKET_STATUS_RESET_VALUE;
    intc_driver_register_io(NULL, NULL);   /* 不注入钩子 → 走默认 I/O */
    intc_driver_set_mmio_mirror(g_mirror_regs);
    intc_set_local_socket(0);
}

/* -------------------------------------------------------------------------
 *                            测试用例
 * ------------------------------------------------------------------------- */

/* 1. 初始化与复位值 */
TEST_CASE(init_reset_values)
{
    sim_reset();
    intc_driver_init();   /* 写所有 RW 寄存器到复位值（RO 写被 sim 忽略） */

    ASSERT_EQ(intc_irq_get_enable(),    IRQ_ENABLE_RESET_VALUE,    "0x%08X");
    ASSERT_EQ(intc_irq_get_mask(),      IRQ_MASK_RESET_VALUE,      "0x%08X");
    ASSERT_EQ(intc_irq_get_trigger(),   IRQ_TRIGGER_RESET_VALUE,   "0x%08X");
    ASSERT_EQ(intc_irq_get_polarity(),  IRQ_POLARITY_RESET_VALUE,  "0x%08X");
    ASSERT_EQ(intc_irq_get_raw(),       IRQ_RAW_RESET_VALUE,       "0x%08X");
    ASSERT_EQ(intc_irq_get_pending(),   IRQ_PENDING_RESET_VALUE,   "0x%08X");
    ASSERT_EQ(intc_get_socket_status(), SOCKET_STATUS_RESET_VALUE, "0x%08X");

    ASSERT_EQ(intc_is_global_enabled(),       0U, "%u");  /* ternary F */
    ASSERT_EQ(intc_is_local_access_enabled(), 0U, "%u");  /* ternary F */
    ASSERT_EQ(intc_check_access(), 1, "%d");              /* LOCAL_EN=0 → open (first if T) */
    ASSERT_EQ(intc_get_access_state(), INTC_ACCESS_OPEN, "%d");
}

/* 2. 全局使能/关闭：含 early-return 两分支 */
TEST_CASE(global_enable_disable)
{
    sim_reset();

    intc_global_enable();                                  /* F: proceed */
    ASSERT_EQ(intc_is_global_enabled(), 1U, "%u");         /* ternary T */
    intc_global_enable();                                  /* T: already enabled */
    ASSERT_EQ(g_sim_regs[0] & INTC_CTRL_GLOBAL_EN_Msk, INTC_CTRL_GLOBAL_EN_Msk, "0x%08X");

    intc_global_disable();                                 /* F: proceed */
    ASSERT_EQ(intc_is_global_enabled(), 0U, "%u");         /* ternary F */
    intc_global_disable();                                 /* T: already disabled */

    /* RMW 不破坏其他位 */
    g_sim_regs[0] = INTC_CTRL_SOFT_RESET_Msk;
    intc_global_enable();
    ASSERT_TRUE((g_sim_regs[0] & INTC_CTRL_SOFT_RESET_Msk) != 0u,
                "enable RMW 破坏 SOFT_RESET, ctrl=0x%08X", g_sim_regs[0]);
    ASSERT_EQ(intc_is_global_enabled(), 1U, "%u");
}

/* 3. 软件复位：含 early-return 两分支 */
TEST_CASE(soft_reset)
{
    sim_reset();
    intc_global_enable();
    uint32_t before = g_sim_regs[0];

    intc_soft_reset();                                     /* F: proceed */
    ASSERT_EQ(g_sim_regs[0], before | INTC_CTRL_SOFT_RESET_Msk, "0x%08X");
    intc_soft_reset();                                     /* T: already in reset */
    ASSERT_EQ(g_sim_regs[0], before | INTC_CTRL_SOFT_RESET_Msk, "0x%08X");
    ASSERT_EQ(intc_is_global_enabled(), 1U, "%u");
}

/* 4. local socket 访问使能/关闭：含 early-return 两分支 */
TEST_CASE(local_access_enable_disable)
{
    sim_reset();

    intc_local_access_enable();                            /* F: proceed */
    ASSERT_EQ(intc_is_local_access_enabled(), 1U, "%u");   /* ternary T */
    intc_local_access_enable();                            /* T: already enabled */

    intc_local_access_disable();                           /* F: proceed */
    ASSERT_EQ(intc_is_local_access_enabled(), 0U, "%u");   /* ternary F */
    intc_local_access_disable();                           /* T: already disabled */
}

/* 5. intc_check_access / intc_get_access_state 全部分支 */
TEST_CASE(check_access_and_state)
{
    sim_reset();
    /* OPEN: LOCAL_EN=0 (first if T) */
    ASSERT_EQ(intc_check_access(), 1, "%d");
    ASSERT_EQ(intc_get_access_state(), INTC_ACCESS_OPEN, "%d");

    /* OWNER: LOCAL_EN=1, current == owner (first if F, ternary/second if T) */
    intc_set_local_socket(5);
    g_sim_regs[8] = 5u;
    g_sim_regs[0] |= INTC_CTRL_LOCAL_EN_Msk;
    ASSERT_EQ(intc_check_access(), 1, "%d");
    ASSERT_EQ(intc_get_access_state(), INTC_ACCESS_OWNER, "%d");

    /* DENIED: LOCAL_EN=1, current != owner (first if F, ternary/second if F) */
    intc_set_local_socket(3);
    ASSERT_EQ(intc_check_access(), 0, "%d");
    ASSERT_EQ(intc_get_access_state(), INTC_ACCESS_DENIED, "%d");
}

/* 6. intc_claim_socket 全部分支 */
TEST_CASE(claim_socket)
{
    sim_reset();
    intc_set_local_socket(7);
    ASSERT_EQ(intc_get_local_socket(), 7u, "%u");   /* 覆盖 intc_get_local_socket */

    /* LOCAL_EN=0 → 直接写, return 0 (first if F) */
    ASSERT_EQ(intc_claim_socket(7), 0, "%d");
    ASSERT_EQ(g_sim_regs[8] & SOCKET_ID_ID_Msk, 7u, "%u");

    /* LOCAL_EN=1, current != owner → return -1 (first if T, inner if T) */
    g_sim_regs[0] |= INTC_CTRL_LOCAL_EN_Msk;
    g_sim_regs[8] = 7u;            /* owner = 7 */
    intc_set_local_socket(3);       /* current = 3 != 7 */
    ASSERT_EQ(intc_claim_socket(9), -1, "%d");

    /* LOCAL_EN=1, current == owner → 写, return 0 (first if T, inner if F) */
    intc_set_local_socket(7);       /* current = 7 == owner */
    ASSERT_EQ(intc_claim_socket(9), 0, "%d");
    ASSERT_EQ(g_sim_regs[8] & SOCKET_ID_ID_Msk, 9u, "%u");
}

/* 7. IRQ 使能/关闭：含访问被拒 (T) 与放行 (F) 两分支 */
TEST_CASE(irq_enable_disable_access)
{
    sim_reset();

    /* open access → proceed, return 0 (if !check F) */
    ASSERT_EQ(intc_irq_enable(0x1u), 0, "%d");
    ASSERT_EQ(intc_irq_get_enable(), 0x1u, "0x%08X");
    ASSERT_EQ(intc_irq_disable(0x1u), 0, "%d");
    ASSERT_EQ(intc_irq_get_enable(), 0x0u, "0x%08X");

    /* denied → return -1 (if !check T) */
    g_sim_regs[0] |= INTC_CTRL_LOCAL_EN_Msk;
    g_sim_regs[8] = 5u;
    intc_set_local_socket(1);      /* current = 1 != owner 5 */
    ASSERT_EQ(intc_irq_enable(0x2u), -1, "%d");
    ASSERT_EQ(intc_irq_disable(0x2u), -1, "%d");
    ASSERT_EQ(intc_irq_get_enable(), 0x0u, "0x%08X");

    /* owner → proceed (if !check F) */
    intc_set_local_socket(5);
    ASSERT_EQ(intc_irq_enable(0x4u), 0, "%d");
    ASSERT_EQ(intc_irq_get_enable(), 0x4u, "0x%08X");
    ASSERT_EQ(intc_irq_disable(0x4u), 0, "%d");
}

/* 8. IRQ 屏蔽/解除屏蔽：含访问被拒 (T) 与放行 (F) 两分支 */
TEST_CASE(irq_mask_unmask_access)
{
    sim_reset();

    ASSERT_EQ(intc_irq_unmask(0x1u), 0, "%d");
    ASSERT_EQ(intc_irq_get_mask(), 0xFFFFFFFEu, "0x%08X");
    ASSERT_EQ(intc_irq_mask(0x1u), 0, "%d");
    ASSERT_EQ(intc_irq_get_mask(), 0xFFFFFFFFu, "0x%08X");

    g_sim_regs[0] |= INTC_CTRL_LOCAL_EN_Msk;
    g_sim_regs[8] = 5u;
    intc_set_local_socket(2);
    ASSERT_EQ(intc_irq_mask(0x2u), -1, "%d");
    ASSERT_EQ(intc_irq_unmask(0x2u), -1, "%d");
    ASSERT_EQ(intc_irq_get_mask(), 0xFFFFFFFFu, "0x%08X");
}

/* 9. 触发类型/极性：trig==EDGE(T)/LEVEL(F)、pol==HIGH(T)/LOW(F) + 访问两分支 */
TEST_CASE(irq_trigger_polarity)
{
    sim_reset();

    ASSERT_EQ(intc_irq_set_trigger(0x3u, INTC_TRIG_EDGE), 0, "%d");   /* trig T */
    ASSERT_EQ(intc_irq_get_trigger(), 0x3u, "0x%08X");
    ASSERT_EQ(intc_irq_set_trigger(0x3u, INTC_TRIG_LEVEL), 0, "%d");  /* trig F */
    ASSERT_EQ(intc_irq_get_trigger(), 0x0u, "0x%08X");
    intc_irq_set_trigger(0x1u, INTC_TRIG_EDGE);   /* bit0 edge */
    intc_irq_set_trigger(0x2u, INTC_TRIG_LEVEL);  /* bit1 level */
    ASSERT_EQ(intc_irq_get_trigger(), 0x1u, "0x%08X");

    ASSERT_EQ(intc_irq_set_polarity(0x1u, INTC_POL_LOW), 0, "%d");   /* pol F → clear */
    ASSERT_EQ(intc_irq_get_polarity(), 0xFFFFFFFEu, "0x%08X");
    ASSERT_EQ(intc_irq_set_polarity(0x1u, INTC_POL_HIGH), 0, "%d");  /* pol T → set */
    ASSERT_EQ(intc_irq_get_polarity(), 0xFFFFFFFFu, "0x%08X");

    /* denied: trig/polarity 两 API 的 if !check T 分支 */
    g_sim_regs[0] |= INTC_CTRL_LOCAL_EN_Msk;
    g_sim_regs[8] = 5u;
    intc_set_local_socket(9);
    ASSERT_EQ(intc_irq_set_trigger(0x1u, INTC_TRIG_EDGE), -1, "%d");
    ASSERT_EQ(intc_irq_set_polarity(0x1u, INTC_POL_HIGH), -1, "%d");
}

/* 10. IRQ ack：访问两分支 */
TEST_CASE(irq_ack_access)
{
    sim_reset();
    ASSERT_EQ(intc_irq_ack(0x5u), 0, "%d");
    ASSERT_EQ(g_sim_regs[5], 0x5u, "0x%08X");

    g_sim_regs[0] |= INTC_CTRL_LOCAL_EN_Msk;
    g_sim_regs[8] = 5u;
    intc_set_local_socket(0);
    ASSERT_EQ(intc_irq_ack(0x1u), -1, "%d");
    ASSERT_EQ(g_sim_regs[5], 0x5u, "0x%08X");   /* 被拒，ack 不变 */
}

/* 11. 镜像模式：覆盖 hw_read/hw_write 三元 false 分支与 default_reg_* mirror 分支 */
TEST_CASE(default_io_mirror_mode)
{
    mirror_reset();

    intc_global_enable();
    ASSERT_EQ(intc_is_global_enabled(), 1U, "%u");
    intc_global_disable();
    intc_soft_reset();
    ASSERT_TRUE((g_mirror_regs[0] & INTC_CTRL_SOFT_RESET_Msk) != 0u,
                "mirror soft_reset 失败, ctrl=0x%08X", g_mirror_regs[0]);

    ASSERT_EQ(intc_irq_enable(0xFFu), 0, "%d");
    ASSERT_EQ(intc_irq_get_enable(), 0xFFu, "0x%08X");
    ASSERT_EQ(intc_irq_unmask(0xFFu), 0, "%d");
    ASSERT_EQ(intc_irq_get_mask(), 0xFFFFFF00u, "0x%08X");

    g_mirror_regs[3] = 0x12345678u;                       /* IRQ_RAW */
    ASSERT_EQ(intc_irq_get_raw(), 0x12345678u, "0x%08X");
    g_mirror_regs[4] = 0xABCDEF01u;                      /* IRQ_PENDING */
    ASSERT_EQ(intc_irq_get_pending(), 0xABCDEF01u, "0x%08X");
    g_mirror_regs[9] = (1u << SOCKET_STATUS_OWNER_ID_Pos) | SOCKET_STATUS_ACCESS_DENIED_Msk;
    ASSERT_EQ(intc_get_socket_status(), (1u << SOCKET_STATUS_OWNER_ID_Pos) | SOCKET_STATUS_ACCESS_DENIED_Msk,
              "0x%08X");
}

/* 12. 默认 I/O 全部分支：probe(0) 镜像 + probe(1) volatile(mmap) */
TEST_CASE(default_io_probe_all_branches)
{
    intc_driver_ut_probe_default_io(0);   /* if mode==0 T, for 循环, mirror 路径 */

    int rc = ut_map_mmio_page();
    ASSERT_TRUE(rc == 0, "mmap MAP_FIXED@0x40000000 失败, 无法覆盖 volatile 分支");
    intc_driver_ut_probe_default_io(1);   /* if mode==0 F, volatile 路径 */
    ut_unmap_mmio_page();

    ASSERT_TRUE(1, "probe 执行完成（无段错误即通过）");
}

/* 13. RO 寄存器写保护：驱动经 API 不应写 RO 寄存器 */
TEST_CASE(ro_write_protection_via_sim)
{
    sim_reset();
    intc_driver_init();
    intc_global_enable();
    intc_global_disable();
    intc_soft_reset();
    intc_irq_enable(0xFu);
    intc_irq_ack(0x5u);
    ASSERT_EQ(g_sim_ro_writes, 0U, "%u");   /* 驱动 API 未写任何 RO 寄存器 */

    /* 直接通过 sim 层写 RO 寄存器 → 计数 + 忽略 */
    sim_reg_write(IRQ_RAW_ADDR, 0xDEADu);
    ASSERT_EQ(g_sim_ro_writes, 1U, "%u");
    ASSERT_EQ(sim_reg_read(IRQ_RAW_ADDR), IRQ_RAW_RESET_VALUE, "0x%08X");
}

/* -------------------------------------------------------------------------
 *                              主函数
 * ------------------------------------------------------------------------- */
int main(void)
{
    printf("=== IntcController (AXI 中断控制器) 驱动单元测试 ===\n\n");

    RUN_TEST(init_reset_values);
    RUN_TEST(global_enable_disable);
    RUN_TEST(soft_reset);
    RUN_TEST(local_access_enable_disable);
    RUN_TEST(check_access_and_state);
    RUN_TEST(claim_socket);
    RUN_TEST(irq_enable_disable_access);
    RUN_TEST(irq_mask_unmask_access);
    RUN_TEST(irq_trigger_polarity);
    RUN_TEST(irq_ack_access);
    RUN_TEST(default_io_mirror_mode);
    RUN_TEST(default_io_probe_all_branches);
    RUN_TEST(ro_write_protection_via_sim);

    printf("\n=== 汇总: PASS=%d, FAIL=%d ===\n", g_pass_cnt, g_fail_cnt);
    return g_fail_cnt == 0 ? 0 : 1;
}
