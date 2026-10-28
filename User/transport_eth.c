/*********************************************************************
 * File Name          : transport_eth.c
 *
 * Description        : 以太网传输层（WCHNET 协议栈集成）。
 *                      流程：ETH 初始化 → (DHCP 获取地址/DNS 服务器)
 *                            → DNS 解析 ws.dtu.ap.openxiot.cn
 *                            → TCP 连接 443 → ONLINE
 *                      断线/失败自动指数退避重连。
 *******************************************************************************/
#include "transport_eth.h"
#include "dtu_config.h"

#include "eth_driver.h"     /* 提供 WCHNET 封装：ETH_LibInit / SocketInf / TimeIsr 等 */
#include <string.h>
#include <stdio.h>

/* DHCP 回调状态 */
#define DHCP_CB_OK         0
#define DHCP_CB_FAIL       (~0u & 0xFF)

/* ==================== 传输子状态机 ==================== */
enum { TS_DHCP, TS_DNS, TS_CONNECT, TS_ONLINE, TS_FAIL };
static volatile int      ts;
static uint32_t          ts_fail_at;        /* g_sys_ms 时刻，到点重试 */
static uint32_t          reconnect_ms;      /* 当前退避值 */

/* 全局网络缓冲（对齐，供 WCHNET socket 接收） */
__attribute__((aligned(4))) uint8_t TransportMAC[6];
__attribute__((aligned(4))) uint8_t TransportRecvBuf[RECE_BUF_LEN];
static uint8_t           TransportRemoteIP[4];
static uint8_t           TransportSocketId = 0xFF;
static volatile uint8_t  TransportOpened = 0;   /* TCP ESTABLISHED */

volatile uint32_t g_sys_ms = 0;   /* 系统毫秒节拍，TIM2 中断累加 */

/* ==================== 工具 ==================== */
static void transport_schedule_retry(void)
{
    if (reconnect_ms == 0) reconnect_ms = DTU_RECONNECT_MIN_MS;
    else if (reconnect_ms < DTU_RECONNECT_MAX_MS) reconnect_ms *= 2;
    if (reconnect_ms > DTU_RECONNECT_MAX_MS) reconnect_ms = DTU_RECONNECT_MAX_MS;
    ts_fail_at = (uint32_t)g_sys_ms + reconnect_ms;
}

static void transport_print_ip(const char *tag, const uint8_t *ip)
{
    printf("%s %d.%d.%d.%d\r\n", tag, ip[0], ip[1], ip[2], ip[3]);
}

/* 前向声明（本文件内互相调用） */
static u8   dtu_dhcp_cb(u8 status, void *arg);
static void dtu_dns_cb(const char *name, u8 *ipaddr, void *arg);

/* ==================== DNS ==================== */
static void transport_start_dns(u8 *dns_server_ip)
{
    u8 ret;
    WCHNET_DNSStop();
    if (dns_server_ip) WCHNET_InitDNS(dns_server_ip, 53);
    ret = WCHNET_HostNameGetIp((char *)DTU_HOST, TransportRemoteIP,
                               dtu_dns_cb, NULL);
    if (ret != WCHNET_ERR_SUCCESS) {      /* DNS 立即失败 */
        transport_schedule_retry();
        ts = TS_FAIL;
    }
}

/* ==================== TCP 连接 ==================== */
static void transport_do_connect(void)
{
    SOCK_INF inf;
    u8 ret;

    memset(&inf, 0, sizeof(inf));
    memcpy(inf.IPAddr, TransportRemoteIP, 4);
    inf.DesPort   = DTU_PORT;
    inf.SourPort  = (u16)(1000u + (g_sys_ms & 0x0FFFFu));
    inf.ProtoType = PROTO_TYPE_TCP;
    inf.RecvBufLen= RECE_BUF_LEN;

    ret = WCHNET_SocketCreat(&TransportSocketId, &inf);
    if (ret != WCHNET_ERR_SUCCESS) {
        printf("SocketCreat fail %02X\r\n", (u8)ret);
        transport_schedule_retry();
        ts = TS_FAIL;
        return;
    }
    ret = WCHNET_SocketConnect(TransportSocketId);
    if (ret != WCHNET_ERR_SUCCESS) {
        printf("SocketConnect fail %02X\r\n", (u8)ret);
        WCHNET_SocketClose(TransportSocketId, TCP_CLOSE_ABANDON);
        transport_schedule_retry();
        ts = TS_FAIL;
    }
}

int transport_tcp_read(uint8_t *buf, uint32_t maxlen)
{
    u32 len, avail;
    u8 ret;

    if (!TransportOpened) return -1;
    avail = WCHNET_SocketRecvLen(TransportSocketId, NULL);
    if (avail == 0) return 0;
    if (avail > maxlen) avail = maxlen;
    len = avail;
    ret = WCHNET_SocketRecv(TransportSocketId, buf, &len);
    if (ret != WCHNET_ERR_SUCCESS) return -1;
    return (int)len;
}

int transport_tcp_write(const uint8_t *buf, uint32_t len)
{
    u32 l = len;
    u8 ret;

    if (!TransportOpened) return -1;
    ret = WCHNET_SocketSend(TransportSocketId, (u8 *)(void *)buf, &l);
    if (ret != WCHNET_ERR_SUCCESS) return -1;
    return (int)l;
}

int transport_is_online(void)
{
    return (int)TransportOpened;
}

