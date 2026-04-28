/**
 * @file  pwm.h
 * @brief PWM 输出模块（TIM1）- 25 kHz 4-pin 风扇控制
 *
 * 约束/约定：
 *   - 目标频率固定 25 kHz（4-pin 风扇推荐值）
 *   - duty 范围 0~100（百分比），内部做饱和
 *   - 初始化后默认不自动设置 duty：由 fanctl 决策后调用 Pwm_SetDuty
 *
 * 硬件映射（已定板，严禁更改）：
 *   - PA8  -> TIM1_CH1 -> FAN1_PWM
 *   - PA9  -> TIM1_CH2 -> FAN2_PWM
 */
#ifndef PWM_H
#define PWM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PWM_CH1 = 1,
    PWM_CH2 = 2,
} pwm_channel_t;

/**
 * @brief 初始化 TIM1 PWM 输出（25 kHz）并启动 CH1/CH2
 */
void Pwm_Init(void);

/**
 * @brief 设置指定通道 duty（0~100%）
 *
 * @param ch   PWM_CH1 / PWM_CH2
 * @param pct  百分比（超界会被饱和到 [0,100]）
 */
void Pwm_SetDuty(pwm_channel_t ch, uint8_t pct);

#ifdef __cplusplus
}
#endif

#endif /* PWM_H */

