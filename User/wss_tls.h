/*********************************************************************
 * File Name          : wss_tls.h
 *
 * Description        : wss 的 TLS 适配层。为上层 WebSocket 提供统一的
 *                      加解密字节流接口，底层流由 transport_eth 提供
 *                      （transport_tcp_read/write）。
 *
 *                      DTU_TLS(见 dtu_config.h)：
 *                        0 —— 明文透传（ws://，用于先联调 DTU 逻辑）
 *                        1 —— wss（需在此接入一个 TLS 引擎，见 wss_tls.c 注释）
 *
 * wss_tls_read/write 语义与 ws_io_t 一致：
 *   read : 非阻塞，返回读取字节(<=max)；无数据 0；差错/未连接 <0。
 *   write: 返回写入字节；差错/未连接 <0。
 *******************************************************************************/
#ifndef __WSS_TLS_H__
#define __WSS_TLS_H__

#include <stdint.h>

/* 初始化 TLS 上下文 */
void wss_tls_init(void);

/* 在 TCP 之上完成 TLS 握手（合作式、非阻塞，需在主循环反复调用直至结束）。
 * 返回：0=握手完成(可收发)；1=仍在进行，请稍后再次调用；<0=握手失败。 */
int  wss_tls_handshake(void);

/* 加解密字节流 I/O */
int  wss_tls_read(uint8_t *buf, uint32_t maxlen);
int  wss_tls_write(const uint8_t *buf, uint32_t len);

/* TLS 层是否已 Open（TLS=0 时等于 TCP online）。 */
int  wss_tls_is_open(void);

/* 关闭 TLS（底层连接由 transport 负责）。 */
void wss_tls_close(void);

#endif /* __WSS_TLS_H__ */