/* ==================== 中断回调 ==================== */
/* DHCP 回调（status==0 成功，arg 指向租约：IP/GW/Mask/DNS1/DNS2） */
static u8 dtu_dhcp_cb(u8 status, void *arg)
{
    u8 *p = (u8 *)arg;

    if (!status) {
        printf("DHCP OK, IP = %d.%d.%d.%d\r\n", p[0], p[1], p[2], p[3]);
        /* DNS 服务器用路由器下发的 DNS1(p[12..15]) */
        transport_start_dns(&p[12]);
        ts = TS_DNS;
    } else {
        printf("DHCP fail %02X, retry by stack\r\n", (u8)status);
    }
    return DHCP_CB_OK;
}

/* DNS 回调：ipaddr!=NULL 表示解析成功 */
static void dtu_dns_cb(const char *name, u8 *ipaddr, void *arg)
{
    (void)name; (void)arg;

    if (ipaddr) {
        memcpy(TransportRemoteIP, ipaddr, 4);
        transport_print_ip("DNS OK ->", TransportRemoteIP);
        transport_do_connect();
        ts = TS_CONNECT;
    } else {
        printf("DNS fail, retry\r\n");
        transport_schedule_retry();
        ts = TS_FAIL;
    }
}

/* Socket 中断 */
static void handle_sockint(u8 socketid, u8 intstat)
{
    if (socketid != TransportSocketId) return;

    if (intstat & SINT_STAT_CONNECT) {
        /* 把 socket 接收缓冲指到应用缓冲（与 WCH 示例一致） */
        WCHNET_ModifyRecvBuf(socketid, (u32)(unsigned long)TransportRecvBuf, RECE_BUF_LEN);
        TransportOpened = 1;
        reconnect_ms = DTU_RECONNECT_MIN_MS;   /* 成功后重置退避 */
        ts = TS_ONLINE;
        printf("TCP connected\r\n");
    }
    if (intstat & SINT_STAT_DISCONNECT) {
        printf("TCP disconnected\r\n");
        TransportOpened = 0;
        WCHNET_SocketClose(socketid, TCP_CLOSE_ABANDON);
        transport_schedule_retry();
        ts = TS_FAIL;
    }
    if (intstat & SINT_STAT_TIM_OUT) {
        printf("TCP timeout\r\n");
        TransportOpened = 0;
        WCHNET_SocketClose(socketid, TCP_CLOSE_ABANDON);
        transport_schedule_retry();
        ts = TS_FAIL;
    }
}

static void handle_globalint(void)
{
    u8 g = WCHNET_GetGlobalInt();

    if (g & GINT_STAT_SOCKET) {
        u32 i;
        for (i = 0; i < WCHNET_MAX_SOCKET_NUM; i++) {
            u8 s = WCHNET_GetSocketInt((u8)i);
            if (s) handle_sockint((u8)i, s);
        }
    }
}

/* ==================== 初始化 / 轮询 / 重连 ==================== */
void transport_eth_init(void)
{
    const u8 custommac[6] = DTU_MAC;
    int use_custom_mac = 0;
    int i;

    memset(TransportRemoteIP, 0, sizeof(TransportRemoteIP));
    TransportOpened = 0;
    reconnect_ms = 0;

    WCHNET_GetMacAddr(TransportMAC);
    for (i = 0; i < 6; i++) if (custommac[i]) use_custom_mac = 1;
    if (use_custom_mac) memcpy(TransportMAC, custommac, 6);

    if (DTU_USE_DHCP) {
        u8 z[4] = {0, 0, 0, 0};
        ETH_LibInit(z, z, z, TransportMAC);     /* DHCP 会用自身地址覆写 */
        WCHNET_DHCPSetHostname("yed-s710ym-dtu");
        WCHNET_DHCPStart(dtu_dhcp_cb);
        ts = TS_DHCP;
    } else {
        u8 sip[4] = DTU_IP_STATIC, sgw[4] = DTU_IP_GW, smask[4] = DTU_IP_MASK;
        u8 sdns[4] = DTU_IP_DNS;
        ETH_LibInit(sip, sgw, smask, TransportMAC);
        printf("Static IP ");
        transport_print_ip("", sip);
        transport_start_dns(sdns);
        ts = TS_DNS;
    }
}

void transport_eth_poll(void)
{
    /* 驱动协议栈主任务并处理中断 */
    WCHNET_MainTask();
    if (WCHNET_QueryGlobalInt()) {
        handle_globalint();
    }

    /* 退避到点后重试：回到 DNS 解析→连接（地址可复用） */
    if (ts == TS_FAIL) {
        if ((uint32_t)g_sys_ms >= ts_fail_at) {
            TransportOpened = 0;
            if (TransportSocketId < WCHNET_MAX_SOCKET_NUM)
                WCHNET_SocketClose(TransportSocketId, TCP_CLOSE_ABANDON);
            transport_start_dns(NULL);
            ts = TS_DNS;
        }
    }
}

void transport_eth_reconnect(void)
{
    TransportOpened = 0;
    reconnect_ms = DTU_RECONNECT_MIN_MS;
    if (TransportSocketId < WCHNET_MAX_SOCKET_NUM)
        WCHNET_SocketClose(TransportSocketId, TCP_CLOSE_ABANDON);
    transport_start_dns(NULL);
    ts = TS_DNS;
}