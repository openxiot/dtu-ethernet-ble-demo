/*********************************************************************
 * File Name          : ota_app.c
 *
 * Description        : App 侧 OTA 主逻辑。
 *
 * 网络升级：WS 通知 → 关闭 WS/TLS → 单 socket 连固件服务器 →
 *          HTTPS GET 写 Staging → CRC32/SHA-256 + RSA 验签 →
 *          写 Meta STAGED → 复位 → Bootloader 搬运。
 *******************************************************************************/
#include "ota_app.h"
#include "ota_config.h"
#include "ota_layout.h"
#include "ota_meta.h"
#include "ota_pkg.h"
#include "ota_url.h"
#include "ota_https.h"
#include "ota_crc.h"
#include "ota_pubkey.h"
#include "transport_eth.h"
#include "ch32v20x.h"

#include "mbedtls/pk.h"
#include "mbedtls/rsa.h"

#include <string.h>
#include <stdio.h>

#define NOTIFY_MAGIC0   0x44u
#define NOTIFY_MAGIC1   0x54u
#define NOTIFY_MAGIC2   0x55u
#define NOTIFY_MAGIC3   0x4Fu
#define NOTIFY_VERSION  1u
#define NOTIFY_TYPE_OTA 0x10u

enum { OA_IDLE = 0, OA_PENDING, OA_DOWNLOADING };

static uint8_t   s_state;
static int       s_err;
static int       s_report_failure;
static uint32_t  s_boot_ms;
static int       s_confirmed;
static ota_url_t s_url;
static uint32_t  s_file_size;
static uint32_t  s_version;
static uint16_t  s_flags;

static int elapsed(uint32_t start, uint32_t ms)
{
    return (uint32_t)((uint32_t)g_sys_ms - start) >= ms;
}

static uint16_t get_u16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* ==================== 启动确认 ==================== */
static void confirm_running(void)
{
    ota_meta_t m;

    ota_meta_load(&m);
    m.state = OTA_META_APP_VALID;
    m.attempts = 0;
    m.image_version = OTA_FW_VERSION;     /* 以实际运行的版本为准 */
    ota_meta_save(&m);

    s_confirmed = 1;
    printf("[ota] boot confirmed, fw=%lu\r\n",
           (unsigned long)OTA_FW_VERSION);
}

/* ==================== 验签 ==================== */
static int verify_signature(const ota_pkg_header_t *ph)
{
    const uint8_t *base = (const uint8_t *)OTA_FLASH_PTR(OTA_STAGING_BASE);
    uint8_t digest[32];
    mbedtls_pk_context pk;
    int ret;

    ota_pkg_signature_digest(ph, base + OTA_PKG_HEADER_SIZE,
                             ph->image_size, digest);

    mbedtls_pk_init(&pk);
    ret = mbedtls_pk_parse_public_key(&pk, ota_pubkey_der, OTA_PUBKEY_DER_LEN);
    if (ret == 0) {
        mbedtls_rsa_context *rsa = mbedtls_pk_rsa(pk);
        ret = mbedtls_rsa_pkcs1_verify(rsa, NULL, NULL, MBEDTLS_RSA_PUBLIC,
                                       MBEDTLS_MD_SHA256, 32, digest,
                                       ph->signature);
    }
    mbedtls_pk_free(&pk);

    if (ret != 0) {
        printf("[ota] signature fail %d\r\n", ret);
        return -1;
    }
    return 0;
}

static int stage_verified_package(void)
{
    ota_pkg_header_t ph;
    ota_meta_t m;
    int rc;

    rc = ota_pkg_verify_staging(&ph);
    if (rc != OTA_ST_OK) return rc;

    if (ph.file_size != s_file_size || ph.image_version != s_version)
        return OTA_ST_BAD_PACKAGE;
    if (ota_https_crc32() != ota_crc32(
            (const void *)OTA_FLASH_PTR(OTA_STAGING_BASE), s_file_size))
        return OTA_ST_CRC_MISMATCH;

    rc = ota_pkg_check(&ph, ota_app_version(), OTA_HW_ID, 1);
    if (rc != OTA_ST_OK) return rc;

    if (verify_signature(&ph) != 0)
        return OTA_ST_BAD_SIGNATURE;

    ota_meta_load(&m);
    m.state = OTA_META_STAGED;
    m.flags = OTA_META_FLAG_VERIFIED;
    if (s_flags & 0x0001u) m.flags |= OTA_META_FLAG_ALLOW_DOWNGRADE;
    m.image_size = ph.image_size;
    m.image_crc32 = ph.image_crc32;
    m.image_version = ph.image_version;
    m.hw_id = ph.hw_id;
    m.source = OTA_META_SOURCE_NETWORK;
    memcpy(m.image_sha256, ph.image_sha256, 32);
    if (ota_meta_save(&m) != 0)
        return OTA_ST_FLASH_ERR;

    printf("[ota] staged ok, ver=%lu size=%lu, reboot\r\n",
           (unsigned long)ph.image_version, (unsigned long)ph.image_size);
    return OTA_ST_OK;
}

static void finish_fail(int err)
{
    s_err = err;
    s_report_failure = 1;
    s_state = OA_IDLE;
    ota_https_abort();
    transport_eth_ota_stop();
    printf("[ota] failed, code=%d\r\n", err);
}

