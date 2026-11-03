/*********************************************************************
 * File Name          : ota_pkg.c
 *
 * Description        : .dtufw 包头解析、字段检查与 Staging 校验。
 *******************************************************************************/
#include "ota_pkg.h"
#include "ota_crc.h"
#include "ota_sha256.h"
#include <string.h>

static const uint8_t pkg_magic[8] = {
    OTA_PKG_MAGIC0, OTA_PKG_MAGIC1, OTA_PKG_MAGIC2, OTA_PKG_MAGIC3,
    OTA_PKG_MAGIC4, OTA_PKG_FORMAT, 0x00, 0x00
};

int ota_pkg_parse(const uint8_t raw[OTA_PKG_HEADER_SIZE], ota_pkg_header_t *out)
{
    ota_pkg_header_t h;

    memcpy(&h, raw, sizeof(h));

    if (memcmp(h.magic, pkg_magic, sizeof(pkg_magic)) != 0)
        return OTA_ST_BAD_MAGIC;
    if (h.header_size != OTA_PKG_HEADER_SIZE)
        return OTA_ST_BAD_PACKAGE;
    if (h.image_size == 0 || h.image_size > OTA_PKG_IMAGE_MAX)
        return OTA_ST_NO_SPACE;
    if (h.image_size % OTA_FLASH_PAGE_SIZE)
        return OTA_ST_BAD_SIZE;
    if (h.file_size != OTA_PKG_HEADER_SIZE + h.image_size)
        return OTA_ST_BAD_SIZE;

    if (out) *out = h;
    return OTA_ST_OK;
}

int ota_pkg_check(const ota_pkg_header_t *h, uint32_t current_version, uint32_t hw_id,
                  int have_current)
{
    if (!h) return OTA_ST_BAD_PACKAGE;

    if (h->hw_id != hw_id)
        return OTA_ST_HW_MISMATCH;

    if (have_current && !(h->flags & 1u)) {
        if (h->image_version <= current_version)
            return OTA_ST_VERSION_REJECTED;
    }
    return OTA_ST_OK;
}

int ota_pkg_verify_staging(ota_pkg_header_t *out_header)
{
    const uint8_t *base = (const uint8_t *)OTA_FLASH_PTR(OTA_STAGING_BASE);
    ota_pkg_header_t h;
    ota_sha256_ctx sha;
    uint32_t crc;
    uint32_t off;
    uint8_t digest[32];
    int rc;

    rc = ota_pkg_parse(base, &h);
    if (rc != OTA_ST_OK) return rc;

    ota_crc32_init(&crc);
    ota_sha256_init(&sha);

    for (off = 0; off < h.image_size; off += OTA_FLASH_PAGE_SIZE) {
        const uint8_t *p = base + OTA_PKG_HEADER_SIZE + off;
        ota_crc32_update(&crc, p, OTA_FLASH_PAGE_SIZE);
        ota_sha256_update(&sha, p, OTA_FLASH_PAGE_SIZE);
    }
    ota_sha256_final(&sha, digest);

    if (ota_crc32_final(crc) != h.image_crc32)
        return OTA_ST_CRC_MISMATCH;
    if (memcmp(digest, h.image_sha256, sizeof(digest)) != 0)
        return OTA_ST_CRC_MISMATCH;

    if (out_header) *out_header = h;
    return OTA_ST_OK;
}

void ota_pkg_signature_digest(const ota_pkg_header_t *h,
                              const uint8_t *image, uint32_t image_size,
                              uint8_t out[32])
{
    ota_sha256_ctx sha;

    ota_sha256_init(&sha);
    ota_sha256_update(&sha, h, 0x40);          /* 只覆盖 magic..image_sha256 */
    ota_sha256_update(&sha, image, image_size);
    ota_sha256_final(&sha, out);
}
