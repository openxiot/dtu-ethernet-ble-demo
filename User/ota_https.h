/*********************************************************************
 * File Name          : ota_https.h
 *
 * Description        : OTA 固件 HTTPS GET 下载（设计文档 9.4）。
 *                      复用 transport 的单个 TCP socket 与 wss_tls 的 mbedTLS。
 *                      poll 式状态机，下载过程中不得同时运行 WS。
 *******************************************************************************/
#ifndef __OTA_HTTPS_H__
#define __OTA_HTTPS_H__

#include <stdint.h>
#include "ota_url.h"

/* 开始下载：url 必须已通过 ota_url_parse 校验。
 * file_size 是整个 .dtufw 的字节数（512 + 256 对齐镜像）。 */
int  ota_https_start(const ota_url_t *url, uint32_t file_size);

/* 推进下载。返回 0=进行中，1=完成，<0=OTA_ST_* 错误。 */
int  ota_https_poll(void);

/* 中止并释放 TLS；不改动 transport 的目标地址。 */
void ota_https_abort(void);

uint8_t  ota_https_progress(void);     /* 0..100 */
uint32_t ota_https_received(void);     /* 已收字节数 */
uint32_t ota_https_crc32(void);        /* 整个文件的 CRC32（完成后有效） */

#endif /* __OTA_HTTPS_H__ */
