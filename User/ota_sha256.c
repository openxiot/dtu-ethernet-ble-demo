/*********************************************************************
 * File Name          : ota_sha256.c
 *
 * Description        : 精简 SHA-256 实现（无动态内存、无查表）。
 *******************************************************************************/
#include "ota_sha256.h"
#include <string.h>

static const uint32_t K[64] = {
    0x428A2F98u, 0x71374491u, 0xB5C0FBCFu, 0xE9B5DBA5u,
    0x3956C25Bu, 0x59F111F1u, 0x923F82A4u, 0xAB1C5ED5u,
    0xD807AA98u, 0x12835B01u, 0x243185BEu, 0x550C7DC3u,
    0x72BE5D74u, 0x80DEB1FEu, 0x9BDC06A7u, 0xC19BF174u,
    0xE49B69C1u, 0xEFBE4786u, 0x0FC19DC6u, 0x240CA1CCu,
    0x2DE92C6Fu, 0x4A7484AAu, 0x5CB0A9DCu, 0x76F988DAu,
    0x983E5152u, 0xA831C66Du, 0xB00327C8u, 0xBF597FC7u,
    0xC6E00BF3u, 0xD5A79147u, 0x06CA6351u, 0x14292967u,
    0x27B70A85u, 0x2E1B2138u, 0x4D2C6DFCu, 0x53380D13u,
    0x650A7354u, 0x766A0ABBu, 0x81C2C92Eu, 0x92722C85u,
    0xA2BFE8A1u, 0xA81A664Bu, 0xC24B8B70u, 0xC76C51A3u,
    0xD192E819u, 0xD6990624u, 0xF40E3585u, 0x106AA070u,
    0x19A4C116u, 0x1E376C08u, 0x2748774Cu, 0x34B0BCB5u,
    0x391C0CB3u, 0x4ED8AA4Au, 0x5B9CCA4Fu, 0x682E6FF3u,
    0x748F82EEu, 0x78A5636Fu, 0x84C87814u, 0x8CC70208u,
    0x90BEFFFAu, 0xA4506CEBu, 0xBEF9A3F7u, 0xC67178F2u
};

#define ROTR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha256_block(ota_sha256_ctx *ctx, const uint8_t *p)
{
    uint32_t w[64];
    uint32_t a, b, c, d, e, f, g, h;
    uint32_t i, t1, t2;

    for (i = 0; i < 16; i++) {
        w[i] = ((uint32_t)p[i * 4 + 0] << 24) |
               ((uint32_t)p[i * 4 + 1] << 16) |
               ((uint32_t)p[i * 4 + 2] << 8)  |
               ((uint32_t)p[i * 4 + 3]);
    }
    for (i = 16; i < 64; i++) {
        uint32_t s0 = ROTR(w[i - 15], 7) ^ ROTR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ROTR(w[i - 2], 17) ^ ROTR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    a = ctx->h[0]; b = ctx->h[1]; c = ctx->h[2]; d = ctx->h[3];
    e = ctx->h[4]; f = ctx->h[5]; g = ctx->h[6]; h = ctx->h[7];

    for (i = 0; i < 64; i++) {
        uint32_t S1 = ROTR(e, 6) ^ ROTR(e, 11) ^ ROTR(e, 25);
        uint32_t ch = (e & f) ^ ((~e) & g);
        uint32_t S0 = ROTR(a, 2) ^ ROTR(a, 13) ^ ROTR(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);

        t1 = h + S1 + ch + K[i] + w[i];
        t2 = S0 + maj;
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }

    ctx->h[0] += a; ctx->h[1] += b; ctx->h[2] += c; ctx->h[3] += d;
    ctx->h[4] += e; ctx->h[5] += f; ctx->h[6] += g; ctx->h[7] += h;
}

void ota_sha256_init(ota_sha256_ctx *ctx)
{
    ctx->h[0] = 0x6A09E667u; ctx->h[1] = 0xBB67AE85u;
    ctx->h[2] = 0x3C6EF372u; ctx->h[3] = 0xA54FF53Au;
    ctx->h[4] = 0x510E527Fu; ctx->h[5] = 0x9B05688Cu;
    ctx->h[6] = 0x1F83D9ABu; ctx->h[7] = 0x5BE0CD19u;
    ctx->total = 0;
    ctx->used = 0;
}

void ota_sha256_update(ota_sha256_ctx *ctx, const void *data, uint32_t len)
{
    const uint8_t *p = (const uint8_t *)data;

    ctx->total += len;

    if (ctx->used) {
        uint32_t n = 64 - ctx->used;
        if (n > len) n = len;
        memcpy(ctx->buf + ctx->used, p, n);
        ctx->used += n;
        p += n;
        len -= n;
        if (ctx->used == 64) {
            sha256_block(ctx, ctx->buf);
            ctx->used = 0;
        }
    }

    while (len >= 64) {
        sha256_block(ctx, p);
        p += 64;
        len -= 64;
    }

    if (len) {
        memcpy(ctx->buf, p, len);
        ctx->used = len;
    }
}

void ota_sha256_final(ota_sha256_ctx *ctx, uint8_t out[32])
{
    uint64_t bits = ctx->total << 3;
    uint32_t i;

    ctx->buf[ctx->used++] = 0x80;
    if (ctx->used > 56) {
        while (ctx->used < 64) ctx->buf[ctx->used++] = 0;
        sha256_block(ctx, ctx->buf);
        ctx->used = 0;
    }
    while (ctx->used < 56) ctx->buf[ctx->used++] = 0;

    for (i = 0; i < 8; i++)
        ctx->buf[56 + i] = (uint8_t)(bits >> (56 - i * 8));
    sha256_block(ctx, ctx->buf);

    for (i = 0; i < 8; i++) {
        out[i * 4 + 0] = (uint8_t)(ctx->h[i] >> 24);
        out[i * 4 + 1] = (uint8_t)(ctx->h[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(ctx->h[i] >> 8);
        out[i * 4 + 3] = (uint8_t)(ctx->h[i]);
    }
}

void ota_sha256(const void *data, uint32_t len, uint8_t out[32])
{
    ota_sha256_ctx ctx;

    ota_sha256_init(&ctx);
    ota_sha256_update(&ctx, data, len);
    ota_sha256_final(&ctx, out);
}
