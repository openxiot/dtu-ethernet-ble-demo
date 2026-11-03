/*********************************************************************
 * File Name          : ota_app.h
 *
 * Description        : App 侧 OTA：WS 通知解析、HTTPS 下载、整包校验、
 *                      写 Meta STAGED、启动确认与结果上报。
 *******************************************************************************/
#ifndef __OTA_APP_H__
#define __OTA_APP_H__

#include <stdint.h>

/* 上电初始化：读取 Meta，安排 30 s 启动确认。 */
void ota_app_init(void);

/* 解析并暂存 WS OTA 通知（T LV "DTUO"）。返回 OTA_ST_*。
 * OTA_ST_OK 时 dtu_app 应回复 "ota=starting"，并在回调返回后处理 pending。 */
int  ota_app_on_notify(const uint8_t *data, uint32_t len);

/* 1 = 已收到合法通知，需要 dtu_app 关闭 WS/TLS 后调用 ota_app_begin()。 */
int  ota_app_pending(void);

/* 1 = OTA 进行中，主循环只应调用 ota_app_poll()。 */
int  ota_app_busy(void);

/* 启动下载。调用前必须已 ws_client_close() + wss_tls_close()。 */
void ota_app_begin(void);

/* 推进状态机；返回 1 表示本设备仍在 OTA（含刚完成复位），0 表示空闲。 */
int  ota_app_poll(void);

/* App 请求下次进入串口升级：写 BOOT_UART 后复位，不返回。 */
int  ota_app_request_serial(void);

uint32_t ota_app_version(void);

/* 升级完成待上报（Meta flags.bit2）。 */
int  ota_app_report_pending(void);
void ota_app_report_done(void);

/* 上次失败的错误码（OTA_ST_*）。 */
int  ota_app_last_error(void);

/* 取出一次待上报的失败；没有返回 -1。 */
int  ota_app_take_failure(void);

#endif /* __OTA_APP_H__ */
