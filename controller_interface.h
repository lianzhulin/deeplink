/**
 * @file    controller_interface.h
 * @brief   ModuleController 控制器寄存器接口定义
 * @note    由 controller_interface.json 生成, 版本 1.0.0
 */
#ifndef CONTROLLER_INTERFACE_H
#define CONTROLLER_INTERFACE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/* ======== 基地址与寄存器偏移 ======== */
#define MODULE_CTRL_BASE_ADDR       0x40000000U
#define CTRL_REG_OFFSET             0x00U
#define STATUS_REG_OFFSET           0x04U

#define CTRL_REG_ADDR               (MODULE_CTRL_BASE_ADDR + CTRL_REG_OFFSET)
#define STATUS_REG_ADDR             (MODULE_CTRL_BASE_ADDR + STATUS_REG_OFFSET)

/* ======== CTRL_REG (0x00, RW) ======== */
#define CTRL_REG_MODULE_EN_Pos      0U
#define CTRL_REG_MODULE_EN_Msk      (0x1U << CTRL_REG_MODULE_EN_Pos)
#define CTRL_REG_MODULE_EN_DIS      0x0U
#define CTRL_REG_MODULE_EN_EN       0x1U

#define CTRL_REG_SOFT_RESET_Pos     1U
#define CTRL_REG_SOFT_RESET_Msk     (0x1U << CTRL_REG_SOFT_RESET_Pos)
#define CTRL_REG_SOFT_RESET_NORM    0x0U
#define CTRL_REG_SOFT_RESET_RST     0x1U

/* ======== STATUS_REG (0x04, RO) ======== */
#define STATUS_REG_MODULE_STATE_Pos 0U
#define STATUS_REG_MODULE_STATE_Msk (0x3U << STATUS_REG_MODULE_STATE_Pos)
#define STATUS_REG_MODULE_STATE_IDLE    0x0U
#define STATUS_REG_MODULE_STATE_RUNNING 0x1U
#define STATUS_REG_MODULE_STATE_BUSY    0x2U
#define STATUS_REG_MODULE_STATE_ERROR   0x3U

#define STATUS_REG_FIFO_EMPTY_Pos   2U
#define STATUS_REG_FIFO_EMPTY_Msk   (0x1U << STATUS_REG_FIFO_EMPTY_Pos)

#define STATUS_REG_FIFO_FULL_Pos    3U
#define STATUS_REG_FIFO_FULL_Msk    (0x1U << STATUS_REG_FIFO_FULL_Pos)

#define STATUS_REG_ERR_FLAG_Pos     4U
#define STATUS_REG_ERR_FLAG_Msk     (0x1U << STATUS_REG_ERR_FLAG_Pos)

#define STATUS_REG_ERR_CODE_Pos     8U
#define STATUS_REG_ERR_CODE_Msk     (0xFFU << STATUS_REG_ERR_CODE_Pos)

/* ======== 复位值 ======== */
#define CTRL_REG_RESET_VALUE        0x00000000U
#define STATUS_REG_RESET_VALUE      (0x1U << STATUS_REG_FIFO_EMPTY_Pos)

/** 模块工作状态枚举 */
typedef enum {
    MODULE_STATE_IDLE    = STATUS_REG_MODULE_STATE_IDLE,
    MODULE_STATE_RUNNING = STATUS_REG_MODULE_STATE_RUNNING,
    MODULE_STATE_BUSY    = STATUS_REG_MODULE_STATE_BUSY,
    MODULE_STATE_ERROR   = STATUS_REG_MODULE_STATE_ERROR,
} module_state_t;

#ifdef __cplusplus
}
#endif

#endif /* CONTROLLER_INTERFACE_H */
