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
static uint8_t           TransportDnsServer[4];     /* 记住 DNS 服务器，重连时复用 */
static uint8_t           TransportSocketId = 0xFF;
static volatile uint8_t  TransportOpened = 0;   /* TCP ESTABLISHED */

volatile uint32_t g_sys_ms = 0;   /* 系统毫秒节拍，TIM2 中断累加 */

/* 诊断用：TCP 层累计收发字节数（TLS 握手失败时打印） */
uint32_t g_tcp_tx_bytes;
uint32_t g_tcp_rx_bytes;

/* ==================== 网络状态日志 ==================== */
static int      link_up = -1;       /* -1=未知, 0=down, 1=up */
static uint32_t link_check_ms;      /* 上次查询 PHY 状态的时刻 */
static int      phase_prev = -1;    /* 上一阶段，用于记录进入时间 */
static uint32_t phase_ms;           /* 当前阶段开始时刻 */
static uint32_t hint_ms;            /* 上次周期提示时刻 */
static int      dhcp_started;       /* DHCP 是否已启动 */
static uint32_t dhcp_start_ms;      /* 本轮 DHCP 启动时刻 */

/* ==================== 工具 ==================== */
/* 前向声明（本文件内互相调用） */
static u8   dtu_dhcp_cb(u8 status, void *arg);
static void dtu_dns_cb(const char *name, u8 *ipaddr, void *arg);
static void transport_start_dns(u8 *dns_server_ip);

static const char *ts_name(int s)
{
    switch (s) {
    case TS_DHCP:    return "DHCP";
    case TS_DNS:     return "DNS";
    case TS_CONNECT: return "CONNECT";
    case TS_ONLINE:  return "ONLINE";
    case TS_FAIL:    return "RETRY-WAIT";
    default:         return "?";
    }
}

/* 启动/重启 DHCP（DHCP 客户端要在链路就绪后再启动，否则可能不发 DISCOVER） */
static void transport_dhcp_restart(const char *why)
{
    u8 ret;

    if (dhcp_started) WCHNET_DHCPStop();
    ret = WCHNET_DHCPStart(dtu_dhcp_cb);
    dhcp_started = (ret == WCHNET_ERR_SUCCESS);
    dhcp_start_ms = (uint32_t)g_sys_ms;
    printf("DHCP start (%s), ret=%02X\r\n", why, (unsigned)ret);
}

/* 链路 up/down 事件：down 时停 DHCP/关连接，up 时重新获取地址 */
static void transport_on_link_change(int up)
{
    if (!up) {
        TransportOpened = 0;
        if (TransportSocketId < WCHNET_MAX_SOCKET_NUM)
            WCHNET_SocketClose(TransportSocketId, TCP_CLOSE_ABANDON);
        TransportSocketId = 0xFF;      /* 失效 id，忽略关 socket 后迟到的中断 */
        if (dhcp_started) {
            WCHNET_DHCPStop();
            dhcp_started = 0;
        }
        if (DTU_USE_DHCP) ts = TS_DHCP;
        return;
    }

    if (DTU_USE_DHCP) {
        transport_dhcp_restart("link up");
        ts = TS_DHCP;
    } else {
        u8 sdns[4] = DTU_IP_DNS;
        transport_start_dns(sdns);
        ts = TS_DNS;
    }
}

