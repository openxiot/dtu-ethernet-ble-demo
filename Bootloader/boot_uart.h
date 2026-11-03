/*********************************************************************
 * File Name          : boot_uart.h
 *
 * Description        : Bootloader 的 USART1（PA9 TX / PA10 RX）驱动。
 *                      只用于 IAP 二进制协议，不输出日志文本。
 *******************************************************************************/
#ifndef __BOOT_UART_H__
#define __BOOT_UART_H__

#include <stdint.h>

void     boot_uart_init(uint32_t baudrate);
void     boot_uart_send(const uint8_t *data, uint32_t len);
int      boot_uart_getc(uint8_t *c, uint32_t timeout_ms);   /* 0=成功，-1=超时 */
uint32_t boot_millis(void);
int      boot_millis_elapsed(uint32_t start, uint32_t ms);

#endif /* __BOOT_UART_H__ */
