/**
 * @file  tach.c
 * @brief 风扇 TACH 测速模块实现（TIM3 输入捕获）
 *
 * 实现策略：
 *   - TIM3 计数频率设为 1 MHz（PSC=15，16MHz/16=1MHz），方便把 tick 直接当作 us
 *   - 同时使能 Update 中断（溢出），用 ovf 计数扩展为 32-bit 时间戳：
 *       ext = (ovf << 16) | cnt16
 *   - 每次捕获（CC1/CC2）时计算 dt = ext_now - ext_last（天然 wrap-safe）
 *   - RPM 计算：
 *       tick = 1 us
 *       pulses_per_rev = 2
 *       rpm = 60e6 / (dt_us * pulses_per_rev) = 30e6 / dt_us
 *   - 4-sample 滑动平均：对最近 4 次瞬时 rpm 求均值
 *   - 停转判定：now_ms - last_pulse_ms > 1000ms => rpm=0
 */

#include "tach.h"

#include <string.h>

#include "stm32g0xx_hal.h"

/* =========================================================================== */
/* 编译期参数 */
/* =========================================================================== */

#define TACH_TIMER_HZ            1000000UL
#define TACH_PULSES_PER_REV      2UL
#define TACH_STOP_TIMEOUT_MS     1000UL
#define TACH_AVG_SAMPLES         4U

/* =========================================================================== */
/* 对外暴露给 IRQ handler 的 TIM 句柄 */
/* =========================================================================== */

TIM_HandleTypeDef htim3;

/* =========================================================================== */
/* 内部状态 */
/* =========================================================================== */

typedef struct {
    volatile uint32_t last_ext;
    volatile uint32_t last_pulse_ms;
    volatile uint32_t inst_rpm;
    uint32_t          hist[TACH_AVG_SAMPLES];
    uint8_t           hist_n;
    uint8_t           hist_idx;
    volatile uint32_t rpm_avg;
} tach_ch_state_t;

static volatile uint32_t ovf_cnt;
static tach_ch_state_t   st1;
static tach_ch_state_t   st2;

/* =========================================================================== */
/* 内部工具 */
/* =========================================================================== */

static uint32_t _ext_now_from_capture(uint16_t cap16)
{
    /* 读取 ovf_cnt + cap16 组合成扩展时间戳。
     * 极端情况下，如果溢出刚发生且捕获值很小，ovf_cnt 可能已自增；
     * 误差最多 65536us，对 rpm 有影响但很罕见（且 4-sample 平均会抑制）。
     * 这里保持实现简单、ISR 常数时间。 */
    uint32_t ovf = ovf_cnt;
    return (ovf << 16) | (uint32_t)cap16;
}

static uint32_t _rpm_from_dt(uint32_t dt_us)
{
    if (dt_us == 0U) {
        return 0U;
    }
    /* rpm = 30e6 / dt_us */
    return (uint32_t)(30000000UL / dt_us);
}

static void _push_sample(tach_ch_state_t *st, uint32_t rpm)
{
    st->hist[st->hist_idx] = rpm;
    st->hist_idx = (uint8_t)((st->hist_idx + 1U) % TACH_AVG_SAMPLES);
    if (st->hist_n < TACH_AVG_SAMPLES) {
        st->hist_n++;
    }

    uint32_t sum = 0U;
    for (uint8_t i = 0; i < st->hist_n; i++) {
        sum += st->hist[i];
    }
    st->rpm_avg = (st->hist_n == 0U) ? 0U : (sum / st->hist_n);
}

/* =========================================================================== */
/* 公共接口 */
/* =========================================================================== */

