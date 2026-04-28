/**
 * @file  stm32g0xx_it.c
 * @brief 中断处理：核心异常 + USB 共享中断
 */

#include "stm32g0xx_hal.h"

extern PCD_HandleTypeDef hpcd_USB_DRD_FS;
extern TIM_HandleTypeDef htim3;

void NMI_Handler(void)        { while (1) {} }
void HardFault_Handler(void)  { while (1) {} }
void SVC_Handler(void)        {}
void PendSV_Handler(void)     {}
void SysTick_Handler(void)    { HAL_IncTick(); }

/* G0B1 上 USB 与 UCPD1/2 共享同一个中断向量 */
void USB_UCPD1_2_IRQHandler(void)
{
    HAL_PCD_IRQHandler(&hpcd_USB_DRD_FS);
}

void TIM3_TIM4_IRQHandler(void)
{
    HAL_TIM_IRQHandler(&htim3);
}
