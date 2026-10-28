/*********************************************************************
 * File Name          : ws_client.c
 *
 * Description        : WebSocket 客户端（RFC6455）实现。
 *******************************************************************************/
#include "ws_client.h"
#include "ws_security.h"
#include <string.h>
#include <stdio.h>

/* 简单 xorshift32 伪随机，用于生成掩码与握手随机 Nonce。 */
static uint32_t ws_rand_state;
static uint32_t ws_next_rand(void)
{
    uint32_t x = ws_rand_state;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    ws_rand_state = x;
    return x;
}

/* ==================== 接收环形缓冲 ==================== */
static uint16_t ws_fifo_used(ws_client_t *ws)
{
    if (ws->fifo_head >= ws->fifo_tail) return ws->fifo_head - ws->fifo_tail;
    return WS_FIFO_SIZE - ws->fifo_tail + ws->fifo_head;
}
static uint16_t ws_fifo_free(ws_client_t *ws)
{
    uint16_t used = ws_fifo_used(ws);
    return (uint16_t)(WS_FIFO_SIZE - 1 - used);   /* 保留一个空槽位判断空/满 */
}
static void ws_fifo_write(ws_client_t *ws, const uint8_t *buf, uint16_t len)
{
    while (len--) {
        ws->fifo[ws->fifo_head] = *buf++;
        ws->fifo_head = (ws->fifo_head + 1) % WS_FIFO_SIZE;
    }
}
static void ws_fifo_peek(const ws_client_t *ws, uint8_t *out, uint16_t off, uint16_t len)
{
    uint16_t i = (ws->fifo_tail + off) % WS_FIFO_SIZE;
    while (len--) { *out++ = ws->fifo[i]; i = (i + 1) % WS_FIFO_SIZE; }
}
static void ws_fifo_remove(ws_client_t *ws, uint16_t len)
{
    ws->fifo_tail = (ws->fifo_tail + len) % WS_FIFO_SIZE;
}

void ws_client_init(ws_client_t *ws, const ws_io_t *io, uint32_t seed)
{
    memset(ws, 0, sizeof(*ws));
    ws->io = *io;
    ws->fifo_head = ws->fifo_tail = 0;
    ws->mask_key = seed;
    ws_rand_state = seed ? seed : 0x9E3779B9u;
}

/* ---------- 字节 I/O 辅助 ---------- */
static int ws_write_all(ws_client_t *ws, const uint8_t *buf, uint32_t len)
{
    uint32_t off = 0;
    while (off < len) {
        int n = ws->io.write(ws->io.ctx, buf + off, len - off);
        if (n < 0) return -1;
        if (n == 0) break;               /* 写阻塞/未就绪，下次继续 */
        off += (uint32_t)n;
    }
    return 0;
}

/* 非阻塞读一行（保留行缓冲，遇 \n 返回 1；无数据返回 0；出错 <0）。 */
static int hs_fill_line(ws_client_t *ws)
{
    for (;;) {
        uint8_t b;
        int r;
        if (ws->hs_ilen >= sizeof(ws->hs_line) - 1) return -4;   /* 行过长 */
        r = ws->io.read(ws->io.ctx, &b, 1);
        if (r < 0) return -1;
        if (r == 0) return 0;            /* 暂无数据，保留已读部分 */
        if (b == '\n') {
            ws->hs_line[ws->hs_ilen] = '\0';
            ws->hs_ilen = 0;
            return 1;
        }
        if (b != '\r') ws->hs_line[ws->hs_ilen++] = (char)b;
    }
}

