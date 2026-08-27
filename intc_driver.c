/*
 * intc_driver.c - AXI 中断控制器 (IntcController) 驱动
 *
 * 说明：
 *   - AXI4-Lite 总线：默认 I/O 通过 MMIO 基址 INTC_BASE_ADDR 的 volatile 指针
 *     访问寄存器，对应 AXI 单拍 R/W 事务；平台可通过 intc_driver_register_io()
 *     注入自己的 AXI master 读写实现。
 *   - g_mirror 镜像模式（UT 用，用户态安全）：当 g_mirror != NULL 且未注入钩子时，
 *     默认 I/O 读写走镜像数组，避免访问真实硬件地址导致段错误。
 *   - default_reg_index 用「地址-基址 / 4」计算下标，含越界/未对齐防御分支。
 *   - RO 寄存器 (IRQ_RAW / IRQ_PENDING / SOCKET_STATUS) 写忽略。
 *   - local socket 访问：LOCAL_EN=1 时，IRQ_ENABLE/IRQ_MASK/IRQ_TRIGGER/
 *     IRQ_POLARITY/IRQ_ACK/SOCKET_ID 等敏感寄存器仅 owner socket 可写；
 *     intc_check_access() 集中判定，写 API 返回 -1 表示被拒。
 *   - intc_driver_ut_probe_default_io() 为 UT-only 辅助，覆盖默认 I/O 全部分支。
 */

#include "intc_driver.h"
#include "controller_interface.h"

#include <stddef.h>
#include <stdint.h>

/* -------------------------------------------------------------------------
 * 全局状态
 * ------------------------------------------------------------------------- */
static reg_read_fn_t  g_read_fn  = NULL;
static reg_write_fn_t g_write_fn = NULL;
static uint32_t      *g_mirror   = NULL;
static uint8_t        g_current_socket = 0u;   /* 软件侧「本机 socket」上下文 */

/* -------------------------------------------------------------------------
 * 寄存器可写属性表（idx 顺序与 default_reg_index 一致）
 *   0 INTC_CTRL      RW   1
 *   1 IRQ_ENABLE     RW   1
 *   2 IRQ_MASK       RW   1
 *   3 IRQ_RAW        RO   0
 *   4 IRQ_PENDING    RO   0
 *   5 IRQ_ACK        RW(W1C) 1
 *   6 IRQ_TRIGGER    RW   1
 *   7 IRQ_POLARITY   RW   1
 *   8 SOCKET_ID      RW   1
 *   9 SOCKET_STATUS  RO   0
 * ------------------------------------------------------------------------- */
static const int reg_writable[INTC_NUM_REGS] = { 1, 1, 1, 0, 0, 1, 1, 1, 1, 0 };

/* -------------------------------------------------------------------------
 * 镜像模式接口
 * ------------------------------------------------------------------------- */
void intc_driver_set_mmio_mirror(uint32_t *mirror)
{
    g_mirror = mirror;
}

/* -------------------------------------------------------------------------
 * 自定义 I/O 钩子接口（接 AXI master 或 UT 模拟）
 * ------------------------------------------------------------------------- */
void intc_driver_register_io(reg_read_fn_t r, reg_write_fn_t w)
{
    g_read_fn  = r;
    g_write_fn = w;
}

/* -------------------------------------------------------------------------
 * default_reg_index: 地址 → 数组下标
 *   合法地址 = INTC_BASE_ADDR + 4*i, i ∈ [0, INTC_NUM_REGS)
 *   非法（低于基址 / 未对齐 / 越界）→ -1
 * ------------------------------------------------------------------------- */
static int default_reg_index(uint32_t addr)
{
    if (addr < INTC_BASE_ADDR) return -1;
    uint32_t off = (uint32_t)(addr - INTC_BASE_ADDR);
    if ((off & 0x3u) != 0u) return -1;          /* 必须 4-byte 对齐 */
    uint32_t idx = off >> 2;
    if (idx >= INTC_NUM_REGS) return -1;         /* 越界 */
    return (int)idx;
}

/* -------------------------------------------------------------------------
 * default_reg_read / default_reg_write（默认 I/O：镜像 or 真实 MMIO）
 * ------------------------------------------------------------------------- */
static uint32_t default_reg_read(uint32_t addr)
{
    int idx = default_reg_index(addr);
    if (idx < 0) return 0xFFFFFFFFu;             /* 非法地址返回全 1 */
    if (g_mirror != NULL) return g_mirror[idx];
    return *((volatile uint32_t *)(uintptr_t)addr);
}

