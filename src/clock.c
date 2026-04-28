/**
 * @file  clock.c
 * @brief 时钟配置实现 - 见 clock.h 设计说明
 *
 * STM32G0 时钟方案选型理由：
 *   1. 不需要外部晶振：HSI16 出厂 1% / HSI48 出厂 1%，配 CRS 校准后 USB 时钟
 *      实际偏差 <50 ppm，远好于 USB FS 协议要求（2500 ppm）。BOM 上省一颗晶振。
 *   2. SYSCLK = HSI16 而非 PLL 64MHz：本工程业务负载小，16 MHz 完全够用，
 *      代价是 FLASH 0WS、外设时钟简单、低功耗潜力大。需要更高分辨率的 PWM
 *      或更密集的 USB 流量时再考虑切 PLL。
 *   3. CRS 同步源 = USB SOF：USB 已枚举时主机每 1ms 发一次 SOF，CRS 拿这个
 *      1kHz 信号当 reference，反向调 HSI48.TRIM。无需任何外部信号。
 */

#include "stm32g0xx_hal.h"
#include "clock.h"
#include "error.h"

void Clock_Init(void)
{
    RCC_OscInitTypeDef       RCC_OscInitStruct = {0};
    RCC_ClkInitTypeDef       RCC_ClkInitStruct = {0};
    RCC_PeriphCLKInitTypeDef PeriphClkInit     = {0};
    RCC_CRSInitTypeDef       RCC_CRSInitStruct = {0};

    /* 1) HSI + HSI48 都开
     *    HSI 是 SYSCLK 时钟源；HSI48 仅供 USB，二者完全独立 */
    RCC_OscInitStruct.OscillatorType      = RCC_OSCILLATORTYPE_HSI
                                          | RCC_OSCILLATORTYPE_HSI48;
    RCC_OscInitStruct.HSIState            = RCC_HSI_ON;
    RCC_OscInitStruct.HSIDiv              = RCC_HSI_DIV1;
    RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
    RCC_OscInitStruct.HSI48State          = RCC_HSI48_ON;
    RCC_OscInitStruct.PLL.PLLState        = RCC_PLL_NONE;
    if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK) {
        Error_Handler();
    }

    /* 2) SYSCLK = HSI16，AHB/APB 不分频，FLASH 0WS */
    RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK
                                | RCC_CLOCKTYPE_SYSCLK
                                | RCC_CLOCKTYPE_PCLK1;
    RCC_ClkInitStruct.SYSCLKSource   = RCC_SYSCLKSOURCE_HSI;
    RCC_ClkInitStruct.AHBCLKDivider  = RCC_SYSCLK_DIV1;
    RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV1;
    if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_0) != HAL_OK) {
        Error_Handler();
    }

    /* 3) USB 外设时钟源 = HSI48
     *    ⚠ STM32G0 上 HSI48 不能作为 SYSCLK，只能给 USB；二者完全分离 */
    PeriphClkInit.PeriphClockSelection = RCC_PERIPHCLK_USB;
    PeriphClkInit.UsbClockSelection    = RCC_USBCLKSOURCE_HSI48;
    if (HAL_RCCEx_PeriphCLKConfig(&PeriphClkInit) != HAL_OK) {
        Error_Handler();
    }

    /* 4) CRS：USB SOF 自动校准 HSI48
     *    SyncSource = USB（SOF 帧周期 = 0xBB7F+1 = 48000 个 HSI48 周期）
     *    Polarity   = Rising（SOF 信号上升沿当作同步触发）
     *    配置后无需轮询 CRS_SR，硬件全自动；要看实时状态见 crs_diag.c */
    __HAL_RCC_CRS_CLK_ENABLE();
    RCC_CRSInitStruct.Prescaler             = RCC_CRS_SYNC_DIV1;
    RCC_CRSInitStruct.Source                = RCC_CRS_SYNC_SOURCE_USB;
    RCC_CRSInitStruct.Polarity              = RCC_CRS_SYNC_POLARITY_RISING;
    RCC_CRSInitStruct.ReloadValue           = RCC_CRS_RELOADVALUE_DEFAULT;
    RCC_CRSInitStruct.ErrorLimitValue       = RCC_CRS_ERRORLIMIT_DEFAULT;
    RCC_CRSInitStruct.HSI48CalibrationValue = RCC_CRS_HSI48CALIBRATION_DEFAULT;
    HAL_RCCEx_CRSConfig(&RCC_CRSInitStruct);
}
