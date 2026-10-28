/*********************************************************************
 * File Name          : mbedtls_config.h
 *
 * Description        : 本工程 mbedTLS 的**完整精简配置**（替换默认大配置）。
 *                      由 mbedtls/include/mbedtls/config.h 直接包含。
 *
 * 目标：CH32V208 (32K SRAM / 160K Flash) 上作为客户端连
 *       wss://ws.dtu.ap.openxiot.cn:443 ，只保留跑通所需的最小集合。
 *
 * 依据对服务端的实测：
 *   - 证书为 RSA-2048（DigiCert GeoTrust 链，3 级，约 3665 字节）；
 *   - TLS1.2 仅接受 ECDHE-RSA-AES128/256-GCM-SHAx（ECDSA、纯 RSA 交换均拒绝）；
 *   - 设备不校验服务端证书（VERIFY_NONE），但需解析证书取公钥验握手签名。
 * 故本配置只开：TLS1.2 + ECDHE_RSA + AES-128-GCM-SHA256 + RSA/ECP(secp256r1)
 *             + X509 解析 + 熵/DRBG；其余（DHM/ECDSA/CBC/CCM/DTLS/PSA/FS/PEM…）全关。
 *******************************************************************************/
#ifndef MBEDTLS_CONFIG_H_LOCAL
#define MBEDTLS_CONFIG_H_LOCAL

/* ============ 曲线：只用 secp256r1 ============ */
#define MBEDTLS_ECP_DP_SECP256R1_ENABLED

/* ============ 密钥交换 / TLS ============ */
#define MBEDTLS_KEY_EXCHANGE_ECDHE_RSA_ENABLED
#define MBEDTLS_SSL_PROTO_TLS1_2
#define MBEDTLS_SSL_CLI_C
#define MBEDTLS_SSL_TLS_C
#define MBEDTLS_SSL_SERVER_NAME_INDICATION    /* SNI */

/* TLS 记录缓冲：输入需容纳服务器证书链(~3.7K)，输出只发 ClientHello 等小消息 */
#define MBEDTLS_SSL_MAX_CONTENT_LEN     4096
#define MBEDTLS_SSL_IN_CONTENT_LEN      4096
#define MBEDTLS_SSL_OUT_CONTENT_LEN     512

/* 只用一个密码套件（TLS1.2 + ECDHE_RSA + AES-128-GCM + SHA256） */
#define MBEDTLS_SSL_CIPHERSUITES \
    MBEDTLS_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256

/* ============ 对称算法 ============ */
#define MBEDTLS_AES_C
#define MBEDTLS_GCM_C
#define MBEDTLS_CIPHER_C
#define MBEDTLS_AES_ROM_TABLES              /* 表放 Flash，省 RAM */

/* ============ 哈希 ============ */
#define MBEDTLS_SHA256_C
#define MBEDTLS_MD_C

/* ============ 大数 / 公钥 ============ */
#define MBEDTLS_BIGNUM_C
#define MBEDTLS_ECP_C
#define MBEDTLS_ECDH_C
#define MBEDTLS_RSA_C
#define MBEDTLS_PKCS1_V15                    /* RSA 证书/签名用 PKCS1 v1.5 */
#define MBEDTLS_PK_C
#define MBEDTLS_PK_PARSE_C                   /* 解析证书里的公钥 */
#define MBEDTLS_ASN1_PARSE_C
#define MBEDTLS_ASN1_WRITE_C
#define MBEDTLS_OID_C

/* ============ X509（仅解析，不验签；取服务器公钥用） ============ */
#define MBEDTLS_X509_USE_C
#define MBEDTLS_X509_CRT_PARSE_C

/* ============ 错误信息（不用完整字符串表，仅提供 mbedtls_strerror 桩） ============ */
#define MBEDTLS_ERROR_STRERROR_DUMMY

/* ============ 随机数 ============ */
#define MBEDTLS_ENTROPY_C
#define MBEDTLS_CTR_DRBG_C
#define MBEDTLS_NO_PLATFORM_ENTROPY          /* 用 wss_tls.c 的 weak_entropy */
#define MBEDTLS_ENTROPY_FORCE_SHA256         /* 未开 SHA512，熵用 SHA256 */
#define MBEDTLS_ENTROPY_MAX_SOURCES         1

/* ============ ECP/MPI 调优（省 RAM） ============ */
#define MBEDTLS_ECP_WINDOW_SIZE             2
#define MBEDTLS_ECP_FIXED_POINT_OPTIM       0
#define MBEDTLS_ECP_NIST_OPTIM
#define MBEDTLS_MPI_MAX_SIZE                256   /* RSA-2048 模数 */

#endif /* MBEDTLS_CONFIG_H_LOCAL */
