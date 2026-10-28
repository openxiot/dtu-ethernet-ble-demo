/*********************************************************************
 * File Name          : ws_security.c
 *
 * Description        : WebSocket 握手安全原语：SHA-1 与 Base64 实现。
 *******************************************************************************/
#include "ws_security.h"
#include <string.h>

/* ==================== SHA-1 ==================== */
typedef struct {
    uint32_t h[5];
    uint64_t bytes;
    uint8_t  block[64];
    size_t   block_len;
} ws_sha1_ctx;

static uint32_t ws_rotl(uint32_t x, uint32_t n) { return (x << n) | (x >> (32 - n)); }

static void ws_sha1_process_block(ws_sha1_ctx *c, const uint8_t *p)
{
    uint32_t w[80];
    uint32_t a, b, cc, d, e;
    uint32_t f, k;
    int i;

    for (i = 0; i < 16; i++) {
        w[i] = ((uint32_t)p[i * 4] << 24) | ((uint32_t)p[i * 4 + 1] << 16) |
               ((uint32_t)p[i * 4 + 2] << 8) | (uint32_t)p[i * 4 + 3];
    }
    for (i = 16; i < 80; i++) {
        w[i] = ws_rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    }

    a = c->h[0]; b = c->h[1]; cc = c->h[2]; d = c->h[3]; e = c->h[4];
    for (i = 0; i < 80; i++) {
        if (i < 20) { f = (b & cc) | ((~b) & d);  k = 0x5A827999; }
        else if (i < 40) { f = b ^ cc ^ d;  k = 0x6ED9EBA1; }
        else if (i < 60) { f = (b & cc) | (b & d) | (cc & d);  k = 0x8F1BBCDC; }
        else { f = b ^ cc ^ d;  k = 0xCA62C1D6; }
        {
            uint32_t tmp = ws_rotl(a, 5) + f + e + k + w[i];
            e = d; d = cc; cc = ws_rotl(b, 30); b = a; a = tmp;
        }
    }

    c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d; c->h[4] += e;
}

void ws_sha1(const uint8_t *data, size_t len, uint8_t out[20])
{
    ws_sha1_ctx c;
    uint8_t fin[72];
    uint8_t b;
    size_t pad;

    c.h[0] = 0x67452301; c.h[1] = 0xEFCDAB89; c.h[2] = 0x98BADCFE;
    c.h[3] = 0x10325476; c.h[4] = 0xC3D2E1F0;
    c.bytes = 0; c.block_len = 0;

    while (len > 0) {
        size_t take = (64 - c.block_len < len) ? (64 - c.block_len) : len;
        memcpy(c.block + c.block_len, data, take);
        c.block_len += take; data += take; len -= take; c.bytes += take;
        if (c.block_len == 64) { ws_sha1_process_block(&c, c.block); c.block_len = 0; }
    }

    /* 填充：0x80 ... 0(补 64 位长) */
    fin[0] = 0x80;
    pad = (c.block_len < 56) ? (56 - c.block_len) : (120 - c.block_len);
    memset(&fin[1], 0, pad - 1 + 8);
    /* 写 64 位原始字节数(bit)，大端 */
    {
        uint64_t bits = (uint64_t)c.bytes * 8;
        int i;
        for (i = 0; i < 8; i++) {
            fin[1 + pad - 1 + i] = (uint8_t)(bits >> (56 - i * 8));
        }
    }
    for (b = 0; b < pad + 8; ) {
        size_t take = (64 - c.block_len < (size_t)(pad + 8) - b) ?
                      (64 - c.block_len) : ((size_t)(pad + 8) - b);
        memcpy(c.block + c.block_len, fin + b, take);
        c.block_len += take; b = (uint8_t)(b + take);
        if (c.block_len == 64) { ws_sha1_process_block(&c, c.block); c.block_len = 0; }
    }

    for (b = 0; b < 5; b++) {
        out[b * 4]     = (uint8_t)(c.h[b] >> 24);
        out[b * 4 + 1] = (uint8_t)(c.h[b] >> 16);
        out[b * 4 + 2] = (uint8_t)(c.h[b] >> 8);
        out[b * 4 + 3] = (uint8_t)(c.h[b]);
    }
}

/* ==================== Base64 ==================== */
static const char ws_b64_tab[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

size_t ws_base64_encode(const uint8_t *data, size_t len, char *out)
{
    size_t i, o = 0;
    for (i = 0; i + 2 < len; i += 3) {
        uint32_t v = ((uint32_t)data[i] << 16) | ((uint32_t)data[i + 1] << 8) | data[i + 2];
        out[o++] = ws_b64_tab[(v >> 18) & 0x3f];
        out[o++] = ws_b64_tab[(v >> 12) & 0x3f];
        out[o++] = ws_b64_tab[(v >> 6)  & 0x3f];
        out[o++] = ws_b64_tab[v        & 0x3f];
    }
    if (len - i == 1) {
        uint32_t v = (uint32_t)data[i] << 16;
        out[o++] = ws_b64_tab[(v >> 18) & 0x3f];
        out[o++] = ws_b64_tab[(v >> 12) & 0x3f];
        out[o++] = '='; out[o++] = '=';
    } else if (len - i == 2) {
        uint32_t v = ((uint32_t)data[i] << 16) | ((uint32_t)data[i + 1] << 8);
        out[o++] = ws_b64_tab[(v >> 18) & 0x3f];
        out[o++] = ws_b64_tab[(v >> 12) & 0x3f];
        out[o++] = ws_b64_tab[(v >> 6)  & 0x3f];
        out[o++] = '=';
    }
    return o;
}

char *ws_compute_accept(const char *client_key, char *accept)
{
    static const char magic[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    char concat[64];
    uint8_t digest[20];
    size_t n;

    strcpy(concat, client_key);
    strcat(concat, magic);
    ws_sha1((const uint8_t *)concat, strlen(concat), digest);
    n = ws_base64_encode(digest, 20, (char *)concat);
    concat[n] = '\0';
    strcpy(accept, concat);
    return accept;
}