/* 每秒查一次 PHY 链路，变化时打印；未上线时每 5s 打印当前阶段 */
static void transport_log_status(void)
{
    uint32_t now = (uint32_t)g_sys_ms;
    uint8_t phy;
    int up;

    if (ts != phase_prev) {          /* 阶段切换：重新计时 */
        phase_prev = ts;
        phase_ms = now;
        printf("[net] -> %s\r\n", ts_name(ts));
    }

    if ((uint32_t)(now - link_check_ms) < 1000u) return;
    link_check_ms = now;

    phy = WCHNET_GetPHYStatus();
    up = (phy & PHY_LINK_SUCCESS) ? 1 : 0;
    if (up != link_up) {
        link_up = up;
        printf("ETH link %s (phy=%02X)\r\n", up ? "UP" : "DOWN", (unsigned)phy);
        transport_on_link_change(up);
    }

    /* 链路已就绪但 DHCP 一直没有成功回调：重启 DHCP 客户端（启动失败也重试） */
    if (ts == TS_DHCP && link_up == 1 &&
        (uint32_t)(now - dhcp_start_ms) >= (uint32_t)DTU_DHCP_RETRY_MS) {
        transport_dhcp_restart("dhcp timeout");
    }

    if (ts != TS_ONLINE && (uint32_t)(now - hint_ms) >= 5000u) {
        hint_ms = now;
        printf("[net] %s, link=%s, phy=%02X, %lus\r\n", ts_name(ts),
               up ? "up" : "down", (unsigned)phy,
               (unsigned long)((now - phase_ms) / 1000u));
    }
}

static void transport_schedule_retry(void)
{
    if (reconnect_ms == 0) reconnect_ms = DTU_RECONNECT_MIN_MS;
    else if (reconnect_ms < DTU_RECONNECT_MAX_MS) reconnect_ms *= 2;
    if (reconnect_ms > DTU_RECONNECT_MAX_MS) reconnect_ms = DTU_RECONNECT_MAX_MS;
    ts_fail_at = (uint32_t)g_sys_ms + reconnect_ms;
    printf("retry in %lums\r\n", (unsigned long)reconnect_ms);
}

static void transport_print_ip(const char *tag, const uint8_t *ip)
{
    printf("%s %d.%d.%d.%d\r\n", tag, ip[0], ip[1], ip[2], ip[3]);
}

/* ==================== DNS ==================== */
static void transport_start_dns(u8 *dns_server_ip)
{
    u8 ret;

    if (dns_server_ip) memcpy(TransportDnsServer, dns_server_ip, 4);

    WCHNET_DNSStop();
    /* 重连时 dns_server_ip 为 NULL，用上次 DHCP/静态配置记住的服务器 */
    if (TransportDnsServer[0] | TransportDnsServer[1] |
        TransportDnsServer[2] | TransportDnsServer[3]) {
        WCHNET_InitDNS(TransportDnsServer, 53);
        printf("DNS query %s (server %d.%d.%d.%d)\r\n", DTU_HOST,
               TransportDnsServer[0], TransportDnsServer[1],
               TransportDnsServer[2], TransportDnsServer[3]);
    } else {
        printf("DNS query %s (no server!)\r\n", DTU_HOST);
    }

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
        TransportSocketId = 0xFF;
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
    if (ret != WCHNET_ERR_SUCCESS) {
        printf("tcp_read err %02X (id=%d avail=%lu)\r\n",
               (unsigned)ret, TransportSocketId, (unsigned long)avail);
        return -1;
    }
    g_tcp_rx_bytes += len;
    return (int)len;
}