void Tach_Init(void)
{
    memset(&st1, 0, sizeof(st1));
    memset(&st2, 0, sizeof(st2));
    ovf_cnt = 0U;

    __HAL_RCC_GPIOA_CLK_ENABLE();
    GPIO_InitTypeDef gi = {0};
    gi.Pin       = GPIO_PIN_6 | GPIO_PIN_7;
    gi.Mode      = GPIO_MODE_AF_PP;          /* 外部已上拉到 3.3V；MCU 端按复用输入即可 */
    gi.Pull      = GPIO_NOPULL;
    gi.Speed     = GPIO_SPEED_FREQ_LOW;
    gi.Alternate = GPIO_AF1_TIM3;
    HAL_GPIO_Init(GPIOA, &gi);

    __HAL_RCC_TIM3_CLK_ENABLE();

    htim3.Instance               = TIM3;
    htim3.Init.Prescaler         = 15U;                 /* 16MHz/16 = 1MHz */
    htim3.Init.CounterMode       = TIM_COUNTERMODE_UP;
    htim3.Init.Period            = 0xFFFFU;
    htim3.Init.ClockDivision     = TIM_CLOCKDIVISION_DIV1;
    htim3.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
    (void)HAL_TIM_IC_Init(&htim3);

    TIM_IC_InitTypeDef ic = {0};
    ic.ICPolarity  = TIM_INPUTCHANNELPOLARITY_RISING;
    ic.ICSelection = TIM_ICSELECTION_DIRECTTI;
    ic.ICPrescaler = TIM_ICPSC_DIV1;
    ic.ICFilter    = 0x0FU;  /* 数字滤波，抑制毛刺（外部已有 RC） */
    (void)HAL_TIM_IC_ConfigChannel(&htim3, &ic, TIM_CHANNEL_1);
    (void)HAL_TIM_IC_ConfigChannel(&htim3, &ic, TIM_CHANNEL_2);

    /* 使能 update 中断用于溢出扩展 */
    __HAL_TIM_ENABLE_IT(&htim3, TIM_IT_UPDATE);

    /* NVIC：TIM3 */
    /* 给 USB IRQ 让路：TIM3/TIM4 放到更低优先级，避免 tach 噪声导致枚举抖动 */
    HAL_NVIC_SetPriority(TIM3_TIM4_IRQn, 3U, 0U);
    HAL_NVIC_EnableIRQ(TIM3_TIM4_IRQn);

    (void)HAL_TIM_IC_Start_IT(&htim3, TIM_CHANNEL_1);
    (void)HAL_TIM_IC_Start_IT(&htim3, TIM_CHANNEL_2);
}

void Tach_Service(uint32_t now_ms)
{
    /* 停转检测：超过阈值则清零输出（不清历史，避免刚恢复时平均值偏低） */
    if ((uint32_t)(now_ms - st1.last_pulse_ms) > TACH_STOP_TIMEOUT_MS) {
        st1.rpm_avg = 0U;
        st1.inst_rpm = 0U;
    }
    if ((uint32_t)(now_ms - st2.last_pulse_ms) > TACH_STOP_TIMEOUT_MS) {
        st2.rpm_avg = 0U;
        st2.inst_rpm = 0U;
    }
}

uint32_t Tach_GetRpm(tach_channel_t ch)
{
    switch (ch) {
    case TACH_CH1: return st1.rpm_avg;
    case TACH_CH2: return st2.rpm_avg;
    default:       return 0U;
    }
}

/* =========================================================================== */
/* HAL 回调：IRQ 上下文 */
/* =========================================================================== */

void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{
    if (htim->Instance == TIM3) {
        ovf_cnt++;
    }
}

void HAL_TIM_IC_CaptureCallback(TIM_HandleTypeDef *htim)
{
    if (htim->Instance != TIM3) {
        return;
    }

    uint32_t now_ms = HAL_GetTick();

    if (htim->Channel == HAL_TIM_ACTIVE_CHANNEL_1) {
        uint16_t cap = (uint16_t)HAL_TIM_ReadCapturedValue(htim, TIM_CHANNEL_1);
        uint32_t ext = _ext_now_from_capture(cap);
        uint32_t dt  = (uint32_t)(ext - st1.last_ext);
        st1.last_ext = ext;
        st1.last_pulse_ms = now_ms;
        st1.inst_rpm = _rpm_from_dt(dt);
        _push_sample(&st1, st1.inst_rpm);
    } else if (htim->Channel == HAL_TIM_ACTIVE_CHANNEL_2) {
        uint16_t cap = (uint16_t)HAL_TIM_ReadCapturedValue(htim, TIM_CHANNEL_2);
        uint32_t ext = _ext_now_from_capture(cap);
        uint32_t dt  = (uint32_t)(ext - st2.last_ext);
        st2.last_ext = ext;
        st2.last_pulse_ms = now_ms;
        st2.inst_rpm = _rpm_from_dt(dt);
        _push_sample(&st2, st2.inst_rpm);
    } else {
        /* ignore */
    }
}

