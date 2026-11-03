/*********************************************************************
 * File Name          : boot_uart.c
 *
 * Description        : USART1 轮询收发 + SysTick 毫秒计时。
 *******************************************************************************/
#include "boot_uart.h"
#include "ch32v20x.h"

static volatile uint32_t s_ms;

void SysTick_Handler(void) __attribute__((interrupt("WCH-Interrupt-fast")));
void SysTick_Handler(void)
{
    SysTick->SR = 0;
    s_ms++;
}

uint32_t boot_millis(void)
{
    return s_ms;
}

int boot_millis_elapsed(uint32_t start, uint32_t ms)
{
    return (uint32_t)(s_ms - start) >= ms;
}

void boot_uart_init(uint32_t baudrate)
{
    GPIO_InitTypeDef  gpio;
    USART_InitTypeDef uart;

    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA | RCC_APB2Periph_USART1, ENABLE);

    gpio.GPIO_Pin   = GPIO_Pin_9;
    gpio.GPIO_Speed = GPIO_Speed_50MHz;
    gpio.GPIO_Mode  = GPIO_Mode_AF_PP;
    GPIO_Init(GPIOA, &gpio);

    gpio.GPIO_Pin   = GPIO_Pin_10;
    gpio.GPIO_Speed = GPIO_Speed_50MHz;
    gpio.GPIO_Mode  = GPIO_Mode_IPU;        /* 空闲为高，避免悬空误触发 */
    GPIO_Init(GPIOA, &gpio);

    uart.USART_BaudRate            = baudrate;
    uart.USART_WordLength          = USART_WordLength_8b;
    uart.USART_StopBits            = USART_StopBits_1;
    uart.USART_Parity              = USART_Parity_No;
    uart.USART_HardwareFlowControl = USART_HardwareFlowControl_None;
    uart.USART_Mode                = USART_Mode_Tx | USART_Mode_Rx;
    USART_Init(USART1, &uart);
    USART_Cmd(USART1, ENABLE);

    /* SysTick 1 ms */
    SysTick->CTLR = 0;
    SysTick->SR   = 0;
    SysTick->CNT  = 0;
    SysTick->CMP  = SystemCoreClock / 1000u - 1u;
    NVIC_EnableIRQ(SysTick_IRQn);
    SysTick->CTLR = 0x0F;
}

void boot_uart_send(const uint8_t *data, uint32_t len)
{
    while (len--) {
        while (USART_GetFlagStatus(USART1, USART_FLAG_TC) == RESET)
            ;
        USART_SendData(USART1, *data++);
    }
}

int boot_uart_getc(uint8_t *c, uint32_t timeout_ms)
{
    uint32_t start = s_ms;

    for (;;) {
        if (USART_GetFlagStatus(USART1, USART_FLAG_RXNE) != RESET) {
            *c = (uint8_t)USART_ReceiveData(USART1);
            return 0;
        }
        if ((uint32_t)(s_ms - start) >= timeout_ms)
            return -1;
    }
}
