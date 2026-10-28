/*********************************************************************
 * File Name          : dtu_app.c
 *
 * Description        : DTU 应用层实现。
 *
 * 协议行为（对齐服务端 DeviceYedS710YmProcessor）：
 *   - 心跳：每 DTU_HEARTBEAT_INTERVAL_S 秒发二进制帧 {0a 0b 0c 0d 0e 0f}
 *   - 信号强度：定时发文本帧 "csq=<随机值>"
 *   - 定位：定时发文本帧 "coordinates=经度,纬度"（第一个值是经度）
 *   - 收到查询帧 {01 02 03 04} 回 {55 aa 55 aa}
 *   - IMEI 已含在 URL 路径，无需单独报文。
 *******************************************************************************/
#include "dtu_app.h"
#include "dtu_config.h"
#include "transport_eth.h"
#include "wss_tls.h"
#include "ws_client.h"

#include <string.h>
#include <stdio.h>

/* ==================== 底层 I/O 桥接（ws_client 使用 wss_tls 字节流） ==================== */
static int tls_read_wrap(void *ctx, uint8_t *buf, uint32_t max) { (void)ctx; return wss_tls_read(buf, max); }
static int tls_write_wrap(void *ctx, const uint8_t *buf, uint32_t len) { (void)ctx; return wss_tls_write(buf, len); }
static const ws_io_t g_ws_io = { tls_read_wrap, tls_write_wrap, NULL };

static ws_client_t g_ws;

/* 心跳帧(服务端约定，勿改) */
static const uint8_t g_heartbeat[6] = DTU_HEARTBEAT_FRAME;
/* demo 专用：无 485，收到服务端任意请求帧就回这个固定帧（见 dtu_config.h 第 7 节） */
static const uint8_t g_demo_reply[] = DTU_DEMO_REPLY_FRAME;

static char g_ws_path[96];
static char g_port_str[8];

/* ==================== 简易伪随机（模拟用） ==================== */
static uint32_t g_rand_state;
static uint32_t app_rand(void)
{
    uint32_t x = g_rand_state;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    g_rand_state = x;
    return x;
}

/* ==================== 应用状态机 ==================== */
enum { ST_DOWN, ST_TLS, ST_WS, ST_OPEN };
static int g_st;
static uint32_t g_last_hb, g_last_csq, g_last_loc;
static int32_t g_lon = DTU_LON_BASE;
static int32_t g_lat = DTU_LAT_BASE;

/* ==================== WS 回调 ==================== */
static void on_ws_text(ws_client_t *ws, const uint8_t *data, uint32_t len)
{
    (void)ws;
    /* 服务端当前不发文本帧，仅打印便于调试 */
    printf("[recv text] %.*s\r\n", (int)len, (const char *)data);
}

static void on_ws_bin(ws_client_t *ws, const uint8_t *data, uint32_t len)
{
    uint32_t i;

    /* 打印收到的服务端请求帧 */
    printf("[recv req %u]: ", (unsigned)len);
    for (i = 0; i < len; i++) printf("%02x ", data[i]);
    printf("\r\n");

    /* 无 485：不论请求内容，一律回固定数据帧（模拟从设备应答） */
    if (ws_send_bin(ws, g_demo_reply, sizeof(g_demo_reply)) == 0) {
        printf("[send] demo reply %u bytes: ", (unsigned)sizeof(g_demo_reply));
        for (i = 0; i < sizeof(g_demo_reply); i++) printf("%02x ", g_demo_reply[i]);
        printf("\r\n");
    }
}

static void on_ws_close(ws_client_t *ws, uint16_t code)
{
    (void)ws;
    printf("[ws close] code=%u\r\n", code);
    g_st = ST_DOWN;
    wss_tls_close();
    transport_eth_reconnect();   /* 服务端关闭 -> 让传输层重连 */
}

/* ==================== 上报 ==================== */
static void report_csq(void)
{
    char buf[32];
    int csq;
    csq = DTU_CSQ_MIN + (int)(app_rand() % ((uint32_t)(DTU_CSQ_MAX - DTU_CSQ_MIN + 1)));
    snprintf(buf, sizeof(buf), "csq=%d", csq);
    if (ws_send_text(&g_ws, buf) == 0)
        printf("[send] %s\r\n", buf);
}

static void report_location(void)
{
    char buf[48];
    int32_t d, i;
    d = (int32_t)(app_rand() % ((uint32_t)(2 * DTU_LOC_JITTER))) - (int32_t)DTU_LOC_JITTER;
    i = (int32_t)(app_rand() % ((uint32_t)(2 * DTU_LOC_JITTER))) - (int32_t)DTU_LOC_JITTER;
    g_lon += d;
    g_lat += i;
    snprintf(buf, sizeof(buf), "coordinates=%d.%07d,%d.%07d",
             (int)(g_lon / 10000000), (int)(g_lon % 10000000),
             (int)(g_lat / 10000000), (int)(g_lat % 10000000));
    if (ws_send_text(&g_ws, buf) == 0)
        printf("[send] %s\r\n", buf);
}

