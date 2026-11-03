/*********************************************************************
 * File Name          : ota_sha256.h
 *
 * Description        : 精简 SHA-256（FIPS 180-4）。
 *                      Bootloader 不链接 mbedTLS，因此 App / Bootloader 共用
 *                      这一份实现；约 1.5 KB Flash。
 *******************************************************************************/
#ifndef __OTA_SHA256_H__
#define __OTA_SHA256_H__

#include <stdint.h>

typedef struct {
    uint32_t h[8];
    uint64_t total;
    uint8_t  buf[64];
    uint32_t used;
} ota_sha256_ctx;

void ota_sha256_init(ota_sha256_ctx *ctx);
void ota_sha256_update(ota_sha256_ctx *ctx, const void *data, uint32_t len);
void ota_sha256_final(ota_sha256_ctx *ctx, uint8_t out[32]);
void ota_sha256(const void *data, uint32_t len, uint8_t out[32]);

#endif /* __OTA_SHA256_H__ */
