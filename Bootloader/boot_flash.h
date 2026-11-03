/*********************************************************************
 * File Name          : boot_flash.h
 *
 * Description        : Bootloader 的 Flash 擦写与 Staging→App 搬运。
 *                      所有地址参数均为 0x0 别名偏移，内部转换成 0x08000000。
 *******************************************************************************/
#ifndef __BOOT_FLASH_H__
#define __BOOT_FLASH_H__

#include <stdint.h>

int  boot_flash_write(uint32_t addr, const uint8_t *data, uint32_t len);
int  boot_flash_erase(uint32_t addr, uint32_t len);
int  boot_copy_staging_to_app(uint32_t image_size, uint32_t *crc_out);
int  boot_flash_verify(uint32_t addr, uint32_t len, uint32_t expect_crc);

#endif /* __BOOT_FLASH_H__ */
