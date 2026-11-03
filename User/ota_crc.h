/*********************************************************************
 * File Name          : ota_crc.h
 *
 * Description        : OTA 使用的 CRC-32/IEEE 与 CRC-16/CCITT-FALSE。
 *                      Bootloader 不能依赖 mbedTLS，因此使用软件实现。
 *******************************************************************************/
#ifndef __OTA_CRC_H__
#define __OTA_CRC_H__

#include <stdint.h>

/* CRC-32/IEEE：poly 0xEDB88320，init 0xFFFFFFFF，xorout 0xFFFFFFFF。 */
void     ota_crc32_init(uint32_t *crc);
void     ota_crc32_update(uint32_t *crc, const void *data, uint32_t len);
uint32_t ota_crc32_final(uint32_t crc);
uint32_t ota_crc32(const void *data, uint32_t len);

/* CRC-16/CCITT-FALSE：poly 0x1021，init 0xFFFF，无 xorout。 */
uint16_t ota_crc16(uint16_t crc, const void *data, uint32_t len);

#endif /* __OTA_CRC_H__ */
