/*********************************************************************
 * File Name          : ota_url.h
 *
 * Description        : OTA 固件 URL 解析与校验（设计文档 9.3）。
 *                      只接受 https://host[:port]/path[?query]。
 *******************************************************************************/
#ifndef __OTA_URL_H__
#define __OTA_URL_H__

#include <stdint.h>

#define OTA_URL_MAX_LEN     220u
#define OTA_HOST_MAX_LEN    64u
#define OTA_PATH_MAX_LEN    160u

typedef struct {
    char     host[OTA_HOST_MAX_LEN];
    uint16_t port;                 /* 缺省 443 */
    char     path[OTA_PATH_MAX_LEN];   /* 含前导 '/' 与 query */
    uint8_t  is_ipv4;              /* host 是 IPv4 字面量时不发 SNI */
} ota_url_t;

/* 解析成功返回 OTA_ST_OK，失败返回 OTA_ST_BAD_PACKAGE / OTA_ST_BAD_SIZE。 */
int ota_url_parse(const char *url, uint32_t len, ota_url_t *out);

#endif /* __OTA_URL_H__ */