/* ==================== 初始化 / 轮询 ==================== */
void dtu_app_init(void)
{
    static int done = 0;
    int n;

    if (done) return;
    done = 1;

    /* WebSocket 路径：/v1/{org}/{model}/{IMEI} */
    n = snprintf(g_ws_path, sizeof(g_ws_path), "%s%s/%s/%s",
                 DTU_WS_PATH_PREFIX, DTU_ORG_ID, DTU_MODEL, DTU_IMEI);
    if (n < 0 || n >= (int)sizeof(g_ws_path)) {
        printf("dtu path too long\r\n");
    }
    snprintf(g_port_str, sizeof(g_port_str), "%u", (unsigned)DTU_PORT);

    /* 随机种子：用目前系统节拍 + IMEI 尾数 */
    g_rand_state = 0x9E3779B9u ^ (uint32_t)g_sys_ms
                 ^ (uint32_t)(DTU_IMEI[0] | (DTU_IMEI[5] << 8) | (DTU_IMEI[9] << 16));

    ws_client_init(&g_ws, &g_ws_io, (uint32_t)g_sys_ms);
    g_ws.on_text  = on_ws_text;
    g_ws.on_bin   = on_ws_bin;
    g_ws.on_close = on_ws_close;

    g_st = ST_DOWN;
    g_last_hb = g_last_csq = g_last_loc = 0;

    printf("DTU ready -> ws%s://%s:%s%s\r\n",
           (DTU_TLS ? "s" : ""), DTU_HOST, g_port_str, g_ws_path);
}

void dtu_app_poll(void)
{
    uint32_t now = (uint32_t)g_sys_ms;

    if (!transport_is_online()) {
        if (g_st != ST_DOWN) {
            g_st = ST_DOWN;
            ws_client_close(&g_ws);
            wss_tls_close();            /* 释放 TLS 上下文/缓冲，避免重连泄漏 heap */
            printf("transport down, wait reconnect\r\n");
        }
        return;
    }

    if (g_st == ST_DOWN) {
        /* 传输在线：初始化 TLS，进入 TLS 握手阶段（合作式，逐轮轮询） */
        g_st = ST_TLS;
        wss_tls_init();
        return;
    }

    if (g_st == ST_TLS) {
        int tr = wss_tls_handshake();
        if (tr == 1) return;            /* TLS 握手进行中，下轮再试 */
        if (tr < 0) {
            printf("TLS handshake fail\r\n");
            wss_tls_close();
            transport_eth_reconnect();
            g_st = ST_DOWN;
            return;
        }
        g_st = ST_WS;                   /* TLS 完成，进入 WebSocket 握手 */
        g_ws.open = 0;
        g_ws.hs_state = 0;              /* 重置 WS 握手状态机 */
        return;
    }

    if (g_st == ST_WS) {
        int hr = ws_client_handshake(&g_ws, DTU_HOST, g_port_str, g_ws_path);
        if (hr == 0) {
            g_st = ST_OPEN;
            g_last_hb = g_last_csq = g_last_loc = now;
            printf("WebSocket open\r\n");
        } else if (hr < 0) {
            printf("WS handshake fail\r\n");
            wss_tls_close();
            transport_eth_reconnect();
            g_st = ST_DOWN;
        }
        return;   /* hr==1 进行中，下轮再试 */
    }

    if (g_st != ST_OPEN) return;

    /* OPEN：接收帧（含查询应答） */
    if (ws_client_poll(&g_ws) < 0) {
        /* 链路读出错：交给传输层重连 */
        wss_tls_close();
        transport_eth_reconnect();
        g_st = ST_DOWN;
        return;
    }

    /* 定时上报 */
    if ((uint32_t)(now - g_last_hb) >= (uint32_t)(DTU_HEARTBEAT_INTERVAL_S * 1000u)) {
        g_last_hb = now;
        ws_send_bin(&g_ws, g_heartbeat, sizeof(g_heartbeat));
    }
    if ((uint32_t)(now - g_last_csq) >= (uint32_t)(DTU_CSQ_INTERVAL_S * 1000u)) {
        g_last_csq = now;
        report_csq();
    }
    if ((uint32_t)(now - g_last_loc) >= (uint32_t)(DTU_LOC_INTERVAL_S * 1000u)) {
        g_last_loc = now;
        report_location();
    }
}