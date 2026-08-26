/*
 * deeplink_client.c — 客户端测试程序（通过 read/write/ioctl 操作设备）
 *
 * 使用方式：
 *   ./deeplink_client <设备路径>
 *   （真实 CUSE 设备为 /dev/deeplink；模拟版为 /tmp/deeplink_fs/deeplink）
 */

#define _XOPEN_SOURCE 700
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/types.h>

/* ---------- 与 cuse_deeplink.c / fuse_deeplink_sim.c 共享的 ioctl 定义 ---------- */
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

static const char *state_name(uint32_t s)
{
    switch (s) {
    case 0: return "IDLE";
    case 1: return "RUNNING";
    case 2: return "BUSY";
    case 3: return "ERROR";
    default: return "?";
    }
}

static void dump_regs(int fd)
{
    uint8_t buf[8];
    ssize_t n = pread(fd, buf, sizeof(buf), 0);
    if (n != 8) { fprintf(stderr, "  read regs failed: %s\n", strerror(errno)); return; }
    uint32_t ctrl, status;
    memcpy(&ctrl,   buf + 0, 4);
    memcpy(&status, buf + 4, 4);
    printf("  CTRL   = 0x%08X\n", ctrl);
    printf("  STATUS = 0x%08X\n", status);
}

static void write_ctrl(int fd, uint32_t val)
{
    ssize_t n = pwrite(fd, &val, 4, 0);
    if (n != 4) fprintf(stderr, "  write CTRL failed: %s\n", strerror(errno));
    else        printf("  [pwrite] CTRL <- 0x%08X\n", val);
}

#define IOCTL_R(fd, name, cmd) do {                        \
    uint32_t v = 0xDEADu;                                   \
    if (ioctl(fd, cmd, &v) < 0)                             \
        printf("  ioctl " #name " -> ERR %s\n", strerror(errno)); \
    else                                                    \
        printf("  ioctl " #name " = %u (0x%X)\n", v, v);    \
} while (0)

#define IOCTL_V(fd, name, cmd) do {                        \
    if (ioctl(fd, cmd, NULL) < 0)                           \
        printf("  ioctl " #name " -> ERR %s\n", strerror(errno)); \
    else                                                    \
        printf("  ioctl " #name " OK\n");                   \
} while (0)

int main(int argc, char *argv[])
{
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <device-path>\n", argv[0]);
        return 1;
    }
    const char *path = argv[1];
    int fd = open(path, O_RDWR);
    if (fd < 0) { perror(path); return 1; }
    printf("Opened: %s (fd=%d)\n\n", path, fd);

    /* ---- Step 1: 初始状态 ---- */
    printf("[Step 1] 初始寄存器值（刚 init 完）:\n");
    dump_regs(fd);
    printf("\n");

    printf("[Step 2] 通过 ioctl 查询初始状态:\n");
    IOCTL_R(fd, "IS_ENABLED",  DEEPLINK_IOCTL_IS_ENABLED);
    IOCTL_R(fd, "GET_STATE",   DEEPLINK_IOCTL_GET_STATE);
    IOCTL_R(fd, "HAS_ERROR",   DEEPLINK_IOCTL_HAS_ERROR);
    IOCTL_R(fd, "FIFO_EMPTY",  DEEPLINK_IOCTL_FIFO_EMPTY);
    IOCTL_R(fd, "FIFO_FULL",   DEEPLINK_IOCTL_FIFO_FULL);
    printf("\n");

    /* ---- Step 3: 通过 ioctl ENABLE ---- */
    printf("[Step 3] ioctl ENABLE 模块:\n");
    IOCTL_V(fd, "ENABLE", DEEPLINK_IOCTL_ENABLE);
    dump_regs(fd);
    IOCTL_R(fd, "IS_ENABLED", DEEPLINK_IOCTL_IS_ENABLED);
    printf("\n");

    /* ---- Step 4: 通过 pwrite 直接写 CTRL_REG 关闭模块 ---- */
    printf("[Step 4] pwrite CTRL（直接写寄存器方式）把 MODULE_EN 清 0:\n");
    /* 先读再写，保留其它位 */
    uint8_t tmp[4];
    pread(fd, tmp, 4, 0);
    uint32_t ctrl;
    memcpy(&ctrl, tmp, 4);
    ctrl &= ~0x1u;   /* 清 MODULE_EN bit 0 */
    write_ctrl(fd, ctrl);
    dump_regs(fd);
    IOCTL_R(fd, "IS_ENABLED", DEEPLINK_IOCTL_IS_ENABLED);
    printf("\n");

    /* ---- Step 5: ioctl SOFT_RESET ---- */
    printf("[Step 5] ioctl 软复位:\n");
    IOCTL_V(fd, "SOFT_RESET", DEEPLINK_IOCTL_SOFT_RESET);
    dump_regs(fd);
    printf("\n");

    /* ---- Step 6: 验证状态名映射 ---- */
    {
        uint32_t state_val = 99u;
        if (ioctl(fd, DEEPLINK_IOCTL_GET_STATE, &state_val) < 0) {
            printf("[Step 6] GET_STATE ioctl failed: %s\n", strerror(errno));
        } else {
            printf("[Step 6] GET_STATE 状态名: %s (val=%u)\n",
                   state_name(state_val), state_val);
        }
    }

    close(fd);
    printf("\nAll tests done.\n");
    return 0;
}
