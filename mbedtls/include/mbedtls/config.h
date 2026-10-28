/**
 * \file config.h
 *
 * 本工程的 mbedTLS 配置入口（已由默认大配置替换为薄封装）。
 * 真正的配置在 User/mbedtls_config.h（User/ 在头文件搜索路径中），
 * 针对 CH32V208 + ws.dtu.ap.openxiot.cn 做了最小化裁剪：
 *   TLS1.2 客户端 + ECDHE-RSA + AES-128-GCM-SHA256 + RSA/ECP(secp256r1) + X509 解析。
 * 末尾引入 check_config.h 做一致性校验。
 */
#ifndef MBEDTLS_CONFIG_H
#define MBEDTLS_CONFIG_H

#include "mbedtls_config.h"
#include "mbedtls/check_config.h"

#endif /* MBEDTLS_CONFIG_H */
