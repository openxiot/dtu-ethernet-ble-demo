/*********************************************************************
 * File Name          : ota_https.c
 *
 * Description        : 极简 HTTPS 客户端：GET → 状态行/头部 → body 写 Staging。
 *                      v1：只接受 200 + Content-Length，不跟随重定向，
 *                      不支持 chunked 与 Range。
 *******************************************************************************/
#include "ota_https.h"
#include "ota_layout.h"
#include "ota_flash.h"
#include "ota_crc.h"
#include "transport_eth.h"
#include "wss_tls.h"
#include "debug.h"

#include <string.h>
#include <stdio.h>

#define RESP_HDR_MAX        768u
#define BODY_READ_MAX       256u
#define LINK_TMO_MS         10000u
#define TLS_TMO_MS          15000u
#define HDR_TMO_MS          15000u
#define BODY_IDLE_TMO_MS    10000u
#define TOTAL_TMO_MS        300000u

enum {
    HS_IDLE = 0,
    HS_WAIT_LINK,
    HS_TLS,
    HS_SEND,
    HS_HEADER,
    HS_BODY,
    HS_DONE,
    HS_FAIL,
};

static ota_url_t s_url;
static uint32_t  s_file_size;
static uint32_t  s_received;
static uint32_t  s_page_off;          /* 尚未落盘的页内偏移 */
static uint32_t  s_t0;
static uint32_t  s_step_t0;
static uint8_t   s_state;
static int       s_err;
static uint32_t  s_crc;

static uint8_t s_page[OTA_FLASH_PAGE_SIZE];
static uint8_t s_hdr[RESP_HDR_MAX];
static uint32_t s_hdr_len;
static char    s_req[512];
static uint32_t s_req_len, s_req_sent;

static int elapsed(uint32_t start, uint32_t ms)
{
    return (uint32_t)((uint32_t)g_sys_ms - start) >= ms;
}

static int set_fail(int err)
{
    s_err = err;
    s_state = HS_FAIL;
    return err;
}

static int flush_page(void)
{
    if (s_page_off == 0) return 0;
    if (ota_flash_write(OTA_STAGING_BASE + s_received - s_page_off,
                        s_page, OTA_FLASH_PAGE_SIZE) != 0)
        return -1;
    s_page_off = 0;
    return 0;
}

static int feed_body(const uint8_t *p, uint32_t n)
{
    while (n) {
        uint32_t room = OTA_FLASH_PAGE_SIZE - s_page_off;
        uint32_t k = (n < room) ? n : room;

        if (s_received + k > s_file_size)
            return -1;
        memcpy(&s_page[s_page_off], p, k);
        s_page_off += k;
        s_received += k;
        ota_crc32_update(&s_crc, p, k);
        p += k;
        n -= k;

        if (s_page_off == OTA_FLASH_PAGE_SIZE) {
            if (ota_flash_write(OTA_STAGING_BASE + s_received - OTA_FLASH_PAGE_SIZE,
                                s_page, OTA_FLASH_PAGE_SIZE) != 0)
                return -1;
            s_page_off = 0;
        }
    }
    return 0;
}

/* 在响应头中查找字段（大小写不敏感）。返回值的起始指针，未找到 NULL。 */
static const char *find_header(const char *name)
{
    static char low[RESP_HDR_MAX];
    uint32_t n = s_hdr_len;
    uint32_t i;
    const char *p;

    if (n >= sizeof(low)) n = sizeof(low) - 1;
    for (i = 0; i < n; i++) {
        char c = (char)s_hdr[i];
        low[i] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
    }
    low[n] = 0;

    {
        char key[24];
        uint32_t kl = (uint32_t)strlen(name);
        if (kl + 2 >= sizeof(key)) return NULL;
        key[0] = '\n';
        for (i = 0; i < kl; i++) {
            char c = name[i];
            key[i + 1] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
        }
        key[kl + 1] = 0;
        p = strstr(low, key);
        if (!p) return NULL;
        return p + 1;   /* 指向原始大小写的位置 */
    }
}

