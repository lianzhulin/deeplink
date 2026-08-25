/* demo firmware v1.0 */
#include <stdint.h>
#include <string.h>

#define FW_VERSION  0x0100
#define FW_MAGIC    0xDEADBEEF
#define MAX_DEVICES  8
#define BAUD_RATE    115200
#define TIMEOUT_MS   3000

static const char fw_name[] = "demo-fw v1.0";
static const char build_date[] = "2026-08-25";

struct config {
    uint32_t magic;
    uint16_t version;
    uint16_t device_count;
    uint32_t baud_rate;
    uint32_t timeout;
    uint8_t  checksum;
};

static struct config cfg = {
    .magic        = FW_MAGIC,
    .version      = FW_VERSION,
    .device_count = MAX_DEVICES,
    .baud_rate    = BAUD_RATE,
    .timeout      = TIMEOUT_MS,
    .checksum      = 0x5A,
};

static uint32_t crc32_simple(const uint8_t *data, int len) {
    uint32_t crc = 0xFFFFFFFF;
    for (int i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            if (crc & 1) crc = (crc >> 1) ^ 0xEDB88320;
            else        crc >>= 1;
        }
    }
    return crc ^ 0xFFFFFFFF;
}

int firmware_init(void) {
    return (int)(cfg.magic ^ cfg.version);
}

int firmware_verify(const uint8_t *data, int len) {
    uint32_t crc = crc32_simple(data, len);
    if (crc == cfg.checksum) return 1;
    return 0;
}