static void default_reg_write(uint32_t addr, uint32_t val)
{
    int idx = default_reg_index(addr);
    if (idx < 0) return;                        /* 非法地址忽略 */
    if (!reg_writable[idx]) return;              /* RO 寄存器忽略写 */
    if (g_mirror != NULL) { g_mirror[idx] = val; return; }
    *((volatile uint32_t *)(uintptr_t)addr) = val;
}

/* -------------------------------------------------------------------------
 * hw_read / hw_write（I/O 分发：钩子 or 默认）
 * ------------------------------------------------------------------------- */
static inline uint32_t hw_read(uint32_t addr)
{
    return (g_read_fn ? g_read_fn : default_reg_read)(addr);
}

static inline void hw_write(uint32_t addr, uint32_t val)
{
    (g_write_fn ? g_write_fn : default_reg_write)(addr, val);
}

/* -------------------------------------------------------------------------
 * intc_driver_ut_probe_default_io  (UT-only)
 *
 * 设计原则 —— 绝对最小化，避免引入新的未覆盖分支：
 *   mode=0: g_mirror != NULL 路径全覆盖（本地数组，无需 mmap）
 *   mode=1: g_mirror == NULL + volatile 路径（调用方需先 mmap MAP_FIXED）
 *   覆盖目标：
 *     default_reg_index : 低于基址 / 未对齐 / 越界 / 合法 (T/F 全分支)
 *     default_reg_read  : idx<0(T/F) + g_mirror!=NULL(T/F)
 *     default_reg_write : idx<0(T) + !writable(T) + g_mirror!=NULL(T/F) + 合法可写
 * ------------------------------------------------------------------------- */
void intc_driver_ut_probe_default_io(int mode)
{
    reg_read_fn_t  save_r = g_read_fn;
    reg_write_fn_t save_w = g_write_fn;
    uint32_t      *save_m = g_mirror;

    g_read_fn  = NULL;
    g_write_fn = NULL;

    /* 三类地址样本：合法(CTRL)、合法但 RO(IRQ_RAW)、未对齐(BASE+1)、
     * 越界(BASE+4*NUM_REGS)、低于基址(0x0) */
    const uint32_t a_valid    = INTC_CTRL_ADDR;                       /* idx 0, 可写 */
    const uint32_t a_ro       = IRQ_RAW_ADDR;                         /* idx 3, RO   */
    const uint32_t a_unalign = INTC_BASE_ADDR + 1u;                  /* 未对齐 */
    const uint32_t a_oor     = INTC_BASE_ADDR + (uint32_t)(INTC_NUM_REGS * 4u); /* 越界 */
    const uint32_t a_low     = 0x00000000u;                          /* 低于基址 */

    if (mode == 0) {
        /* ===== Mode 0: g_mirror != NULL 路径 ===== */
        uint32_t local_mirror[INTC_NUM_REGS];
        for (uint32_t i = 0; i < INTC_NUM_REGS; i++) local_mirror[i] = 0u;
        g_mirror = local_mirror;

        (void)default_reg_read(a_valid);     /* idx=0 合法, mirror → mirror[0]        */
        (void)default_reg_read(a_ro);        /* idx=3 合法, mirror → mirror[3]        */
        (void)default_reg_read(a_unalign);   /* 未对齐 → idx=-1 → 0xFFFFFFFF          */
        (void)default_reg_read(a_oor);       /* 越界   → idx=-1 → 0xFFFFFFFF          */
        (void)default_reg_read(a_low);       /* 低于基址 → idx=-1 → 0xFFFFFFFF        */

        default_reg_write(a_valid, 0x11u);   /* idx=0 可写 → mirror[0]=0x11           */
        default_reg_write(a_ro, 0x22u);       /* idx=3 RO  → 忽略                       */
        default_reg_write(a_unalign, 0x33u);  /* 未对齐 → idx=-1 → 忽略                */
        default_reg_write(a_oor, 0x44u);      /* 越界   → idx=-1 → 忽略                */
        default_reg_write(a_low, 0x55u);      /* 低于基址 → idx=-1 → 忽略              */
    } else {
        /* ===== Mode 1: g_mirror == NULL + volatile 路径 =====
         * 调用方需通过 mmap(MAP_FIXED) 把 INTC_BASE_ADDR 页映射到用户态安全页，
         * 使 `*(volatile uint32_t*)addr` 不段错误；idx<0 与 !writable 分支
         * 在到达 volatile 解引用前已 return，故对 a_unalign/a_oor/a_low/a_ro 安全。
         */
        g_mirror = NULL;

        (void)default_reg_read(a_valid);     /* 合法可写, 走 volatile 读              */
        (void)default_reg_read(a_ro);        /* 合法 RO, 走 volatile 读              */
        (void)default_reg_read(a_unalign);   /* idx=-1 → 0xFFFFFFFF（不解引用）       */
        (void)default_reg_read(a_oor);       /* idx=-1 → 0xFFFFFFFF                   */
        (void)default_reg_read(a_low);       /* idx=-1 → 0xFFFFFFFF                   */

        default_reg_write(a_valid, 0x11u);   /* 合法可写, 走 volatile 写              */
        default_reg_write(a_ro, 0x22u);       /* idx=3 RO → 忽略（不解引用）           */
        default_reg_write(a_unalign, 0x33u);  /* idx=-1 → 忽略                         */
        default_reg_write(a_oor, 0x44u);      /* idx=-1 → 忽略                         */
        default_reg_write(a_low, 0x55u);      /* idx=-1 → 忽略                         */
    }

    g_read_fn  = save_r;
    g_write_fn = save_w;
    g_mirror   = save_m;
}

