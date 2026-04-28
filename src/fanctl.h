/**
 * @file  fanctl.h
 * @brief 风扇控制与安全仲裁（host vs safe）
 *
 * 设计目标（按 prompt.txt 安全策略落地）：
 *   - 上电/复位默认 duty = FAN_DEFAULT_DUTY_PCT
 *   - USB 未枚举 -> 强制 FAN_SAFE_DUTY_PCT
 *   - 主机失联 watchdog（host 命令或 kick 超过阈值未刷新）-> 强制 FAN_SAFE_DUTY_PCT
 *   - 仲裁顺序：override > host_command > safe(default)
 *   - 绝不阻塞：所有判断在 FanCtl_Service(now_ms) 主循环中完成
 *
 * 协议命令：
 *   - set fan <ch> <pct>
 *   - get rpm
 *   - get status
 *   - kick
 */
#ifndef FANCTL_H
#define FANCTL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 上电默认档（与 fail-safe 分离，便于以后独立调参） */
#define FAN_DEFAULT_DUTY_PCT   80U
/* 安全档（USB 未枚举 / 主机失联） */
#define FAN_SAFE_DUTY_PCT      80U

#define FAN_HOST_WD_MS         3000U

typedef enum {
    FAN_SRC_DEFAULT = 0,
    FAN_SRC_HOST    = 1,
    FAN_SRC_SAFE    = 2,
    FAN_SRC_OVERRIDE= 3,
} fan_src_t;

void FanCtl_Init(void);

/**
 * @brief 主循环服务：仲裁 + 写入 PWM
 *
 * @param now_ms HAL_GetTick()
 */
void FanCtl_Service(uint32_t now_ms);

/**
 * @brief 允许上层临时强制 duty（调试/故障兜底）；enable=0 即取消 override
 */
void FanCtl_SetOverride(uint8_t enable, uint8_t duty_pct);

fan_src_t FanCtl_GetSource(void);

/**
 * @brief 获取当前实际 duty（仲裁后的最终输出）
 */
uint8_t FanCtl_GetDuty(uint8_t ch); /* ch=1/2 */

/**
 * @brief 获取 host watchdog 剩余时间（ms）。已超时返回 0
 */
uint32_t FanCtl_GetHostWdLeftMs(uint32_t now_ms);

#ifdef __cplusplus
}
#endif

#endif /* FANCTL_H */

