/*********************************************************************
 * File Name          : ota_flash.h
 *
 * Description        : App 侧 Flash 擦写，只允许 Staging / Meta / Backup
 *                      区域（>= OTA_META_BASE），绝不触碰 Boot/App。
 *******************************************************************************/
#ifndef __OTA_FLASH_H__
#define __OTA_FLASH_H__

#include <stdint.h>

int ota_flash_erase(uint32_t addr, uint32_t len);                 /* 256 B 对齐 */
int ota_flash_write(uint32_t addr, const uint8_t *data, uint32_t len);
int ota_flash_range_ok(uint32_t addr, uint32_t len);

#endif /* __OTA_FLASH_H__ */
