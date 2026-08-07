/**
 * @file    module_driver.c
 * @brief   ModuleController 控制器驱动实现
 */
#include <stddef.h>
#include "module_driver.h"

/* -------------------------------------------------------------------------
 * 钩子与默认实现
 * ------------------------------------------------------------------------- */
static reg_read_fn_t  g_read_fn  = NULL;
static reg_write_fn_t g_write_fn = NULL;

/* 默认实现：把基地址区域当作内存访问，单测会替换此行为 */
static uint32_t default_reg_read(uint32_t addr)
{
    return *((volatile uint32_t *)(uintptr_t)addr);
}

static void default_reg_write(uint32_t addr, uint32_t value)
{
    *((volatile uint32_t *)(uintptr_t)addr) = value;
}

void module_driver_register_io(reg_read_fn_t rfn, reg_write_fn_t wfn)
{
    g_read_fn  = rfn;
    g_write_fn = wfn;
}

static inline uint32_t hw_read(uint32_t addr)
{
    return (g_read_fn ? g_read_fn : default_reg_read)(addr);
}

static inline void hw_write(uint32_t addr, uint32_t value)
{
    (g_write_fn ? g_write_fn : default_reg_write)(addr, value);
}

/* -------------------------------------------------------------------------
 * 驱动 API
 * ------------------------------------------------------------------------- */
void module_driver_init(void)
{
    hw_write(CTRL_REG_ADDR,   CTRL_REG_RESET_VALUE);
    /* STATUS 为 RO，理论上不可写，但部分平台需要同步复位镜像 */
}

void module_enable(void)
{
    uint32_t v = hw_read(CTRL_REG_ADDR);
    v |= CTRL_REG_MODULE_EN_Msk;
    hw_write(CTRL_REG_ADDR, v);
}

void module_disable(void)
{
    uint32_t v = hw_read(CTRL_REG_ADDR);
    v &= ~CTRL_REG_MODULE_EN_Msk;
    hw_write(CTRL_REG_ADDR, v);
}

uint32_t module_is_enabled(void)
{
    return (hw_read(CTRL_REG_ADDR) & CTRL_REG_MODULE_EN_Msk)
           >> CTRL_REG_MODULE_EN_Pos;
}

void module_soft_reset(void)
{
    uint32_t v = hw_read(CTRL_REG_ADDR);
    v |= CTRL_REG_SOFT_RESET_Msk;
    hw_write(CTRL_REG_ADDR, v);
    /* 硬件通常自动清零；若需要等待复位完成，可在此轮询状态 */
}

module_state_t module_get_state(void)
{
    uint32_t v = hw_read(STATUS_REG_ADDR);
    return (module_state_t)((v & STATUS_REG_MODULE_STATE_Msk)
                            >> STATUS_REG_MODULE_STATE_Pos);
}

uint32_t module_fifo_is_empty(void)
{
    return (hw_read(STATUS_REG_ADDR) & STATUS_REG_FIFO_EMPTY_Msk)
           >> STATUS_REG_FIFO_EMPTY_Pos;
}

uint32_t module_fifo_is_full(void)
{
    return (hw_read(STATUS_REG_ADDR) & STATUS_REG_FIFO_FULL_Msk)
           >> STATUS_REG_FIFO_FULL_Pos;
}

uint32_t module_has_error(void)
{
    return (hw_read(STATUS_REG_ADDR) & STATUS_REG_ERR_FLAG_Msk)
           >> STATUS_REG_ERR_FLAG_Pos;
}

uint32_t module_get_err_code(void)
{
    return (hw_read(STATUS_REG_ADDR) & STATUS_REG_ERR_CODE_Msk)
           >> STATUS_REG_ERR_CODE_Pos;
}
