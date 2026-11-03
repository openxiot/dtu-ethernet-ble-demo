/*********************************************************************
 * File Name          : transport_eth.h
 *
 * Description        : 以太网传输层（WCHNET 协议栈集成）。
 *                      负责：初始化 / DHCP / DNS 解析 / TCP 连接与收发，
 *                      断线自动按指数退避重连。
 *                      对上层（wss_tls / dtu_app）只暴露：
 *                        - transport_is_online()  链路是否在建
 *                        - transport_tcp_read/write 原始 TCP 字节流
 *******************************************************************************/
#ifndef __TRANSPORT_ETH_H__
#define __TRANSPORT_ETH_H__

#include <stdint.h>

/* 系统毫秒节拍：由 TIM2 中断累加（DTU_TICK_PERIOD_MS）。 */
extern volatile uint32_t g_sys_ms;

/* TCP 是否 ESTABLISHED（可供 TLS/WS 层使用）。 */
int transport_is_online(void);

/* 初始化网络栈（ETH + DHCP/静态IP），随后在 main 循环每轮调用 transport_eth_poll()。 */
void transport_eth_init(void);

/* 每轮主循环调用：驱动 WCHNET 主任务、处理中断、推进重连状态机。 */
void transport_eth_poll(void);

/* 原始 TCP 字节流（非阻塞）。
 * transport_tcp_read : 返回读取字节数；无数据 0；未连接/出错 <0。
 * transport_tcp_write: 返回写入字节数；未连接/出错 <0。 */
int transport_tcp_read(uint8_t *buf, uint32_t maxlen);
int transport_tcp_write(const uint8_t *buf, uint32_t len);

/* 立即触发一次重连（回到 DNS→连接，按指数退避）。 */
void transport_eth_reconnect(void);

/* 触发重连，但等 ms 毫秒后再重试（用于服务端重复会话等需等待的场景）。 */
void transport_eth_reconnect_delay(uint32_t ms);

/* OTA 专用：把单 socket 切到固件服务器。
 * 调用前必须已关闭 WS/TLS；host 为 NULL 时直接失败。
 * 连接期间 transport_tcp_read/write 照常使用；失败重试也指向该主机。 */
int  transport_eth_ota_start(const char *host, uint16_t port);

/* 结束 OTA：回到 DTU_HOST 并重新连接 WS。 */
void transport_eth_ota_stop(void);

/* 当前是否处于 OTA 目标模式。 */
int  transport_eth_is_ota(void);

#endif /* __TRANSPORT_ETH_H__ */