/**
 * @file  error.c
 * @brief 故障兜底实现 - 见 error.h 设计说明
 */

#include "stm32g0xx_hal.h"
#include "error.h"
#include "led.h"

/**
 * @note  HAL_Init 内部 `HAL_MspInit` 不直接调用此函数；只有应用层主动 Error_Handler()
 *        或 HAL_RCC_OscConfig / HAL_RCC_ClockConfig / HAL_RCCEx_PeriphCLKConfig
 *        失败时调用。后续如需差异化兜底（比如把错误码记到 RTC backup register
 *        再 NVIC_SystemReset 触发软复位），扩展点就在此函数内部。
 */
void Error_Handler(void)
{
    for (;;) {
        LED_Sys_Toggle();
        /* 16 MHz HSI 下，约 100 ms：每次 nop ≈ 1 cycle = 62.5 ns
         * 200000 cycle ≈ 12.5 ms，循环计入分支开销实际约 100 ms
         * 用 volatile 防止编译器把整个循环优化掉 */
        for (volatile uint32_t i = 0; i < 200000U; i++) {
            __NOP();
        }
    }
}

#ifdef USE_FULL_ASSERT
/**
 * @brief HAL assert 失败回调：把行号丢掉，直接进 Error_Handler
 *
 * @note  生产构建会通过 platformio.ini 关闭 USE_FULL_ASSERT，故此函数大部分时候
 *        不参与编译。需要排查时打开 -D USE_FULL_ASSERT 即可。
 */
void assert_failed(uint8_t *file, uint32_t line)
{
    (void)file;
    (void)line;
    Error_Handler();
}
#endif
