/*********************************************************************
 * File Name          : ota_config.h
 *
 * Description        : OTA 编译期配置（App 与 Bootloader 共用）。
 *******************************************************************************/
#ifndef __OTA_CONFIG_H__
#define __OTA_CONFIG_H__

#include "ota_layout.h"

/* 机型 / 硬件版本哈希；固件包与设备必须一致。 */
#define OTA_HW_ID               0x00000001u

/* 当前固件版本（用于版本上报与降级判断）。 */
#define OTA_FW_VERSION          OTA_MAKE_VERSION(0, 1, 0)
#define OTA_BL_VERSION          OTA_MAKE_VERSION(0, 1, 0)

/* 串口 IAP */
#define OTA_UART_BAUD           115200u
#define OTA_BOOT_WINDOW_MS      500u
#define OTA_BOOT_UART_WAIT_MS   30000u
#define OTA_UART_FRAME_TIMEOUT  300u

/* 启动确认 */
#define OTA_APP_CONFIRM_MS      30000u
#define OTA_MAX_ATTEMPTS        5u

#endif /* __OTA_CONFIG_H__ */
