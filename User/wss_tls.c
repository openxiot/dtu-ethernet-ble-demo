/*********************************************************************
 * File Name          : wss_tls.c
 *
 * Description        : wss 的 TLS 适配层。
 *
 * 【重要】本仓库未附带任何 TLS 引擎（WCHNET 只提供 TCP，不含 TLS）。
 *   - DTU_TLS=0：本文件做明文透传，直接桥接 transport_tcp_read/write。
 *     用于先把「WebSocket 握手 + 心跳 + csq/coordinates 上报 + 查询应答」整条
 *     DTU 链路跑通（服务端若提供 ws:// 明文入口，用这种最省）。
 *   - DTU_TLS=1：把 wss_tls_read/write 的明文直通替换为对所选 TLS 引擎
 *     （如 mbedTLS）实例的收发，并在 wss_tls_handshake() 里完成握手。
 *     底层仍是 transport_tcp_read/write；mbedTLS 需裁剪配置以适配 32K SRAM
 *     （例如关闭证书链校验、固定/内嵌服务器证书）。
 *     说明：把 TLS 引擎当作选型项，未随仓库提供；接入后本文件是唯一需要改动处。
 *******************************************************************************/
#include "wss_tls.h"
#include "dtu_config.h"
#include "transport_eth.h"

#if DTU_TLS == 0
/* ---------------- 明文透传（ws://） ---------------- */
void wss_tls_init(void) { }
int  wss_tls_handshake(void) { return 0; }
int  wss_tls_read(uint8_t *buf, uint32_t maxlen) { return transport_tcp_read(buf, maxlen); }
int  wss_tls_write(const uint8_t *buf, uint32_t len) { return transport_tcp_write(buf, len); }
int  wss_tls_is_open(void) { return transport_is_online(); }
void wss_tls_close(void) { }

#else /* DTU_TLS == 1: wss，接入 mbedTLS */

/*
 * mbedTLS 2.28.x LTS 已内置到工程 mbedtls/ 目录（include + library），
 * 使用其默认配置编译；mbedtls/include 已在 .cproject 加进头文件搜索路径。
 * 注意（BRU 运行时内存）：CH32V208 仅 32K SRAM，默认 mbedTLS 的 16K 记录缓冲
 * 无法在运行时与 WCHNET 缓冲共存；若要真正跑起来，需在此基础上裁剪配置
 * （定义 MBEDTLS_CONFIG_FILE 指向一份精简配置并相应调小 SSL 记录缓冲、
 *  关闭大量证书/套件），那一步需在真实工具链上编译联调——见 README 第五节。
 * 本文件 API（mbedtls_ssl_* / entropy / ctr_drbg）在 2.28 下均有。
 */
#include "mbedtls/ssl.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/error.h"
#include <string.h>
#include <stdio.h>

static mbedtls_ssl_context      ssl;
static mbedtls_ssl_config       conf;
static mbedtls_entropy_context  entropy;
static mbedtls_ctr_drbg_context drbg;

static int tls_started;   /* 上下文已初始化、首次握手已开始 */
static int tls_ready;     /* 握手完成，可收发 */

/* 无硬件熵源时的弱熵回调（只用做密钥随机种子的补充）。
 * 正式产品请接入真实 TRNG/ADC 噪声等。 */
static uint32_t entropy_acc;
static int weak_entropy(void *p, unsigned char *out, size_t len, size_t *olen)
{
    size_t i;
    (void)p;
    entropy_acc += (uint32_t)g_sys_ms + 0x9E3779B9u;
    for (i = 0; i < len; i++) {
        uint32_t x = entropy_acc;
        x ^= x << 13; x ^= x >> 17; x ^= x << 5;
        entropy_acc = x;
        out[i] = (unsigned char)(x >> 24);
    }
    *olen = len;
    return 0;
}

/* ---------------- mbedTLS BIO：桥接到 WCHNET TCP ---------------- */
/* send：全部写出的字节或 WANT_WRITE；recv：读到的字节或 WANT_READ。 */

static int net_send(void *ctx, const unsigned char *buf, size_t len)
{
    int n;
    (void)ctx;
    if (!transport_is_online()) return MBEDTLS_ERR_SSL_CONN_EOF;
    n = transport_tcp_write(buf, (uint32_t)len);
    if (n < 0) return MBEDTLS_ERR_SSL_CONN_EOF;
    if (n == 0) return MBEDTLS_ERR_SSL_WANT_WRITE;   /* 发送缓冲满，稍后重试 */
    return n;
}

