/**
 * @file  led.c
 * @brief LED 状态机实现 - 见 led.h 设计说明
 */

#include "stm32g0xx_hal.h"
#include "led.h"

/* ===========================================================================
 * 引脚映射 - 修改这里即可换 LED 接线
 * ========================================================================= */
#define LED_SYS_GPIO_PORT   GPIOA
#define LED_SYS_GPIO_PIN    GPIO_PIN_1

#define LED_USB_GPIO_PORT   GPIOA
#define LED_USB_GPIO_PIN    GPIO_PIN_2

/* ===========================================================================
 * 时序参数
 * ========================================================================= */
#define LED_SYS_HALF_PERIOD_MS  500U  /* 系统心跳 1 Hz：500ms 翻一次 */
#define LED_USB_BLINK_MS         50U  /* USB 事件闪烁短熄灭时长 */

/* ===========================================================================
 * 内部状态（每个 LED 独立一组，避免互相串扰）
 * ========================================================================= */
static uint32_t led_sys_last_toggle_ms = 0U;
static uint32_t led_usb_blink_until_ms = 0U;  /* event 触发后 LED 暂时熄灭到此时刻 */

/* ===========================================================================
 * 初始化
 * ========================================================================= */
void LED_Init(void)
{
    __HAL_RCC_GPIOA_CLK_ENABLE();

    GPIO_InitTypeDef gpio = {0};
    gpio.Pin   = LED_SYS_GPIO_PIN | LED_USB_GPIO_PIN;
    gpio.Mode  = GPIO_MODE_OUTPUT_PP;
    gpio.Pull  = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(GPIOA, &gpio);

    /* 初始默认全灭（早期诊断阶段 main 会在 HAL_Init 之前手动点亮 PA1，
     * 此处不做强制清零，让早期诊断的"已亮"状态延续到 Clock_Init 之后） */
}

/* ===========================================================================
 * LED_SYS（PA1）
 * ========================================================================= */
void LED_Sys_Toggle(void)
{
    HAL_GPIO_TogglePin(LED_SYS_GPIO_PORT, LED_SYS_GPIO_PIN);
}

void LED_Sys_Set(uint8_t on)
{
    HAL_GPIO_WritePin(LED_SYS_GPIO_PORT, LED_SYS_GPIO_PIN,
                      on ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

void LED_Sys_Update(uint32_t now)
{
    /* 用无符号差值判断，HAL_GetTick 49.7 天回绕也安全 */
    if ((uint32_t)(now - led_sys_last_toggle_ms) >= LED_SYS_HALF_PERIOD_MS) {
        led_sys_last_toggle_ms = now;
        HAL_GPIO_TogglePin(LED_SYS_GPIO_PORT, LED_SYS_GPIO_PIN);
    }
}

/* ===========================================================================
 * LED_USB（PA2）
 * ========================================================================= */
void LED_Usb_Update(uint32_t now, uint8_t configured, uint8_t event)
{
    if (!configured) {
        HAL_GPIO_WritePin(LED_USB_GPIO_PORT, LED_USB_GPIO_PIN, GPIO_PIN_RESET);
        led_usb_blink_until_ms = now; /* 进入未配置态时清掉残留 blink 计时 */
        return;
    }

    /* 已枚举：常亮基线，遇事件触发一次短熄灭（"反向闪烁"，亮底上的暗脉冲） */
    if (event) {
        led_usb_blink_until_ms = now + LED_USB_BLINK_MS;
    }

    /* (int32_t)(now - until) < 0 表示尚未到达 until 时刻 → 处于熄灭窗口内 */
    if ((int32_t)(now - led_usb_blink_until_ms) < 0) {
        HAL_GPIO_WritePin(LED_USB_GPIO_PORT, LED_USB_GPIO_PIN, GPIO_PIN_RESET);
    } else {
        HAL_GPIO_WritePin(LED_USB_GPIO_PORT, LED_USB_GPIO_PIN, GPIO_PIN_SET);
    }
}
