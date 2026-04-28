/**
 * @file  led.h
 * @brief 板载 LED 状态机（PA1 = SYS 心跳；PA2 = USB 状态）
 *
 * 设计原则：
 *   - 完全非阻塞：所有 *_Update() 函数取毫秒时间戳作为参数，内部用差值判断
 *     翻转，主循环每轮调用一次即可。
 *   - LED 模块不直接耦合 USB / CDC：业务状态（是否枚举、是否有 RX/TX 事件）
 *     由调用方以参数形式传入。这样模块边界干净，单元测试也方便。
 *   - 提供命令式接口（LED_*_Set / LED_*_Toggle）以便 Error_Handler 等场景使用。
 *
 * 后续扩展点：
 *   - 风扇过温/堵转告警 LED：可以新增 LED_Alert_Update(now, fault_flags)
 *   - 多色 LED 或 PWM 调亮：在本模块内部增加状态枚举
 */
#ifndef LED_H
#define LED_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 配置 LED 引脚（PA1 / PA2）为推挽输出，初始电平 = 灭
 *
 * @note  本函数会负责开 GPIOA 时钟。多次调用安全。
 *        必须在 HAL_Init 之后、Clock_Init 之前/之后均可调用。
 */
void LED_Init(void);

/* ---------------- LED_SYS（PA1，系统心跳） ---------------- */

/** 翻转 LED_SYS 一次（用于 Error_Handler 等同步场景） */
void LED_Sys_Toggle(void);

/** 强制设置 LED_SYS 状态：on=1 亮，on=0 灭 */
void LED_Sys_Set(uint8_t on);

/**
 * @brief 主循环周期更新 LED_SYS：默认 1 Hz 自由翻转（500 ms on / 500 ms off）
 * @param now  当前 HAL_GetTick() 毫秒时间戳
 *
 * @note  只要主循环运行此函数会被周期调用，LED 就会稳定 1 Hz 翻转；
 *        若主循环卡死，PA1 会停在某一个电平，便于现场观察。
 */
void LED_Sys_Update(uint32_t now);

/* ---------------- LED_USB（PA2，USB 状态） ---------------- */

/**
 * @brief 主循环周期更新 LED_USB
 *
 * 状态语义：
 *   - configured = 0       → 灭（未枚举或断开）
 *   - configured = 1, 无事件→ 常亮
 *   - configured = 1, event=1 → 触发一次短熄灭（默认 50 ms）模拟"心跳闪烁"
 *
 * @param now         当前 HAL_GetTick() 毫秒时间戳
 * @param configured  USB 是否已枚举到 CONFIGURED 状态
 * @param event       本轮是否检测到 RX/TX 事件（一次性脉冲，由调用方 take/clear）
 *
 * @note  把 configured/event 当成参数传入，是为了让 LED 模块不依赖 USB 头文件。
 */
void LED_Usb_Update(uint32_t now, uint8_t configured, uint8_t event);

#ifdef __cplusplus
}
#endif

#endif /* LED_H */
