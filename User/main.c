/********************************** (C) COPYRIGHT *******************************
 * File Name          : main.c
 * Description        : DTU 主程序。上电后经以太网以 wss 连接
 *                      ws.dtu.ap.openxiot.cn/v1/{org}/{model}/{IMEI}，
 *                      模拟 yeds710ym 设备：心跳 / csq / coordinates 上报、查询应答。
 *
 * 说明：以太网要求系统时钟为 60MHz 或 120MHz（见 system_ch32v20x.c 已设为 120MHz）。
 *********************************************************************************
 * Copyright (c) 2021 Nanjing Qinheng Microelectronics Co., Ltd.
 *******************************************************************************/

#include "debug.h"
#include "eth_driver.h"
#include "transport_eth.h"
#include "dtu_app.h"
#include "ota_app.h"

/*********************************************************************
 * @fn      TIM2_Init
 *
 * @brief   初始化 TIM2 作为 WCHNET 协议栈定时节拍（WCHNETTIMERPERIOD 毫秒一次）。
 *
 * @return  none
 */
void TIM2_Init(void)
{
    TIM_TimeBaseInitTypeDef TIM_TimeBaseStructure = { 0 };

    RCC_APB1PeriphClockCmd(RCC_APB1Periph_TIM2, ENABLE);

    TIM_TimeBaseStructure.TIM_Period = SystemCoreClock / 1000000;
    TIM_TimeBaseStructure.TIM_Prescaler = WCHNETTIMERPERIOD * 1000 - 1;
    TIM_TimeBaseStructure.TIM_ClockDivision = 0;
    TIM_TimeBaseStructure.TIM_CounterMode = TIM_CounterMode_Up;
    TIM_TimeBaseInit(TIM2, &TIM_TimeBaseStructure);
    TIM_ITConfig(TIM2, TIM_IT_Update, ENABLE);

    TIM_Cmd(TIM2, ENABLE);
    TIM_ClearITPendingBit(TIM2, TIM_IT_Update);
    NVIC_EnableIRQ(TIM2_IRQn);
}

/*********************************************************************
 * @fn      reset_reason
 *
 * @brief   读取 RCC 复位标志，打印上次复位原因（协助定位异常重启）。
 *          NVIC_SystemReset（含 HardFault_Handler）会留下 SFTRST 标志。
 *
 * @return  原因字符串
 */
static const char *reset_reason(void)
{
    if (RCC_GetFlagStatus(RCC_FLAG_PORRST)  != RESET) return "power-on";
    if (RCC_GetFlagStatus(RCC_FLAG_PINRST)  != RESET) return "reset pin";
    if (RCC_GetFlagStatus(RCC_FLAG_SFTRST)  != RESET) return "software(HardFault/WCH-Link)";
    if (RCC_GetFlagStatus(RCC_FLAG_IWDGRST) != RESET) return "IWDG";
    if (RCC_GetFlagStatus(RCC_FLAG_WWDGRST) != RESET) return "WWDG";
    if (RCC_GetFlagStatus(RCC_FLAG_LPWRRST) != RESET) return "low-power";
    return "unknown";
}

/*********************************************************************
 * @fn      main
 *
 * @brief   Main program body.
 *
 * @return  none
 */
int main(void)
{
    Delay_Init();
    USART_Printf_Init(115200);

    SystemCoreClockUpdate();
    printf("DTU boot, SystemClk:%d\r\n", (uint32_t)SystemCoreClock);
    printf("reset: %s\r\n", reset_reason());
    RCC_ClearFlag();
    if ((SystemCoreClock != 120000000) && (SystemCoreClock != 60000000)) {
        printf("WARN: 以太网要求 60/120MHz，请检查 system_ch32v20x.c 时钟配置\r\n");
    }

    TIM2_Init();
    transport_eth_init();
    ota_app_init();
    dtu_app_init();

    while (1) {
        transport_eth_poll();   /* WCHNET 主任务 + 中断 + DHCP/DNS/TCP/重连状态机 */
        dtu_app_poll();         /* TLS/WS 握手 + 心跳/csq/coordinates 上报 + 查询应答 */
    }
}