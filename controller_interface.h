/**
 * @file    controller_interface.h
 * @brief   IntcController (AXI 中断控制器) 寄存器接口定义
 * @note    由 controller_interface.json 生成, 版本 1.0.0
 *
 * 总线约定：AXI4-Lite
 *   - 数据宽度 32-bit，地址 4-byte 对齐
 *   - 单拍读：AR/R 通道，返回一个 32-bit word
 *   - 单拍写：AW/W/B 通道，提交一个 32-bit word
 *   驱动层将上述事务抽象为 reg_read_fn_t / reg_write_fn_t 钩子，
 *   平台负责把钩子接到真实 AXI master 上。
 */
#ifndef CONTROLLER_INTERFACE_H
#define CONTROLLER_INTERFACE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/* ======== 基地址与寄存器数量 ======== */
#define INTC_BASE_ADDR              0x40000000U
#define INTC_NUM_REGS               10U          /* 0x00..0x24, 步长 4 */
#define INTC_NUM_IRQS               32U          /* 32 路中断请求 */

/* ======== 寄存器偏移 ======== */
#define INTC_CTRL_OFFSET            0x00U
#define IRQ_ENABLE_OFFSET           0x04U
#define IRQ_MASK_OFFSET             0x08U
#define IRQ_RAW_OFFSET              0x0CU
#define IRQ_PENDING_OFFSET          0x10U
#define IRQ_ACK_OFFSET              0x14U
#define IRQ_TRIGGER_OFFSET          0x18U
#define IRQ_POLARITY_OFFSET         0x1CU
#define SOCKET_ID_OFFSET            0x20U
#define SOCKET_STATUS_OFFSET        0x24U

/* ======== 寄存器绝对地址 ======== */
#define INTC_CTRL_ADDR              (INTC_BASE_ADDR + INTC_CTRL_OFFSET)
#define IRQ_ENABLE_ADDR             (INTC_BASE_ADDR + IRQ_ENABLE_OFFSET)
#define IRQ_MASK_ADDR               (INTC_BASE_ADDR + IRQ_MASK_OFFSET)
#define IRQ_RAW_ADDR                (INTC_BASE_ADDR + IRQ_RAW_OFFSET)
#define IRQ_PENDING_ADDR            (INTC_BASE_ADDR + IRQ_PENDING_OFFSET)
#define IRQ_ACK_ADDR                (INTC_BASE_ADDR + IRQ_ACK_OFFSET)
#define IRQ_TRIGGER_ADDR            (INTC_BASE_ADDR + IRQ_TRIGGER_OFFSET)
#define IRQ_POLARITY_ADDR           (INTC_BASE_ADDR + IRQ_POLARITY_OFFSET)
#define SOCKET_ID_ADDR              (INTC_BASE_ADDR + SOCKET_ID_OFFSET)
#define SOCKET_STATUS_ADDR          (INTC_BASE_ADDR + SOCKET_STATUS_OFFSET)

/* ======== INTC_CTRL (0x00, RW) ======== */
#define INTC_CTRL_GLOBAL_EN_Pos     0U
#define INTC_CTRL_GLOBAL_EN_Msk     (0x1U << INTC_CTRL_GLOBAL_EN_Pos)
#define INTC_CTRL_GLOBAL_EN_DIS     0x0U
#define INTC_CTRL_GLOBAL_EN_EN      0x1U

#define INTC_CTRL_SOFT_RESET_Pos    1U
#define INTC_CTRL_SOFT_RESET_Msk    (0x1U << INTC_CTRL_SOFT_RESET_Pos)
#define INTC_CTRL_SOFT_RESET_NORM  0x0U
#define INTC_CTRL_SOFT_RESET_RST   0x1U

#define INTC_CTRL_LOCAL_EN_Pos      2U
#define INTC_CTRL_LOCAL_EN_Msk      (0x1U << INTC_CTRL_LOCAL_EN_Pos)
#define INTC_CTRL_LOCAL_EN_OPEN     0x0U   /* 开放访问 */
#define INTC_CTRL_LOCAL_EN_LOCKED   0x1U   /* 仅 owner socket 可写敏感寄存器 */

/* ======== IRQ_ENABLE (0x04, RW) 32-bit ======== */
#define IRQ_ENABLE_ENABLE_Pos       0U
#define IRQ_ENABLE_ENABLE_Msk       (0xFFFFFFFFU << IRQ_ENABLE_ENABLE_Pos)

/* ======== IRQ_MASK (0x08, RW) 32-bit ======== */
#define IRQ_MASK_MASK_Pos           0U
#define IRQ_MASK_MASK_Msk           (0xFFFFFFFFU << IRQ_MASK_MASK_Pos)

