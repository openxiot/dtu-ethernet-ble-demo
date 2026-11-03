/*********************************************************************
 * File Name          : boot_iap.c
 *
 * Description        : 串口 IAP：HELLO/BEGIN/DATA/END/ABORT/RESET。
 *                      帧格式见 ota_layout.h 与设计文档 8.3。
 *******************************************************************************/
#include "boot_iap.h"
#include "boot_uart.h"
#include "boot_flash.h"
#include "boot_main.h"
#include "ota_layout.h"
#include "ota_config.h"
#include "ota_meta.h"
#include "ota_pkg.h"
#include "ota_crc.h"
#include "ota_sha256.h"
#include <string.h>

#define HDR_LEN 10u

typedef struct {
    uint8_t  ver;
    uint8_t  type;
    uint16_t flags;
    uint32_t offset;
    uint16_t len;
} frame_hdr_t;

static uint8_t s_hdr[HDR_LEN];
static uint8_t s_data[OTA_UART_MAX_DATA];
static uint8_t s_pkg_hdr[OTA_PKG_HEADER_SIZE];

static void put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
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

static void send_frame(uint8_t type, uint16_t status,
                       uint32_t offset, const uint8_t *data, uint16_t len)
{
    uint8_t buf[12 + OTA_UART_MAX_DATA + 2];
    uint16_t crc;
    uint32_t n = 0;

    buf[n++] = OTA_UART_SYNC0;
    buf[n++] = OTA_UART_SYNC1;
    buf[n++] = OTA_UART_VERSION;
    buf[n++] = type;
    put_u16(&buf[n], status); n += 2;
    put_u32(&buf[n], offset); n += 4;
    put_u16(&buf[n], len);    n += 2;
    if (len) {
        memcpy(&buf[n], data, len);
        n += len;
    }
    crc = ota_crc16(0xFFFFu, &buf[2], 10u + len);
    put_u16(&buf[n], crc);
    n += 2;

    boot_uart_send(buf, n);
}

/* 返回 0 成功；-1 超时；-2 帧不完整；-3 帧头非法；-4 CRC 错误。 */
static int read_frame(frame_hdr_t *h, uint32_t timeout_ms)
{
    uint8_t b, cb0, cb1;
    uint16_t crc;
    uint8_t got_a5 = 0;
    uint32_t i;

    for (;;) {
        if (boot_uart_getc(&b, timeout_ms) != 0) return -1;
        if (!got_a5) {
            if (b == OTA_UART_SYNC0) got_a5 = 1;
        } else {
            if (b == OTA_UART_SYNC1) break;
            got_a5 = (b == OTA_UART_SYNC0);
        }
    }

    for (i = 0; i < HDR_LEN; i++)
        if (boot_uart_getc(&s_hdr[i], OTA_UART_FRAME_TIMEOUT) != 0) return -2;

    h->ver    = s_hdr[0];
    h->type   = s_hdr[1];
    h->flags  = get_u16(&s_hdr[2]);
    h->offset = get_u32(&s_hdr[4]);
    h->len    = get_u16(&s_hdr[8]);

    if (h->ver != OTA_UART_VERSION || h->len > OTA_UART_MAX_DATA)
        return -3;

    for (i = 0; i < h->len; i++)
        if (boot_uart_getc(&s_data[i], OTA_UART_FRAME_TIMEOUT) != 0) return -2;

    if (boot_uart_getc(&cb0, OTA_UART_FRAME_TIMEOUT) != 0) return -2;
    if (boot_uart_getc(&cb1, OTA_UART_FRAME_TIMEOUT) != 0) return -2;
    crc = (uint16_t)cb0 | ((uint16_t)cb1 << 8);

    {
        uint16_t calc = ota_crc16(0xFFFFu, s_hdr, HDR_LEN);
        calc = ota_crc16(calc, s_data, h->len);
        if (calc != crc) return -4;
    }
    return 0;
}

