/*
 * cuse_deeplink.c — CUSE（用户空间字符设备）示例
 *
 * 功能：将 deeplink 项目的 module_driver 封装成 /dev/deeplink 字符设备。
 *       用户态进程通过 read() 读取寄存器、write() 写 CTRL_REG、
 *       ioctl() 直接调用驱动 API（enable/disable/reset/get_state 等）。
 *
 * 依赖：libfuse3-dev（cuse_lowlevel API）+ 内核必须启用 CONFIG_CUSE
 *       （当前沙箱内核 CONFIG_CUSE=n，因此本程序需在启用 CUSE 的机器上运行；
 *        本项目另提供 fuse_deeplink_sim.c 作为可试运行的等价模拟实现。）
 *
 * 编译：
 *   gcc -std=c99 -Wall -Wextra -D_FILE_OFFSET_BITS=64 \
 *       $(pkg-config --cflags fuse3) \
 *       -o cuse_deeplink cuse_deeplink.c module_driver.c \
 *       $(pkg-config --libs fuse3)
 *
 * 运行（需 root / mknod 权限）：
 *   sudo ./cuse_deeplink -f --name=deeplink
 *   # 另一终端：
 *   sudo mknod /dev/deeplink c MAJOR MINOR   # MAJOR/MINOR 由程序输出
 *   sudo chmod 666 /dev/deeplink
 *   cat /dev/deeplink          # 读 8 字节 (CTRL + STATUS)
 *   echo -ne '\x01\x00\x00\x00' > /dev/deeplink   # 写 CTRL (MODULE_EN=1)
 */

#define FUSE_USE_VERSION 34
#include <fuse3/cuse_lowlevel.h>

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <sys/ioctl.h>

#include "module_driver.h"

/* ------------------------------------------------------------------
 * ioctl 命令定义（8 位方向 + 8 位类型 + 16 位序号）
 *   'D' = Deeplink 驱动类型号
 * ------------------------------------------------------------------ */
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

/* 设备的"文件内容"长度 = 2 个 32-bit 寄存器：CTRL(@0..3) + STATUS(@4..7) */
#define DEV_BYTES 8u

/* 用户态维护的寄存器镜像（module_driver 将通过钩子写入这里） */
static uint32_t g_regs_mirror[2];

/* ---------------- I/O 钩子：把驱动读写引到 g_regs_mirror ---------------- */
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
        /* 模拟硬件：写 SOFT_RESET 后自动清零 & 更新 STATUS 位 */
        if ((val & CTRL_REG_SOFT_RESET_Msk) != 0u) {
            g_regs_mirror[0] &= ~CTRL_REG_SOFT_RESET_Msk;
        }
    }
    /* STATUS 只读，忽略写 */
}

/* ---------------- CUSE 回调 ---------------- */

static void cuse_open(fuse_req_t req, struct fuse_file_info *fi)
{
    (void)fi;
    fuse_reply_open(req, fi);
}

/*
 * read：按字节偏移把 {CTRL, STATUS} 复制给用户。
 *       典型用法：pread(fd, buf, 8, 0) 一次读完两个寄存器。
 */
static void cuse_read(fuse_req_t req, size_t size, off_t off,
                      struct fuse_file_info *fi)
{
    (void)fi;
    uint8_t bytes[DEV_BYTES];

    memcpy(bytes + 0, &g_regs_mirror[0], 4);  /* CTRL   */
    memcpy(bytes + 4, &g_regs_mirror[1], 4);  /* STATUS */

    if ((size_t)off >= DEV_BYTES) {
        fuse_reply_buf(req, NULL, 0);
        return;
    }
    size_t avail = DEV_BYTES - (size_t)off;
    if (size > avail) size = avail;
    fuse_reply_buf(req, (const char *)bytes + off, size);
}

/*
 * write：写 CTRL_REG（offset 0..3），其它范围无效。
 *        一次 pwrite(fd, &val, 4, 0) 等价于写 CTRL_REG。
 */
static void cuse_write(fuse_req_t req, const char *buf, size_t size, off_t off,
                       struct fuse_file_info *fi)
{
    (void)fi;
    if (off < 0 || (size_t)off + size > 4) {
        fuse_reply_err(req, EINVAL);
        return;
    }
    /* 只接受完整 4 字节写入 */
    if (off != 0 || size != 4) {
        fuse_reply_err(req, EINVAL);
        return;
    }
    uint32_t v;
    memcpy(&v, buf, 4);
    hook_reg_write(CTRL_REG_ADDR, v);
    fuse_reply_write(req, size);
}

static void cuse_release(fuse_req_t req, struct fuse_file_info *fi)
{
    (void)fi;
    fuse_reply_err(req, 0);
}

static void cuse_flush(fuse_req_t req, struct fuse_file_info *fi)
{
    (void)fi;
    fuse_reply_err(req, 0);
}