/* ======== IRQ_RAW (0x0C, RO) 32-bit ======== */
#define IRQ_RAW_RAW_Pos             0U
#define IRQ_RAW_RAW_Msk             (0xFFFFFFFFU << IRQ_RAW_RAW_Pos)

/* ======== IRQ_PENDING (0x10, RO) 32-bit ======== */
#define IRQ_PENDING_PENDING_Pos     0U
#define IRQ_PENDING_PENDING_Msk     (0xFFFFFFFFU << IRQ_PENDING_PENDING_Pos)

/* ======== IRQ_ACK (0x14, RW/W1C) 32-bit ======== */
#define IRQ_ACK_ACK_Pos             0U
#define IRQ_ACK_ACK_Msk             (0xFFFFFFFFU << IRQ_ACK_ACK_Pos)

/* ======== IRQ_TRIGGER (0x18, RW) 32-bit ======== */
#define IRQ_TRIGGER_TRIGGER_Pos     0U
#define IRQ_TRIGGER_TRIGGER_Msk     (0xFFFFFFFFU << IRQ_TRIGGER_TRIGGER_Pos)
#define INTC_TRIGGER_LEVEL          0x0U   /* 电平触发 */
#define INTC_TRIGGER_EDGE           0x1U   /* 边沿触发 */

/* ======== IRQ_POLARITY (0x1C, RW) 32-bit ======== */
#define IRQ_POLARITY_POLARITY_Pos   0U
#define IRQ_POLARITY_POLARITY_Msk   (0xFFFFFFFFU << IRQ_POLARITY_POLARITY_Pos)
#define INTC_POLARITY_LOW           0x0U   /* 低有效 */
#define INTC_POLARITY_HIGH          0x1U   /* 高有效 */

/* ======== SOCKET_ID (0x20, RW) ======== */
#define SOCKET_ID_ID_Pos            0U
#define SOCKET_ID_ID_Msk            (0xFFU << SOCKET_ID_ID_Pos)

/* ======== SOCKET_STATUS (0x24, RO) ======== */
#define SOCKET_STATUS_LOCKED_Pos         0U
#define SOCKET_STATUS_LOCKED_Msk        (0x1U << SOCKET_STATUS_LOCKED_Pos)
#define SOCKET_STATUS_OWNER_ID_Pos      8U
#define SOCKET_STATUS_OWNER_ID_Msk      (0xFFU << SOCKET_STATUS_OWNER_ID_Pos)
#define SOCKET_STATUS_ACCESS_DENIED_Pos 16U
#define SOCKET_STATUS_ACCESS_DENIED_Msk  (0x1U << SOCKET_STATUS_ACCESS_DENIED_Pos)

/* ======== 复位值 ======== */
#define INTC_CTRL_RESET_VALUE        0x00000000U
#define IRQ_ENABLE_RESET_VALUE       0x00000000U
#define IRQ_MASK_RESET_VALUE         0xFFFFFFFFU   /* 默认全屏蔽，安全 */
#define IRQ_RAW_RESET_VALUE          0x00000000U
#define IRQ_PENDING_RESET_VALUE      0x00000000U
#define IRQ_ACK_RESET_VALUE          0x00000000U
#define IRQ_TRIGGER_RESET_VALUE      0x00000000U
#define IRQ_POLARITY_RESET_VALUE     0xFFFFFFFFU   /* 默认全高有效 */
#define SOCKET_ID_RESET_VALUE       0x00000000U
#define SOCKET_STATUS_RESET_VALUE   0x00000000U

/** 中断触发类型枚举 */
typedef enum {
    INTC_TRIG_LEVEL = INTC_TRIGGER_LEVEL,
    INTC_TRIG_EDGE  = INTC_TRIGGER_EDGE,
} intc_trigger_t;

/** 中断极性枚举 */
typedef enum {
    INTC_POL_LOW  = INTC_POLARITY_LOW,
    INTC_POL_HIGH = INTC_POLARITY_HIGH,
} intc_polarity_t;

/** local socket 访问状态枚举 */
typedef enum {
    INTC_ACCESS_OPEN   = 0,   /* LOCAL_EN=0，开放访问 */
    INTC_ACCESS_OWNER  = 1,   /* LOCAL_EN=1 且当前 socket 为 owner */
    INTC_ACCESS_DENIED  = 2,  /* LOCAL_EN=1 且当前 socket 非 owner */
} intc_access_state_t;

#ifdef __cplusplus
}
#endif

#endif /* CONTROLLER_INTERFACE_H */
