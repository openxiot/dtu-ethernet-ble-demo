/*********************************************************************
 * File Name          : ota_flash.c
 *
 * Description        : App 侧 Flash 擦写实现。与 Bootloader 的 boot_flash 分开，
 *                      因为 App 只允许写 Meta 之后的区域。
 *******************************************************************************/
#include "ota_flash.h"
#include "ota_layout.h"
#include "ch32v20x_flash.h"
#include <string.h>

static uint32_t s_page[OTA_FLASH_PAGE_SIZE / 4] __attribute__((aligned(4)));

int ota_flash_range_ok(uint32_t addr, uint32_t len)
{
    if (!len) return 0;
    if (addr % OTA_FLASH_PAGE_SIZE) return 0;
    if (len % OTA_FLASH_PAGE_SIZE) return 0;
    if (addr < OTA_META_BASE) return 0;
    if (addr + len > OTA_TOTAL_SIZE) return 0;
    if (addr + len < addr) return 0;
    return 1;
}

int ota_flash_erase(uint32_t addr, uint32_t len)
{
    if (!ota_flash_range_ok(addr, len)) return -1;
    return FLASH_ROM_ERASE(OTA_FLASH_ADDR(addr), len) == FLASH_COMPLETE ? 0 : -1;
}

int ota_flash_write(uint32_t addr, const uint8_t *data, uint32_t len)
{
    uint32_t off;

    if (!ota_flash_range_ok(addr, len)) return -1;

    for (off = 0; off < len; off += OTA_FLASH_PAGE_SIZE) {
        uint32_t page_addr = OTA_FLASH_ADDR(addr + off);

        memcpy(s_page, data + off, OTA_FLASH_PAGE_SIZE);
        if (FLASH_ROM_ERASE(page_addr, OTA_FLASH_PAGE_SIZE) != FLASH_COMPLETE)
            return -1;
        if (FLASH_ROM_WRITE(page_addr, s_page, OTA_FLASH_PAGE_SIZE) != FLASH_COMPLETE)
            return -1;
        if (memcmp((const void *)OTA_FLASH_PTR(addr + off),
                   s_page, OTA_FLASH_PAGE_SIZE) != 0)
            return -1;
    }
    return 0;
}
