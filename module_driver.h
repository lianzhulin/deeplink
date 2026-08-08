/**
 * @file    module_driver.h
 * @brief   ModuleController 控制器驱动对外接口
 */
#ifndef MODULE_DRIVER_H
#define MODULE_DRIVER_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include "controller_interface.h"

/* -------------------------------------------------------------------------
 * 可注入的读写回调：用于单测时替换真实寄存器访问
 * ------------------------------------------------------------------------- */
typedef uint32_t (*reg_read_fn_t)(uint32_t addr);
typedef void     (*reg_write_fn_t)(uint32_t addr, uint32_t value);

/**
 * @brief 注入自定义寄存器读写钩子；传入 NULL 恢复默认实现
 */
void module_driver_register_io(reg_read_fn_t rfn, reg_write_fn_t wfn);

/* -------------------------------------------------------------------------
 * 驱动 API
 * ------------------------------------------------------------------------- */

/**
 * @brief  初始化驱动：将寄存器写入复位值
 */
void module_driver_init(void);

/**
 * @brief  使能模块 (MODULE_EN = 1)
 */
void module_enable(void);

/**
 * @brief  关闭模块 (MODULE_EN = 0)
 */
void module_disable(void);

/**
 * @brief  查询模块是否已使能
 * @return 1: 使能, 0: 关闭
 */
uint32_t module_is_enabled(void);

/**
 * @brief  触发软件复位 (SOFT_RESET=1，写一次即可，硬件自动清零)
 */
void module_soft_reset(void);

/**
 * @brief  获取模块当前工作状态
 */
module_state_t module_get_state(void);

/**
 * @brief  查询 FIFO 是否为空 (1: 空, 0: 非空)
 */
uint32_t module_fifo_is_empty(void);

/**
 * @brief  查询 FIFO 是否为满 (1: 满, 0: 未满)
 */
uint32_t module_fifo_is_full(void);

/**
 * @brief  查询是否有错误标志 (1: 有错误, 0: 无错误)
 */
uint32_t module_has_error(void);

/**
 * @brief  获取错误代码 (仅在 module_has_error()==1 时有效)
 */
uint32_t module_get_err_code(void);

#ifdef __cplusplus
}
#endif

#endif /* MODULE_DRIVER_H */