/* =========================================================================
 * 公共驱动 API
 * ========================================================================= */
void intc_driver_init(void)
{
    /* 写所有 RW 寄存器到复位值；RO 寄存器写会被 default_reg_write 忽略 */
    hw_write(INTC_CTRL_ADDR,    INTC_CTRL_RESET_VALUE);
    hw_write(IRQ_ENABLE_ADDR,    IRQ_ENABLE_RESET_VALUE);
    hw_write(IRQ_MASK_ADDR,      IRQ_MASK_RESET_VALUE);
    hw_write(IRQ_ACK_ADDR,       IRQ_ACK_RESET_VALUE);
    hw_write(IRQ_TRIGGER_ADDR,    IRQ_TRIGGER_RESET_VALUE);
    hw_write(IRQ_POLARITY_ADDR,   IRQ_POLARITY_RESET_VALUE);
    hw_write(SOCKET_ID_ADDR,     SOCKET_ID_RESET_VALUE);
    /* IRQ_RAW / IRQ_PENDING / SOCKET_STATUS 为 RO，不主动写 */
}

/* ---- 全局控制 ---- */
void intc_global_enable(void)
{
    uint32_t v = hw_read(INTC_CTRL_ADDR);
    if ((v & INTC_CTRL_GLOBAL_EN_Msk) != 0u) return;   /* 已使能 */
    v |= INTC_CTRL_GLOBAL_EN_Msk;
    hw_write(INTC_CTRL_ADDR, v);
}

void intc_global_disable(void)
{
    uint32_t v = hw_read(INTC_CTRL_ADDR);
    if ((v & INTC_CTRL_GLOBAL_EN_Msk) == 0u) return;   /* 已关闭 */
    v &= ~INTC_CTRL_GLOBAL_EN_Msk;
    hw_write(INTC_CTRL_ADDR, v);
}

uint32_t intc_is_global_enabled(void)
{
    uint32_t v = hw_read(INTC_CTRL_ADDR);
    return ((v & INTC_CTRL_GLOBAL_EN_Msk) != 0u) ? 1u : 0u;
}

void intc_soft_reset(void)
{
    uint32_t v = hw_read(INTC_CTRL_ADDR);
    if ((v & INTC_CTRL_SOFT_RESET_Msk) != 0u) return;  /* 已在复位 */
    v |= INTC_CTRL_SOFT_RESET_Msk;
    hw_write(INTC_CTRL_ADDR, v);
}

/* ---- local socket 访问 ---- */
void intc_set_local_socket(uint8_t socket_id)
{
    g_current_socket = socket_id;
}

uint8_t intc_get_local_socket(void)
{
    return g_current_socket;
}

int intc_check_access(void)
{
    uint32_t ctrl = hw_read(INTC_CTRL_ADDR);
    if ((ctrl & INTC_CTRL_LOCAL_EN_Msk) == 0u) return 1;   /* LOCAL_EN=0 开放 */
    uint32_t owner = hw_read(SOCKET_ID_ADDR) & SOCKET_ID_ID_Msk;
    return ((uint32_t)g_current_socket == owner) ? 1 : 0;   /* 1=owner, 0=拒绝 */
}

intc_access_state_t intc_get_access_state(void)
{
    uint32_t ctrl = hw_read(INTC_CTRL_ADDR);
    if ((ctrl & INTC_CTRL_LOCAL_EN_Msk) == 0u) return INTC_ACCESS_OPEN;
    uint32_t owner = hw_read(SOCKET_ID_ADDR) & SOCKET_ID_ID_Msk;
    if ((uint32_t)g_current_socket == owner) return INTC_ACCESS_OWNER;
    return INTC_ACCESS_DENIED;
}

