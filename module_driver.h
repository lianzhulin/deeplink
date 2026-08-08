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

/**
 * @brief 为默认 I/O 设置「内存映射镜像」数组（2 个 uint32_t：
 *        [0]=CTRL_REG @ 0x40000000, [1]=STATUS_REG @ 0x40000004）。
 *        仅当未注入自定义钩子时生效。
 *        通过此接口可让默认 I/O 在用户态单测中也能安全运行。
 *        传入 NULL 则回归真实硬件 volatile 访问模式（嵌入式默认）。
 */
void module_driver_set_mmio_mirror(uint32_t mirror[2]);

/**
 * @brief UT 专用：直接调用默认 I/O 的内部分支（不需要真实硬件地址映射）。
 *
 *   - 当 mirror==NULL 且 hook==NULL 时，在测试中若直接驱动 API 访问
 *     0x40000000/4 会触发真实 volatile 地址异常；
 *   - 因此驱动额外提供一个「模拟寄存器缓冲区」接口：在内部使用本地
 *     uint32_t 模拟寄存器完成 default_reg_read/write 的所有分支覆盖
 *     （mirror != NULL 与 == NULL 两条路径，idx=0/1/<0 三分支）。
 *
 *   param mode = 0: 运行 mirror!=NULL 路径
 *   param mode = 1: 运行 g_mirror==NULL+volatile 路径
 *                  （通过本地 buf 模拟，不触及真实 0x40000000）
 *   本接口仅在 UT 编译单元调用，产品代码不使用。
 */
void module_driver_ut_probe_default_io(int mode);

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
