/*********************************************************************
 * File Name          : ota_pkg.h
 *
 * Description        : .dtufw 固件包（512 B 包头 + 镜像）的解析与校验。
 *                      Bootloader 与 App 共用；不依赖 mbedTLS。
 *                      RSA 验签在 App 的 ota_app.c 中完成。
 *******************************************************************************/
#ifndef __OTA_PKG_H__
#define __OTA_PKG_H__

#include <stdint.h>
#include "ota_layout.h"

#define OTA_PKG_MAGIC0  0x44u   /* 'D' */
#define OTA_PKG_MAGIC1  0x54u   /* 'T' */
#define OTA_PKG_MAGIC2  0x55u   /* 'U' */
#define OTA_PKG_MAGIC3  0x46u   /* 'F' */
#define OTA_PKG_MAGIC4  0x57u   /* 'W' */
#define OTA_PKG_FORMAT  0x01u

typedef struct {
    uint8_t  magic[8];          /* 0x000 "DTUFW" + 01 00 00 */
    uint16_t header_size;       /* 0x008 */
    uint16_t flags;             /* 0x00A bit0=允许降级 */
    uint32_t file_size;         /* 0x00C */
    uint32_t image_size;        /* 0x010，256 B 对齐 */
    uint32_t image_crc32;       /* 0x014 */
    uint32_t image_version;     /* 0x018 */
    uint32_t hw_id;             /* 0x01C */
    uint8_t  image_sha256[32];  /* 0x020 */
    uint8_t  signature[256];    /* 0x040，RSA-2048 PKCS#1 v1.5 */
    uint8_t  reserved[192];     /* 0x140，0xFF */
} ota_pkg_header_t;

typedef char ota_pkg_header_size_check[(sizeof(ota_pkg_header_t) == OTA_PKG_HEADER_SIZE) ? 1 : -1];

/* 解析并做静态字段检查。返回 OTA_ST_*。 */
int ota_pkg_parse(const uint8_t raw[OTA_PKG_HEADER_SIZE], ota_pkg_header_t *out);

/* 版本 / 机型 / 尺寸检查；current_version 为 0 表示当前没有有效 App。 */
int ota_pkg_check(const ota_pkg_header_t *h, uint32_t current_version, uint32_t hw_id,
                  int have_current);

/* 校验 Staging 中的包头与镜像（CRC32 + SHA-256，不含 RSA）。
 * 成功时 out_header 可为 NULL。返回 OTA_ST_*。 */
int ota_pkg_verify_staging(ota_pkg_header_t *out_header);

/* 计算签名对象摘要：SHA-256(header[0x000..0x040) || image)。 */
void ota_pkg_signature_digest(const ota_pkg_header_t *h,
                              const uint8_t *image, uint32_t image_size,
                              uint8_t out[32]);

#endif /* __OTA_PKG_H__ */