int transport_tcp_write(const uint8_t *buf, uint32_t len)
{
    u32 l = len;
    u8 ret;

    if (!TransportOpened) return -1;
    ret = WCHNET_SocketSend(TransportSocketId, (u8 *)(void *)buf, &l);
    /* 发送缓冲暂时满（典型：TLS 握手一步连发 CKE/CCS/Finished 三条记录，
     * 而 WCHNET_NUM_TCP_SEG 只有 2 条、旧数据还没被 ACK）——不是断线，
     * 返回 0 让上层稍后重试，绝不能当 EOF 处理。 */
    if (ret == WCHNET_ERR_MEM) return 0;
    if (ret != WCHNET_ERR_SUCCESS) {
        printf("tcp_write err %02X (id=%d len=%lu sent=%lu)\r\n",
               (unsigned)ret, TransportSocketId,
               (unsigned long)len, (unsigned long)l);
        return -1;
    }
    g_tcp_tx_bytes += l;
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
        u8 dns[4] = DTU_IP_DNS;

        printf("DHCP OK: IP=%d.%d.%d.%d GW=%d.%d.%d.%d MASK=%d.%d.%d.%d DNS=%d.%d.%d.%d\r\n",
               p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7],
               p[8], p[9], p[10], p[11], p[12], p[13], p[14], p[15]);
        /* DNS 服务器用路由器下发的 DNS1(p[12..15])；个别路由器下发 0.0.0.0，回退配置值 */
        if (p[12] | p[13] | p[14] | p[15]) memcpy(dns, &p[12], 4);
        transport_start_dns(dns);
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
        g_tcp_tx_bytes = 0;
        g_tcp_rx_bytes = 0;
        TransportOpened = 1;
        reconnect_ms = 0;                      /* 成功后重置退避（下次失败从最短延时开始） */
        ts = TS_ONLINE;
        printf("TCP connected\r\n");
    }
    if (intstat & SINT_STAT_DISCONNECT) {
        printf("TCP disconnected\r\n");
        TransportOpened = 0;
        WCHNET_SocketClose(socketid, TCP_CLOSE_NORMAL);
        TransportSocketId = 0xFF;
        transport_schedule_retry();
        ts = TS_FAIL;
    }
    if (intstat & SINT_STAT_TIM_OUT) {
        printf("TCP timeout\r\n");
        TransportOpened = 0;
        WCHNET_SocketClose(socketid, TCP_CLOSE_RST);
        TransportSocketId = 0xFF;
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

    printf("ETH MAC %02X:%02X:%02X:%02X:%02X:%02X\r\n",
           TransportMAC[0], TransportMAC[1], TransportMAC[2],
           TransportMAC[3], TransportMAC[4], TransportMAC[5]);

    if (DTU_USE_DHCP) {
        u8 z[4] = {0, 0, 0, 0};
        ETH_LibInit(z, z, z, TransportMAC);     /* DHCP 会用自身地址覆写 */
        WCHNET_DHCPSetHostname(DTU_MODEL "-dtu");
        printf("DHCP armed (hostname %s-dtu), wait link\r\n", DTU_MODEL);
        ts = TS_DHCP;                           /* 链路 up 后由 transport_on_link_change 启动 */
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
    transport_log_status();

    /* 退避到点后重试：回到 DNS 解析→连接（地址可复用） */
    if (ts == TS_FAIL) {
        if ((uint32_t)g_sys_ms >= ts_fail_at) {
            TransportOpened = 0;
            if (TransportSocketId < WCHNET_MAX_SOCKET_NUM)
                WCHNET_SocketClose(TransportSocketId, TCP_CLOSE_NORMAL);
            TransportSocketId = 0xFF;
            transport_start_dns(NULL);
            ts = TS_DNS;
        }
    }
}

void transport_eth_reconnect(void)
{
    TransportOpened = 0;
    if (TransportSocketId < WCHNET_MAX_SOCKET_NUM)
        /* 用 NORMAL：发 FIN 走完四次挥手，服务端才能及时释放设备会话 */
        WCHNET_SocketClose(TransportSocketId, TCP_CLOSE_NORMAL);
    TransportSocketId = 0xFF;
    /* 交给退避定时器，避免 TLS/WS 立即失败时高速重试打爆服务端 */
    transport_schedule_retry();
    ts = TS_FAIL;
}

void transport_eth_reconnect_delay(uint32_t ms)
{
    TransportOpened = 0;
    if (TransportSocketId < WCHNET_MAX_SOCKET_NUM)
        WCHNET_SocketClose(TransportSocketId, TCP_CLOSE_NORMAL);
    TransportSocketId = 0xFF;
    ts_fail_at = (uint32_t)g_sys_ms + ms;
    printf("retry in %lums\r\n", (unsigned long)ms);
    ts = TS_FAIL;
}