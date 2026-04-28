/**
 * @file  clock.h
 * @brief 系统时钟配置（HSI16 SYSCLK + HSI48 USB + CRS）
 *
 * 设计目标：
 *   - SYSCLK 16 MHz（HSI16，FLASH 0WS，足够 LED + USB + 后续 PWM/UART 业务）
 *   - USBCLK 48 MHz（HSI48，CRS 用 USB SOF 自动校准到 < 0.25%）
 *   - 不依赖外部晶振（节省 BOM、减少布线）
 *
 * 后续若需要切换时钟方案（例如改 PLL 64MHz 提高 PWM 分辨率），只在
 * clock.c 内部修改即可，对其他模块零侵入。
 */
#ifndef CLOCK_H
#define CLOCK_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 一次性配置系统时钟树并启动 CRS USB 同步校准
 *
 *        必须在 HAL_Init 之后、其他外设初始化之前调用。
 *        失败会进 Error_Handler（永不返回）。
 *
 * @note  当前 SYSCLK = HSI16 = 16 MHz；APB1 = HCLK = 16 MHz；
 *        若以后改成 PLL 升频，注意：
 *          1. 同步调整 FLASH wait state（HSI64 需要 LATENCY_2）
 *          2. CRS 仍走独立的 HSI48 路径，不受 SYSCLK 影响
 *          3. SysTick 由 HAL 自动按 SystemCoreClock 重配，无需额外操作
 */
void Clock_Init(void);

#ifdef __cplusplus
}
#endif

#endif /* CLOCK_H */