static int net_recv(void *ctx, unsigned char *buf, size_t len)
{
    int n;
    (void)ctx;
    if (!transport_is_online()) return MBEDTLS_ERR_SSL_CONN_EOF;
    n = transport_tcp_read(buf, (uint32_t)len);
    if (n < 0) return MBEDTLS_ERR_SSL_CONN_EOF;      /* 连接已断开 */
    if (n == 0) return MBEDTLS_ERR_SSL_WANT_READ;    /* 暂无可读，稍后重试 */
    return n;
}

/* ---------------- 对外接口 ---------------- */
void wss_tls_init(void)
{
    int ret;

    tls_started = 0;
    tls_ready   = 0;
    entropy_acc = 0;

    mbedtls_ssl_init(&ssl);
    mbedtls_ssl_config_init(&conf);
    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&drbg);

    ret = mbedtls_entropy_add_source(&entropy, weak_entropy, NULL, 256,
                                     MBEDTLS_ENTROPY_SOURCE_STRONG);
    if (ret != 0) { printf("[tls] entropy source fail %04X\r\n", (unsigned)ret); return; }
    if (mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy,
                              (const unsigned char *)DTU_IMEI, strlen(DTU_IMEI)) != 0) {
        printf("[tls] ctr_drbg seed fail\r\n"); return;
    }
}

int wss_tls_handshake(void)
{
    int ret;

    if (!transport_is_online()) return -2;

    if (!tls_started) {
        ret = mbedtls_ssl_config_defaults(&conf,
                     MBEDTLS_SSL_IS_CLIENT,
                     MBEDTLS_SSL_TRANSPORT_STREAM,
                     MBEDTLS_SSL_PRESET_DEFAULT);
        if (ret != 0) return -3;

        /* 最小化：不校验证书链以省 RAM（正式环境建议 mbedtls_ssl_conf_ca_chain
         * 校验服务器，并权衡 SRAM；也可内嵌/固定服务器证书而只启 MBEDTLS_SSL_VERIFY_OPTIONAL）。 */
        mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_NONE);
        mbedtls_ssl_conf_rng(&conf, mbedtls_ctr_drbg_random, &drbg);

        if (mbedtls_ssl_setup(&ssl, &conf) != 0) return -4;
        mbedtls_ssl_set_bio(&ssl, NULL, net_send, net_recv, NULL);
        if (mbedtls_ssl_set_hostname(&ssl, DTU_HOST) != 0) return -5;

        tls_started = 1;
    }

    ret = mbedtls_ssl_handshake(&ssl);
    if (ret == 0) {
        tls_ready = 1;
        return 0;                                  /* 完成 */
    }
    if (ret == MBEDTLS_ERR_SSL_WANT_READ ||
        ret == MBEDTLS_ERR_SSL_WANT_WRITE)
        return 1;                                  /* 进行中 */
    {
        char eb[80];
        mbedtls_strerror(ret, eb, sizeof eb);
        printf("[tls] handshake fail %04X %s\r\n", (unsigned)ret, eb);
        return -6;                                 /* 失败 */
    }
}

int wss_tls_read(uint8_t *buf, uint32_t maxlen)
{
    int n;

    if (!tls_ready || !transport_is_online()) return -1;
    n = mbedtls_ssl_read(&ssl, buf, maxlen);
    if (n == MBEDTLS_ERR_SSL_WANT_READ ||
        n == MBEDTLS_ERR_SSL_WANT_WRITE)
        return 0;                                  /* 暂无可读明文 */
    if (n < 0) return -1;                          /* 解密失败 / 对端关闭 */
    return n;
}

int wss_tls_write(const uint8_t *buf, uint32_t len)
{
    int n;

    if (!tls_ready || !transport_is_online()) return -1;
    n = mbedtls_ssl_write(&ssl, buf, len);
    if (n == MBEDTLS_ERR_SSL_WANT_READ ||
        n == MBEDTLS_ERR_SSL_WANT_WRITE)
        return 0;                                  /* 写缓冲满，稍后重试 */
    if (n < 0) return -1;
    return n;
}

int wss_tls_is_open(void)
{
    return tls_ready && transport_is_online();
}

void wss_tls_close(void)
{
    if (tls_ready) {
        mbedtls_ssl_close_notify(&ssl);            /* 尽力发送 close_notify */
    }
    mbedtls_ssl_free(&ssl);
    mbedtls_ssl_config_free(&conf);
    mbedtls_ctr_drbg_free(&drbg);
    mbedtls_entropy_free(&entropy);
    tls_started = 0;
    tls_ready   = 0;
}

#endif /* DTU_TLS */