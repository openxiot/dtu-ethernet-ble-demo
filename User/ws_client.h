/*********************************************************************
 * File Name          : ws_client.h
 *
 * Description        : WebSocket 客户端（RFC6455），与网络传输解耦。
 *                      通过 ws_io_t（底层由 wss_tls 提供字节流）工作：
 *                        - 握手(HTTP Upgrade + Sec-WebSocket-Accept 校验)
 *                        - 客户端帧双层掩码(必须 mask)
 *                        - 分片拼接 / 控制帧(ping→pong, close)
 *                        - 对外回调：文本帧 / 二进制帧 / 关闭
 *******************************************************************************/
#ifndef __WS_CLIENT_H__
#define __WS_CLIENT_H__

#include <stdint.h>

/* 底层字节流 I/O（wss_tls 提供）。
 * read : 非阻塞读取，返回实际读取字节(<=maxlen)；无数据返回 0；出错返回 <0。
 * write: 返回实际写入字节；出错返回 <0。 */
typedef struct {
    int (*read)(void *ctx, uint8_t *buf, uint32_t maxlen);
    int (*write)(void *ctx, const uint8_t *buf, uint32_t len);
    void *ctx;
} ws_io_t;

struct ws_client;
typedef void (*ws_frame_cb)(struct ws_client *ws, const uint8_t *data, uint32_t len);
typedef void (*ws_close_cb)(struct ws_client *ws, uint16_t code);

#define WS_FIFO_SIZE   512         /* 接收流缓冲(含帧头+内容)；DTU 收帧很小，512 足够 */
#define WS_TX_SIZE     256         /* 发送缓冲(含 4 掩码 + 内容)，覆盖心跳/查询应答/上报 */

typedef struct ws_client {
    ws_io_t  io;
    int      open;                 /* 握手成功后置 1 */

    /* 发送缓冲：tx[0..hdr) 放帧头+掩码，tx[hdr..) 放负载 */
    uint8_t  tx[WS_TX_SIZE];
    uint32_t mask_key;             /* 客户端掩码，每次发送轮号+1 */

    /* 分片状态 */
    uint8_t  frag_op;              /* 分片消息原始 opcode，0=无分片进行中 */

    /* 握手状态机（合作式，非阻塞） */
    uint8_t  hs_state;             /* 0=待发请求 1=读状态行 2=读头 3=完成 */
    char     hs_key[32];
    char     hs_accept[48];
    char     hs_line[160];
    uint16_t hs_ilen;

    /* 接收环形缓冲 */
    uint8_t  fifo[WS_FIFO_SIZE];
    uint16_t fifo_head, fifo_tail;

    /* 单帧内容缓冲（去向回调） */
    uint8_t  payload[256];

    /* 回调 */
    ws_frame_cb on_text;
    ws_frame_cb on_bin;
    ws_close_cb on_close;
    void       *app_ctx;
} ws_client_t;

/* 初始化。host/path 在握手时传入。 */
void ws_client_init(ws_client_t *ws, const ws_io_t *io, uint32_t seed);

/* 建立 WebSocket（合作式、非阻塞，需在每轮主循环重复调用直至完成）。
 * host: 不带端口的域名；port: 十进制端口字符串(用于 Host 头)；path: 需以 '/' 开头。
 * 返回：0=握手成功(open 置 1)；1=仍在进行，请稍后再次调用；
 *       <0=握手失败(可触发重连)。 */
int ws_client_handshake(ws_client_t *ws, const char *host, const char *port,
                        const char *path);

/* 发送一帧。返回 0 成功，<0 失败。 */
int ws_send_text(ws_client_t *ws, const char *text);
int ws_send_bin(ws_client_t *ws, const uint8_t *data, uint32_t len);
int ws_send_ping(ws_client_t *ws, const uint8_t *data, uint32_t len);

/* 处理链路字节流：读取并解析可得到的一帧，触发回调。非阻塞。
 * 返回 1=已处理至少一帧完整消息，0=暂无可处理数据，<0=出错。 */
int ws_client_poll(ws_client_t *ws);

/* 主动发送 Close；之后调用方应关闭底层连接。 */
void ws_client_close(ws_client_t *ws);

#endif /* __WS_CLIENT_H__ */