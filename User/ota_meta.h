/*********************************************************************
 * File Name          : ota_meta.h
 *
 * Description        : OTA 元数据，双 4 KB 页轮换保存。
 *
 * 记录固定 256 B，A/B 两页（0x28000 / 0x29000）交替写：
 *   - load 扫描两页，取 magic/CRC 有效且 seq 最大者；
 *   - save 先擦另一页，再写新记录（seq+1），最后回读校验；
 *   - 任意一次掉电最多丢最新一条，旧记录仍在另一页。
 *******************************************************************************/
#ifndef __OTA_META_H__
#define __OTA_META_H__

#include <stdint.h>
#include "ota_layout.h"

#define OTA_META_MAGIC          0x4D41544Fu   /* "OTAM"（小端字节 4F 54 41 4D） */

typedef enum {
    OTA_META_EMPTY = 0,
    OTA_META_APP_VALID = 1,
    OTA_META_STAGED = 2,
    OTA_META_COPYING = 3,
    OTA_META_BOOT_UART = 4,
    OTA_META_RECOVERY = 5,
} ota_meta_state_t;

#define OTA_META_FLAG_ALLOW_DOWNGRADE   (1u << 0)
#define OTA_META_FLAG_VERIFIED          (1u << 1)
#define OTA_META_FLAG_REPORT_PENDING    (1u << 2)

#define OTA_META_SOURCE_NONE            0u
#define OTA_META_SOURCE_NETWORK         1u
#define OTA_META_SOURCE_SERIAL          2u

typedef struct {
    uint32_t magic;             /* 0x00 */
    uint32_t seq;               /* 0x04 */
    uint8_t  state;             /* 0x08 */
    uint8_t  attempts;          /* 0x09 */
    uint16_t flags;             /* 0x0A */
    uint32_t image_size;        /* 0x0C */
    uint32_t image_crc32;       /* 0x10 */
    uint32_t image_version;     /* 0x14 */
    uint32_t hw_id;             /* 0x18 */
    uint32_t source;            /* 0x1C */
    uint8_t  image_sha256[32];  /* 0x20 */
    uint32_t record_crc32;      /* 0x40 */
    uint8_t  reserved[188];     /* 0x44..0xFF */
} ota_meta_t;

/* 读当前有效记录；无有效记录时返回 EMPTY 记录（不访问 Flash 以外的资源）。 */
void ota_meta_load(ota_meta_t *out);

/* 写入一条新记录（自动 seq+1、计算 CRC、写另一页并回读校验）。
 * 成功返回 0，Flash 错误返回 -1。 */
int ota_meta_save(ota_meta_t *m);

/* 便捷接口 */
uint8_t  ota_meta_state(void);
uint32_t ota_meta_version(void);

#endif /* __OTA_META_H__ */
