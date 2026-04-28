/**
 * @file  pwm.c
 * @brief PWM 输出模块实现（TIM1 CH1/CH2）
 *
 * 设计目标：
 *   - 生成 25 kHz、0~100% 可调的推挽 PWM 输出，给 4-pin 风扇 PWM 线使用
 *   - 不依赖 ISR；duty 变更是主循环里写 CCR 寄存器完成
 *
 * 关键点：
 *   - TIM1 是高级定时器，但这里用最简单的 edge-aligned PWM1 模式
 *   - 计数时钟使用 TIM1 的输入时钟（默认来自 APB2）。本项目 SYSCLK=HSI16=16MHz，
 *     APB2 不分频时 TIM1CLK 也是 16MHz。
 *   - 25kHz => Period = 16MHz / 25kHz = 640 个 tick
 *     ARR = 640-1 = 639
 */

#include "pwm.h"

#include "stm32g0xx_hal.h"

/* =========================================================================== */
/* 编译期参数 */
/* =========================================================================== */

#define PWM_TARGET_FREQ_HZ   25000UL
#define PWM_TIMER_HZ         16000000UL   /* SYSCLK=16MHz，且 APB2 不分频 */

/* =========================================================================== */
/* 内部状态 */
/* =========================================================================== */

static TIM_HandleTypeDef htim1;
static uint32_t          tim1_period;     /* ARR+1，作为 duty 换算基准 */

/* =========================================================================== */
/* 内部工具 */
/* =========================================================================== */

static uint32_t pwm_pct_to_ccr(uint8_t pct)
{
    if (pct >= 100U) {
        return tim1_period;
    }
    /* 使用 32-bit 防止乘法溢出；四舍五入避免低占空比全部变 0 */
    return (uint32_t)(((uint32_t)pct * tim1_period + 50U) / 100U);
}

/* =========================================================================== */
/* 公共接口 */
/* =========================================================================== */

void Pwm_Init(void)
{
    /* 1) GPIO：PA8/PA9 复用到 TIM1_CH1/CH2 */
    __HAL_RCC_GPIOA_CLK_ENABLE();
    GPIO_InitTypeDef gi = {0};
    gi.Pin       = GPIO_PIN_8 | GPIO_PIN_9;
    gi.Mode      = GPIO_MODE_AF_PP;
    gi.Pull      = GPIO_NOPULL;
    gi.Speed     = GPIO_SPEED_FREQ_HIGH;
    gi.Alternate = GPIO_AF2_TIM1;
    HAL_GPIO_Init(GPIOA, &gi);

    /* 2) TIM1 基本配置 */
    __HAL_RCC_TIM1_CLK_ENABLE();

    uint32_t period = (PWM_TIMER_HZ / PWM_TARGET_FREQ_HZ);
    if (period < 2U) {
        period = 2U; /* 防御：避免 ARR=0 导致 PWM 不可用 */
    }
    tim1_period = period; /* ARR+1 */

    htim1.Instance               = TIM1;
    htim1.Init.Prescaler         = 0U;
    htim1.Init.CounterMode       = TIM_COUNTERMODE_UP;
    htim1.Init.Period            = period - 1U;
    htim1.Init.ClockDivision     = TIM_CLOCKDIVISION_DIV1;
    htim1.Init.RepetitionCounter = 0U;
    htim1.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
    (void)HAL_TIM_PWM_Init(&htim1);

    TIM_OC_InitTypeDef oc = {0};
    oc.OCMode       = TIM_OCMODE_PWM1;
    oc.Pulse        = 0U;
    oc.OCPolarity   = TIM_OCPOLARITY_HIGH;
    oc.OCNPolarity  = TIM_OCNPOLARITY_HIGH;
    oc.OCFastMode   = TIM_OCFAST_DISABLE;
    oc.OCIdleState  = TIM_OCIDLESTATE_RESET;
    oc.OCNIdleState = TIM_OCNIDLESTATE_RESET;
    (void)HAL_TIM_PWM_ConfigChannel(&htim1, &oc, TIM_CHANNEL_1);
    (void)HAL_TIM_PWM_ConfigChannel(&htim1, &oc, TIM_CHANNEL_2);

    /* 3) 启动 PWM 输出 */
    (void)HAL_TIM_PWM_Start(&htim1, TIM_CHANNEL_1);
    (void)HAL_TIM_PWM_Start(&htim1, TIM_CHANNEL_2);

    /* 默认 duty 留给上层仲裁；此处清零避免上电瞬间乱脉冲 */
    __HAL_TIM_SET_COMPARE(&htim1, TIM_CHANNEL_1, 0U);
    __HAL_TIM_SET_COMPARE(&htim1, TIM_CHANNEL_2, 0U);
}

void Pwm_SetDuty(pwm_channel_t ch, uint8_t pct)
{
    uint32_t ccr = pwm_pct_to_ccr(pct);

    switch (ch) {
    case PWM_CH1:
        __HAL_TIM_SET_COMPARE(&htim1, TIM_CHANNEL_1, ccr);
        break;
    case PWM_CH2:
        __HAL_TIM_SET_COMPARE(&htim1, TIM_CHANNEL_2, ccr);
        break;
    default:
        break;
    }
}

