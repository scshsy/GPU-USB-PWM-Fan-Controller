/**
 * @file  tach.h
 * @brief 风扇 TACH 测速模块（TIM3 输入捕获）
 *
 * 硬件映射（已定板，严禁更改）：
 *   - PA6 -> TIM3_CH1 -> FAN1_TACH
 *   - PA7 -> TIM3_CH2 -> FAN2_TACH
 *
 * 约定：
 *   - 风扇每转 2 个脉冲（常见服务器风扇规格）
 *   - 若 > 1s 未收到脉冲：RPM 视为 0
 *   - 对瞬时 RPM 做 4-sample 滑动平均，减小抖动
 *
 * 注意：
 *   - 模块内部使用 TIM3 的 update 中断扩展 16-bit 计数器到 32-bit 时间戳
 *   - ISR 只做捕获与轻量计算，不做任何阻塞/打印
 */
#ifndef TACH_H
#define TACH_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    TACH_CH1 = 1,
    TACH_CH2 = 2,
} tach_channel_t;

void Tach_Init(void);

/**
 * @brief 主循环服务：处理停转检测与平均值稳定输出
 *
 * @param now_ms HAL_GetTick() 返回值（wrap-safe）
 */
void Tach_Service(uint32_t now_ms);

/**
 * @brief 获取指定通道当前 RPM（已做滑动平均与停转判定）
 */
uint32_t Tach_GetRpm(tach_channel_t ch);

#ifdef __cplusplus
}
#endif

#endif /* TACH_H */

