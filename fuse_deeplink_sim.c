/*
 * fuse_deeplink_sim.c — FUSE 模拟 CUSE 设备（可在当前沙箱试运行）
 *
 * 背景：当前沙箱内核未启用 CONFIG_CUSE，无法注册真实字符设备节点。
 *       本程序使用普通 FUSE 文件系统，挂载一个目录（例如 /tmp/deeplink_fs），
 *       目录里只有一个文件 `deeplink`——对该文件执行 read/write/ioctl，
 *       其行为与 cuse_deeplink.c 完全一致（使用同一份 module_driver 寄存器抽象）。
 *
 * 这相当于把 CUSE 字符设备"等价地"映射成 FUSE 挂载目录里的一个普通文件，
 * 底层驱动 API 100% 复用（同样通过自定义 I/O 钩子接入 module_driver）。
 *
 * 编译：
 *   gcc -std=c99 -Wall -Wextra -D_FILE_OFFSET_BITS=64 \
 *       $(pkg-config --cflags fuse3) \
 *       -o fuse_deeplink_sim fuse_deeplink_sim.c module_driver.c \
 *       $(pkg-config --libs fuse3)
 *
 * 运行：
 *   mkdir -p /tmp/deeplink_fs
 *   ./fuse_deeplink_sim -f /tmp/deeplink_fs    # -f 前台运行，方便看日志
 *   # 另一终端：
 *   ls -la /tmp/deeplink_fs/deeplink
 *   xxd -l 8 /tmp/deeplink_fs/deeplink         # 读 8 字节 (CTRL + STATUS)
 *   printf '\x01\x00\x00\x00' | dd of=/tmp/deeplink_fs/deeplink bs=1 count=4 conv=notrunc 2>/dev/null
 *   # 读 deeplink.h 中的 ioctl 定义，用户程序也可以用 ioctl(fd, CMD, &val)
 */

#define FUSE_USE_VERSION 34
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include <fuse3/fuse.h>

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <time.h>

#include "module_driver.h"

/* ---------- ioctl 命令（与 cuse_deeplink.c 完全相同） ---------- */
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

#define DEV_FILENAME "deeplink"
#define DEV_BYTES    8u   /* CTRL(4) + STATUS(4) */

/* 全局状态：寄存器镜像 */
static uint32_t g_regs_mirror[2];

/* ---------- I/O 钩子：驱动与 g_regs_mirror 桥接 ---------- */
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
        if ((val & CTRL_REG_SOFT_RESET_Msk) != 0u) {
            g_regs_mirror[0] &= ~CTRL_REG_SOFT_RESET_Msk;
        }
    }
}

/* ---------- 将 {CTRL,STATUS} 打包到字节流，或反之解包 ---------- */
static inline void regs_to_bytes(uint8_t out[DEV_BYTES])
{
    memcpy(out + 0, &g_regs_mirror[0], 4);
    memcpy(out + 4, &g_regs_mirror[1], 4);
}

/* ---------- FUSE 回调：只实现 /deeplink 一个文件 ---------- */

static int sim_getattr(const char *path, struct stat *stbuf,
                       struct fuse_file_info *fi)
{
    (void)fi;
    memset(stbuf, 0, sizeof(*stbuf));
    if (strcmp(path, "/") == 0) {
        stbuf->st_mode  = S_IFDIR | 0755;
        stbuf->st_nlink = 2;
        return 0;
    }
    if (strcmp(path, "/" DEV_FILENAME) == 0) {
        stbuf->st_mode  = S_IFREG | 0666;   /* 普通文件，模拟 cdev 的接口语义 */
        stbuf->st_nlink = 1;
        stbuf->st_size  = DEV_BYTES;        /* 8 字节寄存器视图 */
        return 0;
    }
    return -ENOENT;
}

static int sim_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                       off_t off, struct fuse_file_info *fi,
                       enum fuse_readdir_flags flags)
{
    (void)off; (void)fi; (void)flags;
    if (strcmp(path, "/") != 0) return -ENOENT;
    filler(buf, ".",          NULL, 0, 0);
    filler(buf, "..",         NULL, 0, 0);
    filler(buf, DEV_FILENAME, NULL, 0, 0);
    return 0;
}

static int sim_open(const char *path, struct fuse_file_info *fi)
{
    if (strcmp(path, "/" DEV_FILENAME) != 0) return -ENOENT;
    (void)fi;
    return 0;
}