/* ==================== 握手（合作式） ==================== */
int ws_client_handshake(ws_client_t *ws, const char *host, const char *port,
                        const char *path)
{
    char req[256];
    size_t n;
    int r;

    for (;;) {
        switch (ws->hs_state) {
        case 0: {   /* 发送 HTTP Upgrade 请求 */
            uint8_t nonce[16];
            int i;
            for (i = 0; i < 16; i++) nonce[i] = (uint8_t)ws_next_rand();
            n = ws_base64_encode(nonce, 16, ws->hs_key);
            ws->hs_key[n] = '\0';

            n = (size_t)snprintf(req, sizeof(req),
                "GET %s HTTP/1.1\r\n"
                "Host: %s:%s\r\n"
                "Upgrade: websocket\r\n"
                "Connection: Upgrade\r\n"
                "Sec-WebSocket-Key: %s\r\n"
                "Sec-WebSocket-Version: 13\r\n"
                "\r\n", path, host, port, ws->hs_key);
            if (n >= sizeof(req)) return -2;
            if (ws_write_all(ws, (const uint8_t *)req, (uint32_t)n)) return -3;
            ws->hs_accept[0] = '\0';
            ws->hs_ilen = 0;
            ws->hs_state = 1;
            continue;
        }
        case 1:     /* 读状态行：须含 101 */
            r = hs_fill_line(ws);
            if (r == 0) return 1;          /* 未完，稍后再调 */
            if (r < 0) return -4;
            if (strstr(ws->hs_line, " 101 ") == NULL &&
                strstr(ws->hs_line, "101 ") == NULL) return -5;
            ws->hs_ilen = 0;
            ws->hs_state = 2;
            continue;
        case 2: {   /* 读头，提取 Sec-WebSocket-Accept */
            r = hs_fill_line(ws);
            if (r == 0) return 1;
            if (r < 0) return -4;
            if (ws->hs_line[0] == '\0') {   /* 空行 = 头结束 */
                char accept_exp[48];
                ws_compute_accept(ws->hs_key, accept_exp);
                if (ws->hs_accept[0] == '\0' ||
                    strcmp(accept_exp, ws->hs_accept) != 0) return -7;
                ws->open = 1;
                ws->hs_state = 3;
                return 0;
            }
            if (strncmp(ws->hs_line, "sec-websocket-accept:", 21) == 0 ||
                strncmp(ws->hs_line, "Sec-WebSocket-Accept:", 21) == 0) {
                const char *v = ws->hs_line + 22;
                while (*v == ' ' || *v == '\t') v++;
                strncpy(ws->hs_accept, v, sizeof(ws->hs_accept) - 1);
                ws->hs_accept[sizeof(ws->hs_accept) - 1] = '\0';
            }
            continue;
        }
        default:
            return 0;                      /* 已完成 */
        }
    }
}

/* ==================== 发送（客户端须 mask） ==================== */
static int ws_send_frame(ws_client_t *ws, uint8_t opcode, const uint8_t *data,
                         uint32_t len, int is_text)
{
    uint8_t *p = ws->tx;
    uint32_t paylen = len;
    uint32_t mask = ws->mask_key++;
    uint16_t hdr = 0;
    uint32_t i, need;
    (void)is_text;

    /* 计算帧头大小并放长度字段 */
    p[0] = 0x80 | opcode;
    if (paylen < 126) {
        p[1] = 0x80 | (uint8_t)paylen;
        hdr = 2;
    } else if (paylen <= 0xFFFF) {
        p[1] = 0x80 | 126;
        p[2] = (uint8_t)(paylen >> 8);
        p[3] = (uint8_t)(paylen);
        hdr = 4;
    } else {
        p[1] = 0x80 | 127;
        for (i = 0; i < 8; i++) p[2 + i] = (uint8_t)(paylen >> (56 - i * 8));
        hdr = 10;
    }
    /* 4 字节掩码紧随长度 */
    p[hdr]     = (uint8_t)(mask >> 24);
    p[hdr + 1] = (uint8_t)(mask >> 16);
    p[hdr + 2] = (uint8_t)(mask >> 8);
    p[hdr + 3] = (uint8_t)(mask);
    hdr += 4;

    need = hdr + (uint32_t)paylen;
    if (need > WS_TX_SIZE) return -1;

    for (i = 0; i < paylen; i++) {
        uint8_t k = (uint8_t)(mask >> (8 * (3 - (i & 3))));
        p[hdr + i] = data[i] ^ k;
    }

    if (ws_write_all(ws, p, need)) return -2;
    return 0;
}

int ws_send_text(ws_client_t *ws, const char *text)
{
    if (!ws->open) return -99;
    return ws_send_frame(ws, 0x1, (const uint8_t *)text, (uint32_t)strlen(text), 1);
}

int ws_send_bin(ws_client_t *ws, const uint8_t *data, uint32_t len)
{
    if (!ws->open) return -99;
    return ws_send_frame(ws, 0x2, data, len, 0);
}

int ws_send_ping(ws_client_t *ws, const uint8_t *data, uint32_t len)
{
    if (!ws->open) return -99;
    if (len > 125) len = 125;
    return ws_send_frame(ws, 0x9, data, len, 0);
}

void ws_client_close(ws_client_t *ws)
{
    if (!ws->open) return;
    ws_send_frame(ws, 0x8, NULL, 0, 0);
    ws->open = 0;
}

