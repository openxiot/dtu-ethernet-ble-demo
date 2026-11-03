/*********************************************************************
 * File Name          : ota_url.c
 *
 * Description        : OTA URL 解析。不做域名白名单（评审决定）。
 *******************************************************************************/
#include "ota_url.h"
#include "ota_layout.h"
#include <string.h>

static int is_host_char(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '-' || c == '.';
}

static int host_is_ipv4(const char *h)
{
    uint32_t part = 0;
    int digits = 0;

    for (; *h; h++) {
        if (*h == '.') {
            if (!digits) return 0;
            part++;
            digits = 0;
        } else if (*h >= '0' && *h <= '9') {
            digits++;
            if (digits > 3) return 0;
        } else {
            return 0;
        }
    }
    return part == 3 && digits > 0;
}

int ota_url_parse(const char *url, uint32_t len, ota_url_t *out)
{
    const char *p, *auth_end, *path;
    uint32_t n;
    uint32_t host_len;
    uint32_t port = 443;

    if (!url || !out) return OTA_ST_BAD_PACKAGE;
    if (len == 0 || len > OTA_URL_MAX_LEN) return OTA_ST_BAD_SIZE;

    /* scheme 只接受小写 https:// */
    if (len < 9 || memcmp(url, "https://", 8) != 0)
        return OTA_ST_BAD_PACKAGE;

    p = url + 8;
    auth_end = url + len;
    for (; p < auth_end; p++) {
        if (*p == '/' || *p == '?') break;
        if (*p == '#' || *p == '@' || *p == '\\' ||
            (unsigned char)*p <= 0x20 || (unsigned char)*p >= 0x7F)
            return OTA_ST_BAD_PACKAGE;
    }
    if (p == url + 8) return OTA_ST_BAD_PACKAGE;   /* 空 host */

    /* 端口 */
    {
        const char *colon = NULL;
        const char *q;
        for (q = url + 8; q < p; q++) if (*q == ':') colon = q;
        if (colon) {
            const char *d;
            uint32_t v = 0;
            if (colon == url + 8) return OTA_ST_BAD_PACKAGE;
            for (d = colon + 1; d < p; d++) {
                if (*d < '0' || *d > '9') return OTA_ST_BAD_PACKAGE;
                v = v * 10u + (uint32_t)(*d - '0');
                if (v > 65535u) return OTA_ST_BAD_PACKAGE;
            }
            if (v == 0) return OTA_ST_BAD_PACKAGE;
            port = v;
            host_len = (uint32_t)(colon - (url + 8));
        } else {
            host_len = (uint32_t)(p - (url + 8));
        }
    }

    if (host_len == 0 || host_len >= OTA_HOST_MAX_LEN)
        return OTA_ST_BAD_SIZE;

    memcpy(out->host, url + 8, host_len);
    out->host[host_len] = 0;
    for (n = 0; n < host_len; n++)
        if (!is_host_char(out->host[n])) return OTA_ST_BAD_PACKAGE;
    if (out->host[0] == '.' || out->host[0] == '-') return OTA_ST_BAD_PACKAGE;

    out->is_ipv4 = (uint8_t)host_is_ipv4(out->host);
    out->port = (uint16_t)port;

    /* path：为空时补 "/" */
    path = p;
    if (path == auth_end) {
        out->path[0] = '/';
        out->path[1] = 0;
        return OTA_ST_OK;
    }
    if (*path != '/') return OTA_ST_BAD_PACKAGE;    /* "?query" 没有 path */

    n = (uint32_t)(auth_end - path);
    if (n >= OTA_PATH_MAX_LEN) return OTA_ST_BAD_SIZE;
    memcpy(out->path, path, n);
    out->path[n] = 0;

    for (n = 0; n < (uint32_t)(auth_end - path); n++) {
        char c = out->path[n];
        if (c == '#' || c == ' ' || c == '\r' || c == '\n' ||
            (unsigned char)c < 0x20 || (unsigned char)c == 0x7F)
            return OTA_ST_BAD_PACKAGE;
    }
    if (strstr(out->path, "..") != NULL) return OTA_ST_BAD_PACKAGE;

    return OTA_ST_OK;
}