int intc_claim_socket(uint8_t socket_id)
{
    /* LOCAL_EN=1 时，仅当前 owner 可重新声明（避免被别的 socket 抢占） */
    uint32_t ctrl = hw_read(INTC_CTRL_ADDR);
    if ((ctrl & INTC_CTRL_LOCAL_EN_Msk) != 0u) {
        uint32_t owner = hw_read(SOCKET_ID_ADDR) & SOCKET_ID_ID_Msk;
        if (owner != (uint32_t)g_current_socket) return -1;   /* 非 owner，拒绝 */
    }
    hw_write(SOCKET_ID_ADDR, (uint32_t)socket_id & SOCKET_ID_ID_Msk);
    return 0;
}

void intc_local_access_enable(void)
{
    uint32_t v = hw_read(INTC_CTRL_ADDR);
    if ((v & INTC_CTRL_LOCAL_EN_Msk) != 0u) return;   /* 已使能 */
    v |= INTC_CTRL_LOCAL_EN_Msk;
    hw_write(INTC_CTRL_ADDR, v);
}

void intc_local_access_disable(void)
{
    uint32_t v = hw_read(INTC_CTRL_ADDR);
    if ((v & INTC_CTRL_LOCAL_EN_Msk) == 0u) return;   /* 已关闭 */
    v &= ~INTC_CTRL_LOCAL_EN_Msk;
    hw_write(INTC_CTRL_ADDR, v);
}

uint32_t intc_is_local_access_enabled(void)
{
    uint32_t v = hw_read(INTC_CTRL_ADDR);
    return ((v & INTC_CTRL_LOCAL_EN_Msk) != 0u) ? 1u : 0u;
}

/* ---- 32 路中断：使能/屏蔽 ---- */
int intc_irq_enable(uint32_t mask)
{
    if (!intc_check_access()) return -1;               /* 访问被拒 */
    uint32_t v = hw_read(IRQ_ENABLE_ADDR);
    v |= mask;
    hw_write(IRQ_ENABLE_ADDR, v);
    return 0;
}

int intc_irq_disable(uint32_t mask)
{
    if (!intc_check_access()) return -1;
    uint32_t v = hw_read(IRQ_ENABLE_ADDR);
    v &= ~mask;
    hw_write(IRQ_ENABLE_ADDR, v);
    return 0;
}

int intc_irq_mask(uint32_t mask)
{
    if (!intc_check_access()) return -1;
    uint32_t v = hw_read(IRQ_MASK_ADDR);
    v |= mask;
    hw_write(IRQ_MASK_ADDR, v);
    return 0;
}

int intc_irq_unmask(uint32_t mask)
{
    if (!intc_check_access()) return -1;
    uint32_t v = hw_read(IRQ_MASK_ADDR);
    v &= ~mask;
    hw_write(IRQ_MASK_ADDR, v);
    return 0;
}

uint32_t intc_irq_get_enable(void)
{
    return hw_read(IRQ_ENABLE_ADDR);
}

uint32_t intc_irq_get_mask(void)
{
    return hw_read(IRQ_MASK_ADDR);
}

/* ---- 触发类型/极性 ---- */
int intc_irq_set_trigger(uint32_t mask, intc_trigger_t trig)
{
    if (!intc_check_access()) return -1;
    uint32_t v = hw_read(IRQ_TRIGGER_ADDR);
    if (trig == INTC_TRIG_EDGE) v |= mask;     /* 选定位置 1 = 边沿 */
    else                        v &= ~mask;    /* 选定位清 0 = 电平 */
    hw_write(IRQ_TRIGGER_ADDR, v);
    return 0;
}

int intc_irq_set_polarity(uint32_t mask, intc_polarity_t pol)
{
    if (!intc_check_access()) return -1;
    uint32_t v = hw_read(IRQ_POLARITY_ADDR);
    if (pol == INTC_POL_HIGH) v |= mask;       /* 选定位置 1 = 高有效 */
    else                      v &= ~mask;      /* 选定位清 0 = 低有效 */
    hw_write(IRQ_POLARITY_ADDR, v);
    return 0;
}

uint32_t intc_irq_get_trigger(void)
{
    return hw_read(IRQ_TRIGGER_ADDR);
}

uint32_t intc_irq_get_polarity(void)
{
    return hw_read(IRQ_POLARITY_ADDR);
}

/* ---- pending/ack ---- */
int intc_irq_ack(uint32_t mask)
{
    if (!intc_check_access()) return -1;
    uint32_t v = hw_read(IRQ_ACK_ADDR);
    v |= mask;                                  /* W1C：写 1 清对应 pending */
    hw_write(IRQ_ACK_ADDR, v);
    return 0;
}

uint32_t intc_irq_get_raw(void)
{
    return hw_read(IRQ_RAW_ADDR);
}

uint32_t intc_irq_get_pending(void)
{
    return hw_read(IRQ_PENDING_ADDR);
}

uint32_t intc_get_socket_status(void)
{
    return hw_read(SOCKET_STATUS_ADDR);
}
