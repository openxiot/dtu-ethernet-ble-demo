/*********************************************************************
 * File Name          : dtu_app.h
 *
 * Description        : DTU 应用层。组装 WebSocket 客户端并驱动整条链路：
 *                       传输在线 → TLS/握手 → OPEN → 定时上报/心跳/应答。
 *                      上报参数都在 dtu_config.h 配置。
 *******************************************************************************/
#ifndef __DTU_APP_H__
#define __DTU_APP_H__

/* 初始化应用层（在 main 里、transport_eth_init 之后调用一次）。 */
void dtu_app_init(void);

/* 每轮主循环调用：驱动状态机、定时上报、处理接收帧。 */
void dtu_app_poll(void);

#endif /* __DTU_APP_H__ */