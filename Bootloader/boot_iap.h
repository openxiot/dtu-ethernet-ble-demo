/*********************************************************************
 * File Name          : boot_iap.h
 *
 * Description        : 串口 IAP 协议（USART1，A5 5A 帧）。
 *******************************************************************************/
#ifndef __BOOT_IAP_H__
#define __BOOT_IAP_H__

#include <stdint.h>

/* 等待 HELLO 并进入升级会话。wait_ms == 0xFFFFFFFF 表示无限等待。
 * 返回： 0 会话被 ABORT 结束； 1 收到 RESET；
 *       -1 未收到 HELLO 超时； -2 搬运/校验失败（已写 RECOVERY）。
 * 校验通过并搬运完成后，本函数直接跳转 App，不返回。 */
int boot_iap_run(uint32_t wait_ms);

#define OTA_IAP_WAIT_FOREVER    0xFFFFFFFFu

#endif /* __BOOT_IAP_H__ */