static void cuse_fsync(fuse_req_t req, int datasync, struct fuse_file_info *fi)
{
    (void)datasync;
    (void)fi;
    fuse_reply_err(req, 0);
}

static void cuse_ioctl(fuse_req_t req, int cmd, void *arg,
                       struct fuse_file_info *fi, unsigned int flags,
                       const void *in_buf, size_t in_bufsz, size_t out_bufsz)
{
    (void)fi;
    (void)arg;
    (void)in_buf;
    (void)in_bufsz;

    if (flags & FUSE_IOCTL_COMPAT) {
        fuse_reply_err(req, ENOSYS);
        return;
    }

    switch (cmd) {
    case DEEPLINK_IOCTL_ENABLE:
        module_enable();
        fuse_reply_ioctl(req, 0, NULL, 0);
        break;
    case DEEPLINK_IOCTL_DISABLE:
        module_disable();
        fuse_reply_ioctl(req, 0, NULL, 0);
        break;
    case DEEPLINK_IOCTL_SOFT_RESET:
        module_soft_reset();
        fuse_reply_ioctl(req, 0, NULL, 0);
        break;
    case DEEPLINK_IOCTL_IS_ENABLED: {
        uint32_t v = module_is_enabled();
        if (out_bufsz < sizeof(v)) { fuse_reply_err(req, EINVAL); break; }
        fuse_reply_ioctl(req, 0, &v, sizeof(v));
        break;
    }
    case DEEPLINK_IOCTL_GET_STATE: {
        uint32_t v = (uint32_t)module_get_state();
        if (out_bufsz < sizeof(v)) { fuse_reply_err(req, EINVAL); break; }
        fuse_reply_ioctl(req, 0, &v, sizeof(v));
        break;
    }
    case DEEPLINK_IOCTL_HAS_ERROR: {
        uint32_t v = module_has_error();
        if (out_bufsz < sizeof(v)) { fuse_reply_err(req, EINVAL); break; }
        fuse_reply_ioctl(req, 0, &v, sizeof(v));
        break;
    }
    case DEEPLINK_IOCTL_GET_ERR: {
        uint32_t v = module_get_err_code();
        if (out_bufsz < sizeof(v)) { fuse_reply_err(req, EINVAL); break; }
        fuse_reply_ioctl(req, 0, &v, sizeof(v));
        break;
    }
    case DEEPLINK_IOCTL_FIFO_EMPTY: {
        uint32_t v = module_fifo_is_empty();
        if (out_bufsz < sizeof(v)) { fuse_reply_err(req, EINVAL); break; }
        fuse_reply_ioctl(req, 0, &v, sizeof(v));
        break;
    }
    case DEEPLINK_IOCTL_FIFO_FULL: {
        uint32_t v = module_fifo_is_full();
        if (out_bufsz < sizeof(v)) { fuse_reply_err(req, EINVAL); break; }
        fuse_reply_ioctl(req, 0, &v, sizeof(v));
        break;
    }
    default:
        fuse_reply_err(req, ENOTTY);
    }
}

static void cuse_init(void *userdata, struct fuse_conn_info *conn)
{
    (void)conn;
    (void)userdata;
    /* STATUS 寄存器复位值（模拟硬件上电状态） */
    g_regs_mirror[1] = STATUS_REG_RESET_VALUE;
    module_driver_register_io(hook_reg_read, hook_reg_write);
    module_driver_init();
    fprintf(stderr, "[cuse] deeplink init done. CTRL=0x%08X STATUS=0x%08X\n",
            g_regs_mirror[0], g_regs_mirror[1]);
}

static void cuse_destroy(void *userdata)
{
    (void)userdata;
    fprintf(stderr, "[cuse] deeplink destroy.\n");
}

/* ---------------- main ---------------- */

int main(int argc, char *argv[])
{
    struct cuse_info ci;
    struct cuse_lowlevel_ops clop;

    memset(&ci, 0, sizeof(ci));
    memset(&clop, 0, sizeof(clop));

    /* dev_info_argv: 形如 "DEVNAME=deeplink"，用于 udev 自动命名 /dev/deeplink */
    static const char *dev_info_argv[] = { "DEVNAME=deeplink" };
    ci.dev_info_argc = 1;
    ci.dev_info_argv = dev_info_argv;
    ci.flags = CUSE_UNRESTRICTED_IOCTL;

    clop.init     = cuse_init;
    clop.destroy  = cuse_destroy;
    clop.open     = cuse_open;
    clop.read     = cuse_read;
    clop.write    = cuse_write;
    clop.flush    = cuse_flush;
    clop.release  = cuse_release;
    clop.fsync    = cuse_fsync;
    clop.ioctl    = cuse_ioctl;

    return cuse_lowlevel_main(argc, argv, &ci, &clop, NULL);
}