static int sim_read(const char *path, char *buf, size_t size, off_t off,
                    struct fuse_file_info *fi)
{
    (void)fi;
    if (strcmp(path, "/" DEV_FILENAME) != 0) return -ENOENT;

    uint8_t bytes[DEV_BYTES];
    regs_to_bytes(bytes);

    if ((size_t)off >= DEV_BYTES) return 0;
    size_t avail = DEV_BYTES - (size_t)off;
    if (size > avail) size = avail;
    memcpy(buf, bytes + off, size);
    return (int)size;
}

static int sim_write(const char *path, const char *buf, size_t size, off_t off,
                     struct fuse_file_info *fi)
{
    (void)fi;
    if (strcmp(path, "/" DEV_FILENAME) != 0) return -ENOENT;
    if (off < 0 || (size_t)off + size > 4) return -EINVAL;
    if (off != 0 || size != 4) return -EINVAL;   /* 只接受完整 4 字节写 CTRL */

    uint32_t v;
    memcpy(&v, buf, 4);
    hook_reg_write(CTRL_REG_ADDR, v);
    return (int)size;
}

static int sim_truncate(const char *path, off_t size,
                        struct fuse_file_info *fi)
{
    (void)fi;
    if (strcmp(path, "/" DEV_FILENAME) != 0) return -ENOENT;
    /* 设备视图大小固定 8 字节，不允许截断 */
    if (size != 0 && (size_t)size != DEV_BYTES) return -EINVAL;
    return 0;
}

/*
 * ioctl：FUSE 普通文件同样支持 FUSE_IOCTL（通过 /dev/fuse 转发到用户态）。
 *        这里实现的行为与 CUSE 版 cuse_ioctl 完全一致。
 */
static int sim_ioctl(const char *path, int cmd, void *arg,
                     struct fuse_file_info *fi, unsigned int flags, void *data)
{
    (void)fi;
    if (strcmp(path, "/" DEV_FILENAME) != 0) return -ENOENT;
    if (flags & FUSE_IOCTL_COMPAT) return -ENOSYS;

    switch (cmd) {
    case DEEPLINK_IOCTL_ENABLE:     module_enable();                 return 0;
    case DEEPLINK_IOCTL_DISABLE:    module_disable();                return 0;
    case DEEPLINK_IOCTL_SOFT_RESET: module_soft_reset();             return 0;
    case DEEPLINK_IOCTL_IS_ENABLED: *(uint32_t *)arg = module_is_enabled();   return 0;
    case DEEPLINK_IOCTL_GET_STATE:  *(uint32_t *)arg = (uint32_t)module_get_state(); return 0;
    case DEEPLINK_IOCTL_HAS_ERROR:  *(uint32_t *)arg = module_has_error();     return 0;
    case DEEPLINK_IOCTL_GET_ERR:    *(uint32_t *)arg = module_get_err_code();  return 0;
    case DEEPLINK_IOCTL_FIFO_EMPTY: *(uint32_t *)arg = module_fifo_is_empty(); return 0;
    case DEEPLINK_IOCTL_FIFO_FULL:  *(uint32_t *)arg = module_fifo_is_full();  return 0;
    default:
        (void)arg; (void)data;
        return -ENOTTY;
    }
}

static void *sim_init(struct fuse_conn_info *conn, struct fuse_config *cfg)
{
    (void)conn;
    (void)cfg;
    g_regs_mirror[1] = STATUS_REG_RESET_VALUE;
    module_driver_register_io(hook_reg_read, hook_reg_write);
    module_driver_init();
    fprintf(stderr, "[fuse-sim] deeplink init. CTRL=0x%08X STATUS=0x%08X\n",
            g_regs_mirror[0], g_regs_mirror[1]);
    return NULL;
}

static void sim_destroy(void *userdata)
{
    (void)userdata;
    fprintf(stderr, "[fuse-sim] deeplink destroy.\n");
}

/* ---------- FUSE 操作表 ---------- */
static const struct fuse_operations sim_ops = {
    .getattr   = sim_getattr,
    .readdir   = sim_readdir,
    .open      = sim_open,
    .read      = sim_read,
    .write     = sim_write,
    .truncate  = sim_truncate,
    .ioctl     = sim_ioctl,
    .init      = sim_init,
    .destroy   = sim_destroy,
};

int main(int argc, char *argv[])
{
    return fuse_main(argc, argv, &sim_ops, NULL);
}