static void send_hello_ack(const ota_meta_t *m)
{
    uint8_t p[16];

    put_u32(&p[0], OTA_BL_VERSION);
    put_u32(&p[4], (m->state == OTA_META_APP_VALID) ? m->image_version : 0);
    p[8]  = m->state;
    p[9]  = (m->state == OTA_META_STAGED) ? 1 : 0;
    put_u16(&p[10], OTA_FLASH_PAGE_SIZE);
    put_u32(&p[12], OTA_HW_ID);
    send_frame(OTA_MSG_HELLO_ACK, OTA_ST_OK, 0, p, sizeof(p));
}

/* 等待 HELLO；返回 0 收到，-1 超时。 */
static int wait_hello(uint32_t wait_ms, const ota_meta_t *m)
{
    frame_hdr_t h;

    for (;;) {
        int r = read_frame(&h, wait_ms);
        if (r == -1) return -1;
        if (r == 0 && h.type == OTA_MSG_HELLO) {
            send_hello_ack(m);
            return 0;
        }
    }
}

/* 处理一次完整的串口升级会话；返回 0 表示会话结束（ABORT/RESET/超时）。 */
int boot_iap_run(uint32_t wait_ms)
{
    frame_hdr_t h;
    ota_meta_t meta;
    ota_meta_t cur;
    int active = 0;
    uint32_t expected = 0;
    uint32_t file_size = 0;
    uint32_t file_crc_expect = 0;
    uint32_t image_crc = 0;
    uint32_t file_crc = 0;
    ota_sha256_ctx image_sha;
    ota_pkg_header_t ph;
    int header_ok = 0;

    ota_meta_load(&cur);

    if (wait_hello(wait_ms, &cur) != 0)
        return -1;

    for (;;) {
        int r = read_frame(&h, OTA_UART_FRAME_TIMEOUT);

        if (r == -1) {
            /* 会话内允许长时间空闲；只有 ABORT/RESET/END 才退出。 */
            continue;
        }
        if (r != 0) {
            if (active)
                send_frame(OTA_MSG_DATA_ACK, OTA_ST_BAD_FRAME, expected, NULL, 0);
            continue;
        }

        switch (h.type) {
        case OTA_MSG_BEGIN: {
            uint32_t ver, size, fcrc;
            uint16_t flags;

            if (h.len != 14) {
                send_frame(OTA_MSG_BEGIN_ACK, OTA_ST_BAD_FRAME, 0, NULL, 0);
                break;
            }
            size  = get_u32(&s_data[0]);
            fcrc  = get_u32(&s_data[4]);
            ver   = get_u32(&s_data[8]);
            flags = get_u16(&s_data[12]);

            if (!size || size > OTA_PKG_FILE_MAX ||
                (size % OTA_FLASH_PAGE_SIZE)) {
                send_frame(OTA_MSG_BEGIN_ACK, OTA_ST_BAD_SIZE, 0, NULL, 0);
                break;
            }
            if ((cur.state == OTA_META_APP_VALID || cur.state == OTA_META_BOOT_UART) &&
                !(flags & OTA_META_FLAG_ALLOW_DOWNGRADE) &&
                ver <= cur.image_version) {
                send_frame(OTA_MSG_BEGIN_ACK, OTA_ST_VERSION_REJECTED, 0, NULL, 0);
                break;
            }

            file_size = size;
            file_crc_expect = fcrc;
            expected = 0;
            image_crc = 0;
            file_crc = 0;
            header_ok = 0;
            ota_crc32_init(&image_crc);
            ota_crc32_init(&file_crc);
            ota_sha256_init(&image_sha);
            active = 1;

            send_frame(OTA_MSG_BEGIN_ACK, OTA_ST_OK, 0, NULL, 0);
            break;
        }

        case OTA_MSG_DATA: {
            uint16_t st = OTA_ST_OK;

            if (!active) { st = OTA_ST_BAD_STATE; }
            else if (h.offset != expected || h.len != OTA_FLASH_PAGE_SIZE) {
                st = OTA_ST_BAD_SIZE;
            } else if (h.offset + h.len > file_size) {
                st = OTA_ST_BAD_SIZE;
            } else if (boot_flash_write(OTA_STAGING_BASE + h.offset,
                                        s_data, h.len) != 0) {
                st = OTA_ST_FLASH_ERR;
            } else {
                ota_crc32_update(&file_crc, s_data, h.len);
                expected += h.len;

                if (h.offset < OTA_PKG_HEADER_SIZE) {
                    uint32_t n = OTA_PKG_HEADER_SIZE - h.offset;
                    if (n > h.len) n = h.len;
                    memcpy(&s_pkg_hdr[h.offset], s_data, n);
                } else {
                    ota_crc32_update(&image_crc, s_data, h.len);
                    ota_sha256_update(&image_sha, s_data, h.len);
                }

                if (!header_ok && expected == OTA_PKG_HEADER_SIZE) {
                    st = (uint16_t)ota_pkg_parse(s_pkg_hdr, &ph);
                    if (st == OTA_ST_OK)
                        st = (uint16_t)ota_pkg_check(&ph, cur.image_version, OTA_HW_ID,
                                                     cur.state == OTA_META_APP_VALID ||
                                                     cur.state == OTA_META_BOOT_UART);
                    if (st == OTA_ST_OK &&
                        ph.file_size != file_size)
                        st = OTA_ST_BAD_SIZE;
                    if (st == OTA_ST_OK)
                        header_ok = 1;
                }
            }

            if (st != OTA_ST_OK)
                active = 0;
            send_frame(OTA_MSG_DATA_ACK, st, expected, NULL, 0);
            break;
        }

        case OTA_MSG_END: {
            uint32_t icrc;
            uint8_t sha[32];

            if (!active || !header_ok || expected != file_size) {
                send_frame(OTA_MSG_END_ACK, OTA_ST_BAD_STATE, expected, NULL, 0);
                break;
            }

            icrc = ota_crc32_final(image_crc);
            ota_sha256_final(&image_sha, sha);

            if (ota_crc32_final(file_crc) != file_crc_expect ||
                icrc != ph.image_crc32 ||
                memcmp(sha, ph.image_sha256, sizeof(sha)) != 0) {
                active = 0;
                send_frame(OTA_MSG_END_ACK, OTA_ST_CRC_MISMATCH, expected, NULL, 0);
                break;
            }

            memset(&meta, 0, sizeof(meta));
            meta.state = OTA_META_STAGED;
            meta.flags = OTA_META_FLAG_VERIFIED;
            meta.image_size = ph.image_size;
            meta.image_crc32 = ph.image_crc32;
            meta.image_version = ph.image_version;
            meta.hw_id = ph.hw_id;
            meta.source = OTA_META_SOURCE_SERIAL;
            memcpy(meta.image_sha256, ph.image_sha256, 32);

            if (ota_meta_save(&meta) != 0) {
                active = 0;
                send_frame(OTA_MSG_END_ACK, OTA_ST_FLASH_ERR, expected, NULL, 0);
                break;
            }

            send_frame(OTA_MSG_END_ACK, OTA_ST_OK, expected, NULL, 0);
            active = 0;

            /* 搬运并校验，成功后跳 App */
            {
                uint32_t crc_out = 0;
                if (boot_copy_staging_to_app(meta.image_size, &crc_out) == 0 &&
                    crc_out == meta.image_crc32 &&
                    boot_flash_verify(OTA_APP_BASE, meta.image_size,
                                      meta.image_crc32) == 0) {
                    meta.state = OTA_META_APP_VALID;
                    meta.attempts = 0;
                    meta.flags |= OTA_META_FLAG_REPORT_PENDING;
                    ota_meta_save(&meta);
                    boot_jump_to_app();
                }
                meta.state = OTA_META_RECOVERY;
                ota_meta_save(&meta);
                return -2;
            }
        }

        case OTA_MSG_ABORT:
            active = 0;
            return 0;

        case OTA_MSG_RESET:
            return 1;

        default:
            break;
        }
    }
}