static int parse_header(void)
{
    const char *p = (const char *)s_hdr;
    uint32_t status;
    const char *cl;
    uint32_t content_len = 0;

    if (s_hdr_len < 12 || memcmp(p, "HTTP/1.", 7) != 0)
        return OTA_ST_BAD_PACKAGE;
    if (p[8] != ' ') return OTA_ST_BAD_PACKAGE;
    status = 0;
    {
        int i;
        for (i = 9; i < 12; i++) {
            if (p[i] < '0' || p[i] > '9') return OTA_ST_BAD_PACKAGE;
            status = status * 10u + (uint32_t)(p[i] - '0');
        }
    }
    if (status != 200) {
        printf("[ota] http status %lu\r\n", (unsigned long)status);
        return OTA_ST_BAD_PACKAGE;
    }

    if (find_header("transfer-encoding"))
        return OTA_ST_BAD_PACKAGE;       /* v1 不支持 chunked */

    cl = find_header("content-length");
    if (!cl) return OTA_ST_BAD_SIZE;
    cl += strlen("content-length");
    while (*cl == ' ' || *cl == ':') cl++;
    if (*cl < '0' || *cl > '9') return OTA_ST_BAD_SIZE;
    while (*cl >= '0' && *cl <= '9') {
        content_len = content_len * 10u + (uint32_t)(*cl - '0');
        cl++;
        if (content_len > OTA_PKG_FILE_MAX) return OTA_ST_BAD_SIZE;
    }
    if (content_len != s_file_size) {
        printf("[ota] content-length %lu != %lu\r\n",
               (unsigned long)content_len, (unsigned long)s_file_size);
        return OTA_ST_BAD_SIZE;
    }
    return OTA_ST_OK;
}

static int build_request(void)
{
    int n;
    const char *host = s_url.host;
    int is_default_port = (s_url.port == 443);

    if (is_default_port)
        n = snprintf(s_req, sizeof(s_req),
                     "GET %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n"
                     "User-Agent: DTU/1.0\r\n\r\n", s_url.path, host);
    else
        n = snprintf(s_req, sizeof(s_req),
                     "GET %s HTTP/1.1\r\nHost: %s:%u\r\nConnection: close\r\n"
                     "User-Agent: DTU/1.0\r\n\r\n", s_url.path, host,
                     (unsigned)s_url.port);
    if (n <= 0 || n >= (int)sizeof(s_req)) return OTA_ST_BAD_SIZE;
    s_req_len = (uint32_t)n;
    s_req_sent = 0;
    return OTA_ST_OK;
}

int ota_https_start(const ota_url_t *url, uint32_t file_size)
{
    if (!url || file_size == 0 || file_size > OTA_PKG_FILE_MAX)
        return OTA_ST_BAD_SIZE;

    s_url = *url;
    s_file_size = file_size;
    s_received = 0;
    s_page_off = 0;
    s_hdr_len = 0;
    s_req_len = s_req_sent = 0;
    s_err = OTA_ST_OK;
    s_t0 = (uint32_t)g_sys_ms;
    s_step_t0 = s_t0;
    ota_crc32_init(&s_crc);
    s_state = HS_WAIT_LINK;

    if (build_request() != OTA_ST_OK)
        return set_fail(OTA_ST_BAD_SIZE);
    return OTA_ST_OK;
}

