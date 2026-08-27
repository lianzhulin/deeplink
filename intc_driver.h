/**
 * @file    intc_driver.h
 * @brief   IntcController (AXI 中断控制器) 驱动对外接口
 *
 * 功能概览：
 *   - AXI4-Lite 总线接口：通过可注入 reg_read/reg_write 钩子接入平台 AXI master
 *   - 32 路中断请求：每路独立使能 (IRQ_ENABLE) 与屏蔽 (IRQ_MASK)
 *   - local socket 访问：LOCAL_EN=1 时，敏感寄存器仅 owner socket 可写
 */
#ifndef INTC_DRIVER_H
#define INTC_DRIVER_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include "controller_interface.h"

/* -------------------------------------------------------------------------
 * 可注入的读写回调：用于把驱动接到真实 AXI 总线（或 UT 中替换为模拟实现）
 *   - reg_read_fn_t  : 对应 AXI AR/R 单拍读
 *   - reg_write_fn_t : 对应 AXI AW/W/B 单拍写
 * ------------------------------------------------------------------------- */
typedef uint32_t (*reg_read_fn_t)(uint32_t addr);
typedef void     (*reg_write_fn_t)(uint32_t addr, uint32_t value);

/**
 * @brief 注入自定义寄存器读写钩子；传入 NULL 恢复默认实现
 */
void intc_driver_register_io(reg_read_fn_t rfn, reg_write_fn_t wfn);

/**
 * @brief 为默认 I/O 设置「内存映射镜像」数组（长度 >= INTC_NUM_REGS）。
 *        仅当未注入自定义钩子时生效。UT 中用此接口在用户态安全运行默认 I/O。
 *        传入 NULL 则回归真实硬件 volatile 访问模式（嵌入式默认）。
 */
void intc_driver_set_mmio_mirror(uint32_t *mirror);

/**
 * @brief UT 专用：直接调用默认 I/O 内部分支，覆盖 default_reg_read/write 全部分支。
 *   param mode = 0: mirror != NULL 路径
 *   param mode = 1: g_mirror == NULL + volatile 路径
 *                  （调用方需先 mmap MAP_FIXED 把 INTC_BASE_ADDR 页映射到用户态）
 *   本接口仅在 UT 编译单元调用，产品代码不使用。
 */
void intc_driver_ut_probe_default_io(int mode);

/* -------------------------------------------------------------------------
 * 驱动 API
 * ------------------------------------------------------------------------- */

/**
 * @brief  初始化驱动：把所有 RW 寄存器写入复位值（RO 寄存器写忽略）
 */
void intc_driver_init(void);

/* ---- 全局控制 ---- */
void     intc_global_enable(void);           /* GLOBAL_EN=1 */
void     intc_global_disable(void);          /* GLOBAL_EN=0 */
uint32_t intc_is_global_enabled(void);       /* 1=已使能 */
void     intc_soft_reset(void);              /* SOFT_RESET=1 */

/* ---- local socket 访问 ---- */
void     intc_set_local_socket(uint8_t socket_id);   /* 设置软件侧「本机 socket」上下文 */
uint8_t  intc_get_local_socket(void);
int      intc_claim_socket(uint8_t socket_id);      /* 申请成为 owner（写 SOCKET_ID）；返回 0/-1 */
int      intc_check_access(void);                    /* 当前 socket 是否有写敏感寄存器的权限：1=允许 0=拒绝 */
intc_access_state_t intc_get_access_state(void);    /* 访问状态枚举 */
void     intc_local_access_enable(void);            /* LOCAL_EN=1 */
void     intc_local_access_disable(void);           /* LOCAL_EN=0 */
uint32_t intc_is_local_access_enabled(void);        /* 1=LOCAL_EN 已置位 */

/* ---- 32 路中断：使能/屏蔽 ---- */
/* mask 的 bit i = 1 表示操作作用于 IRQ i；返回 0=成功, -1=访问被拒 */
int intc_irq_enable(uint32_t mask);          /* IRQ_ENABLE 置位 */
int intc_irq_disable(uint32_t mask);         /* IRQ_ENABLE 清位 */
int intc_irq_mask(uint32_t mask);            /* IRQ_MASK 置位（屏蔽） */
int intc_irq_unmask(uint32_t mask);          /* IRQ_MASK 清位（解除屏蔽） */
uint32_t intc_irq_get_enable(void);
uint32_t intc_irq_get_mask(void);

/* ---- 触发类型/极性 ---- */
/* mask 选定要操作的 IRQ 位；edge/pol 为 0 或 1，按 mask 展开到对应位 */
int intc_irq_set_trigger(uint32_t mask, intc_trigger_t trig);
int intc_irq_set_polarity(uint32_t mask, intc_polarity_t pol);
uint32_t intc_irq_get_trigger(void);
uint32_t intc_irq_get_polarity(void);

/* ---- pending/ack ---- */
int      intc_irq_ack(uint32_t mask);        /* W1C：写 1 清对应 pending */
uint32_t intc_irq_get_raw(void);             /* 原始输入 */
uint32_t intc_irq_get_pending(void);         /* = RAW & ENABLE & ~MASK */
uint32_t intc_get_socket_status(void);       /* SOCKET_STATUS 原值 */

#ifdef __cplusplus
}
#endif

#endif /* INTC_DRIVER_H */
