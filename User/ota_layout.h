/*********************************************************************
 * File Name          : ota_layout.h
 *
 * Description        : Flash 分区、固件包与 OTA 元数据的公共定义。
 *                      App 与 Bootloader 共用；改分区时必须同时改
 *                      Ld/Link.ld 与 Ld/Link_boot.ld。
 *
 * 冻结布局（v0.4，480 KB）：
 *   0x00000 - 0x02FFF  Bootloader   12 KB
 *   0x03000 - 0x27FFF  App         148 KB   （0x28000 为零等待区终点）
 *   0x28000 - 0x29FFF  OTA Meta      8 KB   （两个 4 KB 页轮换）
 *   0x2A000 - 0x51FFF  Staging     160 KB
 *   0x52000 - 0x77FFF  Backup      152 KB
 *******************************************************************************/
#ifndef __OTA_LAYOUT_H__
#define __OTA_LAYOUT_H__

#include <stdint.h>

/* 0x0 别名用于取指/取数；Flash 控制器只接受 0x08000000 基址。 */
#define OTA_FLASH_ALIAS         0x08000000u
#define OTA_FLASH_PTR(addr)     ((volatile const void *)(OTA_FLASH_ALIAS + (addr)))
#define OTA_FLASH_ADDR(addr)    (OTA_FLASH_ALIAS + (addr))

#define OTA_BOOT_BASE           0x00000000u
#define OTA_BOOT_SIZE           (12u * 1024u)

#define OTA_APP_BASE            0x00003000u
#define OTA_APP_SIZE            (148u * 1024u)

#define OTA_META_BASE           0x00028000u
#define OTA_META_SIZE           (8u * 1024u)
#define OTA_META_PAGE_SIZE      (4u * 1024u)
#define OTA_META_RECORD_SIZE    256u
#define OTA_META_PAGES          (OTA_META_SIZE / OTA_META_PAGE_SIZE)

#define OTA_STAGING_BASE        0x0002A000u
#define OTA_STAGING_SIZE        (160u * 1024u)

#define OTA_BACKUP_BASE         0x00052000u
#define OTA_BACKUP_SIZE         (152u * 1024u)

#define OTA_TOTAL_SIZE          0x00078000u

/* 固件包：512 B 包头 + 镜像（256 B 对齐，尾部补 0xFF）。 */
#define OTA_PKG_HEADER_SIZE     512u
#define OTA_PKG_IMAGE_MAX       OTA_APP_SIZE
#define OTA_PKG_FILE_MAX        (OTA_PKG_HEADER_SIZE + OTA_PKG_IMAGE_MAX)
#define OTA_FLASH_PAGE_SIZE     256u

/* 镜像版本：major << 16 | minor << 8 | patch。 */
#define OTA_MAKE_VERSION(maj, min, pat) \
    ((((uint32_t)(maj)) << 16) | (((uint32_t)(min)) << 8) | ((uint32_t)(pat)))

/* 串口 IAP 帧 */
#define OTA_UART_SYNC0          0xA5u
#define OTA_UART_SYNC1          0x5Au
#define OTA_UART_VERSION        0x01u
#define OTA_UART_MAX_DATA       256u
#define OTA_UART_FRAME_MAX      (10u + OTA_UART_MAX_DATA + 2u)

/* 串口消息类型 */
enum {
    OTA_MSG_HELLO       = 0x01,
    OTA_MSG_HELLO_ACK   = 0x81,
    OTA_MSG_BEGIN       = 0x02,
    OTA_MSG_BEGIN_ACK   = 0x82,
    OTA_MSG_DATA        = 0x03,
    OTA_MSG_DATA_ACK    = 0x83,
    OTA_MSG_END         = 0x04,
    OTA_MSG_END_ACK     = 0x84,
    OTA_MSG_ABORT       = 0x05,
    OTA_MSG_RESET       = 0x06,
};

/* 串口状态码 */
enum {
    OTA_ST_OK           = 0,
    OTA_ST_BUSY         = 1,
    OTA_ST_BAD_FRAME    = 2,
    OTA_ST_BAD_STATE    = 3,
    OTA_ST_NO_SPACE     = 4,
    OTA_ST_BAD_SIZE     = 5,
    OTA_ST_FLASH_ERR    = 6,
    OTA_ST_CRC_MISMATCH = 7,
    OTA_ST_BAD_SIGNATURE= 8,
    OTA_ST_VERSION_REJECTED = 9,
    OTA_ST_HW_MISMATCH  = 10,
    OTA_ST_BAD_PACKAGE  = 11,
    OTA_ST_BAD_MAGIC    = 12,
};

#endif /* __OTA_LAYOUT_H__ */
