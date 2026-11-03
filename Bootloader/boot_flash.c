/*********************************************************************
 * File Name          : boot_flash.c
 *
 * Description        : 256 B 页擦写；搬运时逐页读取 Staging 并写入 App。
 *******************************************************************************/
#include "boot_flash.h"
#include "ota_crc.h"
#include "ota_layout.h"
#include "ch32v20x_flash.h"
#include <string.h>

static uint32_t s_page[OTA_FLASH_PAGE_SIZE / 4] __attribute__((aligned(4)));

int boot_flash_erase(uint32_t addr, uint32_t len)
{
    if ((addr % OTA_FLASH_PAGE_SIZE) || (len % OTA_FLASH_PAGE_SIZE) || !len)
        return -1;
    if (addr + len > OTA_TOTAL_SIZE)
        return -1;
    return FLASH_ROM_ERASE(OTA_FLASH_ADDR(addr), len) == FLASH_COMPLETE ? 0 : -1;
}

int boot_flash_write(uint32_t addr, const uint8_t *data, uint32_t len)
{
    uint32_t off;

    if ((addr % OTA_FLASH_PAGE_SIZE) || (len % OTA_FLASH_PAGE_SIZE) || !len)
        return -1;
    if (addr + len > OTA_TOTAL_SIZE)
        return -1;

    for (off = 0; off < len; off += OTA_FLASH_PAGE_SIZE) {
        uint32_t page_addr = OTA_FLASH_ADDR(addr + off);

        memcpy(s_page, data + off, OTA_FLASH_PAGE_SIZE);
        if (FLASH_ROM_ERASE(page_addr, OTA_FLASH_PAGE_SIZE) != FLASH_COMPLETE)
            return -1;
        if (FLASH_ROM_WRITE(page_addr, s_page, OTA_FLASH_PAGE_SIZE) != FLASH_COMPLETE)
            return -1;
        if (memcmp(OTA_FLASH_PTR(addr + off), s_page, OTA_FLASH_PAGE_SIZE) != 0)
            return -1;
    }
    return 0;
}

int boot_copy_staging_to_app(uint32_t image_size, uint32_t *crc_out)
{
    uint32_t off;
    uint32_t crc;

    if (!image_size || image_size > OTA_APP_SIZE ||
        (image_size % OTA_FLASH_PAGE_SIZE))
        return -1;

    ota_crc32_init(&crc);
    for (off = 0; off < image_size; off += OTA_FLASH_PAGE_SIZE) {
        const uint8_t *src =
            (const uint8_t *)OTA_FLASH_PTR(OTA_STAGING_BASE + OTA_PKG_HEADER_SIZE + off);

        ota_crc32_update(&crc, src, OTA_FLASH_PAGE_SIZE);
        if (boot_flash_write(OTA_APP_BASE + off, src, OTA_FLASH_PAGE_SIZE) != 0)
            return -1;
    }

    if (crc_out) *crc_out = ota_crc32_final(crc);
    return 0;
}

int boot_flash_verify(uint32_t addr, uint32_t len, uint32_t expect_crc)
{
    uint32_t crc;

    ota_crc32_init(&crc);
    ota_crc32_update(&crc, (const void *)OTA_FLASH_PTR(addr), len);
    return ota_crc32_final(crc) == expect_crc ? 0 : -1;
}