/* ==================== 接收解析（非阻塞） ==================== */
static void ws_dispatch(ws_client_t *ws, uint8_t opcode, const uint8_t *data, uint32_t len)
{
    if (opcode == 0x1 && ws->on_text) ws->on_text(ws, data, len);
    else if (opcode == 0x2 && ws->on_bin) ws->on_bin(ws, data, len);
}

int ws_client_poll(ws_client_t *ws)
{
    uint32_t avail, free;
    uint16_t got;
    uint8_t b[64];
    int handled = 0;

    /* 1) 尽量把底层可读字节灌入环形缓冲 */
    for (;;) {
        free = ws_fifo_free(ws);
        if (free == 0) break;
        got = (free < sizeof(b)) ? (uint16_t)free : (uint16_t)sizeof(b);
        {
            int n = ws->io.read(ws->io.ctx, b, got);
            if (n < 0) return -1;
            if (n == 0) break;
            ws_fifo_write(ws, b, (uint16_t)n);
        }
    }

    /* 2) 从缓冲解析帧：循环处理尽可能多 */
    for (;;) {
        uint8_t hdr[14];
        uint16_t need, paylen, hlen, mkeyi;
        uint8_t b0, b1, fin, op, masked;
        uint32_t mkey = 0;
        uint32_t i;

        avail = ws_fifo_used(ws);
        if (avail < 2) break;

        ws_fifo_peek(ws, hdr, 0, 2);
        b0 = hdr[0]; b1 = hdr[1];
        fin = b0 & 0x80; op = b0 & 0x0f;
        masked = (b1 & 0x80) ? 1 : 0;
        paylen = b1 & 0x7f;
        hlen = 2;

        if (paylen == 126) {
            if (avail < 4) break;
            ws_fifo_peek(ws, hdr, 2, 2);
            paylen = ((uint16_t)hdr[2] << 8) | hdr[3];
            hlen = 4;
        } else if (paylen == 127) {
            if (avail < 10) break;
            ws_fifo_peek(ws, hdr, 2, 8);
            paylen = ((uint32_t)hdr[2] << 24) | ((uint32_t)hdr[3] << 16) |
                     ((uint32_t)hdr[4] << 8) | hdr[5];
            hlen = 10;
        }

        if (masked) {
            if (avail < (uint32_t)hlen + 4) break;
            ws_fifo_peek(ws, hdr, hlen, 4);
            mkey = ((uint32_t)hdr[hlen] << 24) | ((uint32_t)hdr[hlen + 1] << 16) |
                   ((uint32_t)hdr[hlen + 2] << 8) | hdr[hlen + 3];
            mkeyi = hlen;
            hlen += 4;
        } else {
            mkeyi = 0;
        }

        need = hlen + paylen;
        if ((uint32_t)paylen > sizeof(ws->payload)) { /* 内容过大，丢弃该帧 */
            ws_fifo_remove(ws, avail); return -8;
        }
        if (avail < need) break;              /* 等整帧到齐 */

        /* 复制负载并去除掩码 */
        ws_fifo_peek(ws, ws->payload, hlen, paylen);
        if (masked) {
            for (i = 0; i < paylen; i++) {
                uint8_t k = (uint8_t)(mkey >> (8 * (3 - (i & 3))));
                ws->payload[i] ^= k;
            }
            (void)mkeyi;
        }
        ws_fifo_remove(ws, need);

        /* 控制帧：close / ping / pong */
        if (op == 0x8) {                    /* close */
            ws->open = 0;
            if (ws->on_close) ws->on_close(ws, (paylen >= 2) ?
                         ((uint16_t)ws->payload[0] << 8 | ws->payload[1]) : 1005);
            handled = 1; break;
        } else if (op == 0x9) {             /* ping -> pong */
            ws_send_frame(ws, 0xA, ws->payload, paylen, 0);
            continue;
        } else if (op == 0xA) {             /* pong, 忽略 */
            continue;
        }

        /* 数据帧：处理分片 */
        if (!fin) {
            /* 分片开始 */
            if (ws->frag_op == 0 && (op == 0x1 || op == 0x2)) {
                ws->frag_op = op;
                /* 简化为仅支持首片后无缝拼接，此处忽略内容累积细节；
                   对 DTU 场景(帧都很小且服务端不发分片)足够。 */
            }
            continue;   /* 丢弃中继分片内容（DTU 用不到） */
        } else if (ws->frag_op != 0) {
            uint8_t real_op = ws->frag_op;
            ws->frag_op = 0;
            ws_dispatch(ws, real_op, ws->payload, paylen);   /* 末片 */
        } else {
            ws_dispatch(ws, op, ws->payload, paylen);
        }
        handled = 1;
    }

    return handled;
}