/* ==================== 对外接口 ==================== */
void ota_app_init(void)
{
    ota_meta_t m;

    ota_meta_load(&m);
    s_boot_ms = (uint32_t)g_sys_ms;
    s_confirmed = 0;
    s_state = OA_IDLE;
    s_err = OTA_ST_OK;
    s_report_failure = 0;
}

int ota_app_on_notify(const uint8_t *data, uint32_t len)
{
    uint32_t url_len;
    uint16_t crc_rx, crc_calc;
    int rc;

    if (s_state != OA_IDLE) return OTA_ST_BUSY;
    if (!data || len < 20 || len > 240) return OTA_ST_BAD_FRAME;

    if (data[0] != NOTIFY_MAGIC0 || data[1] != NOTIFY_MAGIC1 ||
        data[2] != NOTIFY_MAGIC2 || data[3] != NOTIFY_MAGIC3)
        return OTA_ST_BAD_MAGIC;
    if (data[4] != NOTIFY_VERSION || data[5] != NOTIFY_TYPE_OTA)
        return OTA_ST_BAD_PACKAGE;

    url_len = get_u16(&data[16]);
    if (url_len == 0 || url_len > OTA_URL_MAX_LEN || 20u + url_len != len)
        return OTA_ST_BAD_SIZE;

    crc_rx = get_u16(&data[18 + url_len]);
    crc_calc = ota_crc16(0xFFFFu, &data[4], 14u + url_len);
    if (crc_rx != crc_calc) return OTA_ST_BAD_FRAME;

    s_flags = get_u16(&data[6]);
    s_file_size = get_u32(&data[8]);
    s_version = get_u32(&data[12]);

    if (s_file_size < OTA_PKG_HEADER_SIZE + OTA_FLASH_PAGE_SIZE ||
        s_file_size > OTA_PKG_FILE_MAX ||
        ((s_file_size - OTA_PKG_HEADER_SIZE) % OTA_FLASH_PAGE_SIZE))
        return OTA_ST_BAD_SIZE;

    if (!(s_flags & 0x0001u) && s_version <= ota_app_version())
        return OTA_ST_VERSION_REJECTED;

    rc = ota_url_parse((const char *)&data[18], url_len, &s_url);
    if (rc != OTA_ST_OK) return rc;

    s_err = OTA_ST_OK;
    s_state = OA_PENDING;
    printf("[ota] notify: fw=%lu size=%lu url=https://%s:%u%s\r\n",
           (unsigned long)s_version, (unsigned long)s_file_size,
           s_url.host, (unsigned)s_url.port, s_url.path);
    return OTA_ST_OK;
}

int ota_app_pending(void)
{
    return s_state == OA_PENDING;
}

int ota_app_busy(void)
{
    return s_state == OA_DOWNLOADING;
}

void ota_app_begin(void)
{
    if (s_state != OA_PENDING) return;

    if (transport_eth_ota_start(s_url.host, s_url.port) != 0) {
        finish_fail(OTA_ST_BAD_PACKAGE);
        return;
    }
    if (ota_https_start(&s_url, s_file_size) != OTA_ST_OK) {
        finish_fail(OTA_ST_BAD_PACKAGE);
        return;
    }
    s_state = OA_DOWNLOADING;
}

int ota_app_poll(void)
{
    int r;

    if (!s_confirmed && elapsed(s_boot_ms, OTA_APP_CONFIRM_MS))
        confirm_running();

    if (s_state != OA_DOWNLOADING) return 0;

    r = ota_https_poll();
    if (r == 0) return 1;

    if (r < 0) {
        finish_fail(r);
        return 0;
    }

    /* 下载完成：先释放 TLS heap，再校验整包 + 验签 + 写 STAGED */
    ota_https_abort();
    r = stage_verified_package();
    if (r != OTA_ST_OK) {
        finish_fail(r);
        return 0;
    }

    ota_https_abort();
    transport_eth_ota_stop();
    printf("[ota] reboot to apply\r\n");
    NVIC_SystemReset();
    return 1;
}

int ota_app_request_serial(void)
{
    ota_meta_t m;

    ota_meta_load(&m);
    m.state = OTA_META_BOOT_UART;
    if (ota_meta_save(&m) != 0) return -1;
    printf("[ota] reboot to serial IAP\r\n");
    NVIC_SystemReset();
    return 0;
}

uint32_t ota_app_version(void)
{
    return OTA_FW_VERSION;
}

int ota_app_report_pending(void)
{
    ota_meta_t m;

    ota_meta_load(&m);
    return (m.flags & OTA_META_FLAG_REPORT_PENDING) ? 1 : 0;
}

void ota_app_report_done(void)
{
    ota_meta_t m;

    ota_meta_load(&m);
    if (m.flags & OTA_META_FLAG_REPORT_PENDING) {
        m.flags &= (uint16_t)~OTA_META_FLAG_REPORT_PENDING;
        ota_meta_save(&m);
    }
}

int ota_app_last_error(void)
{
    return s_err;
}

int ota_app_take_failure(void)
{
    int e;

    if (!s_report_failure) return -1;
    s_report_failure = 0;
    e = s_err;
    return e;
}