int ota_https_poll(void)
{
    if (s_state == HS_DONE) return 1;
    if (s_state == HS_FAIL) return s_err;
    if (s_state == HS_IDLE) return -1;

    if (elapsed(s_t0, TOTAL_TMO_MS))
        return set_fail(OTA_ST_BAD_FRAME);

    switch (s_state) {
    case HS_WAIT_LINK:
        if (!transport_is_online()) {
            if (elapsed(s_step_t0, LINK_TMO_MS))
                return set_fail(OTA_ST_BAD_STATE);
            return 0;
        }
        wss_tls_init();
        if (s_url.is_ipv4) wss_tls_set_hostname(NULL);
        else               wss_tls_set_hostname(s_url.host);
        s_step_t0 = (uint32_t)g_sys_ms;
        s_state = HS_TLS;
        return 0;

    case HS_TLS: {
        int r = wss_tls_handshake();
        if (r == 1) {
            if (elapsed(s_step_t0, TLS_TMO_MS))
                return set_fail(OTA_ST_BAD_STATE);
            return 0;
        }
        if (r < 0) return set_fail(OTA_ST_BAD_STATE);
        s_step_t0 = (uint32_t)g_sys_ms;
        s_state = HS_SEND;
        return 0;
    }

    case HS_SEND: {
        int n = wss_tls_write((const uint8_t *)s_req + s_req_sent,
                              s_req_len - s_req_sent);
        if (n < 0) return set_fail(OTA_ST_BAD_STATE);
        s_req_sent += (uint32_t)n;
        if (s_req_sent < s_req_len) {
            if (elapsed(s_step_t0, HDR_TMO_MS))
                return set_fail(OTA_ST_BAD_STATE);
            return 0;
        }
        s_step_t0 = (uint32_t)g_sys_ms;
        s_state = HS_HEADER;
        return 0;
    }

    case HS_HEADER: {
        uint8_t tmp[BODY_READ_MAX];
        int n = wss_tls_read(tmp, sizeof(tmp));

        if (n < 0) return set_fail(OTA_ST_BAD_STATE);
        if (n == 0) {
            if (elapsed(s_step_t0, HDR_TMO_MS))
                return set_fail(OTA_ST_BAD_STATE);
            return 0;
        }
        {
            uint32_t k = (uint32_t)n;
            if (s_hdr_len + k > RESP_HDR_MAX)
                return set_fail(OTA_ST_BAD_SIZE);

            /* 头部 + 可能的 body 前缀：先全部进缓冲再找分隔符 */
            memcpy(&s_hdr[s_hdr_len], tmp, k);
            s_hdr_len += k;

            {
                uint32_t i;
                for (i = 0; i + 3 < s_hdr_len; i++) {
                    if (s_hdr[i] == '\r' && s_hdr[i + 1] == '\n' &&
                        s_hdr[i + 2] == '\r' && s_hdr[i + 3] == '\n') {
                        uint32_t body_off = i + 4;
                        int rc;

                        s_hdr[i] = 0;   /* 便于字符串查找 */
                        rc = parse_header();
                        if (rc != OTA_ST_OK) return set_fail(rc);

                        s_step_t0 = (uint32_t)g_sys_ms;
                        s_state = HS_BODY;
                        if (s_hdr_len > body_off) {
                            if (feed_body(&s_hdr[body_off], s_hdr_len - body_off) != 0)
                                return set_fail(OTA_ST_FLASH_ERR);
                        }
                        s_hdr_len = 0;
                        if (s_received == s_file_size) {
                            if (flush_page() != 0) return set_fail(OTA_ST_FLASH_ERR);
                            s_state = HS_DONE;
                            return 1;
                        }
                        break;
                    }
                }
            }
        }
        if (s_state == HS_BODY) return 0;
        return 0;
    }

    case HS_BODY: {
        uint8_t tmp[BODY_READ_MAX];
        int n = wss_tls_read(tmp, sizeof(tmp));

        if (n < 0) return set_fail(OTA_ST_BAD_FRAME);
        if (n == 0) {
            if (s_received == s_file_size) {
                if (flush_page() != 0) return set_fail(OTA_ST_FLASH_ERR);
                s_state = HS_DONE;
                return 1;
            }
            if (elapsed(s_step_t0, BODY_IDLE_TMO_MS))
                return set_fail(OTA_ST_BAD_FRAME);
            return 0;
        }
        s_step_t0 = (uint32_t)g_sys_ms;
        if (feed_body(tmp, (uint32_t)n) != 0)
            return set_fail(OTA_ST_FLASH_ERR);
        if (s_received == s_file_size) {
            if (flush_page() != 0) return set_fail(OTA_ST_FLASH_ERR);
            s_state = HS_DONE;
            return 1;
        }
        return 0;
    }

    default:
        return set_fail(OTA_ST_BAD_STATE);
    }
}

void ota_https_abort(void)
{
    if (s_state != HS_IDLE) {
        wss_tls_close();
        s_state = HS_IDLE;
        s_received = 0;
    }
}

uint8_t ota_https_progress(void)
{
    if (s_file_size == 0) return 0;
    return (uint8_t)((uint64_t)s_received * 100u / s_file_size);
}

uint32_t ota_https_received(void)
{
    return s_received;
}

uint32_t ota_https_crc32(void)
{
    return ota_crc32_final(s_crc);
}
