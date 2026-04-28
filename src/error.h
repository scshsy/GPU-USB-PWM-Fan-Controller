/**
 * @file  error.h
 * @brief 系统级故障兜底入口
 *
 * 任何模块在初始化失败、关键 assert 不通过、HAL 返回 ERROR 等不可恢复
 * 故障时调用此函数。函数永不返回，进入 LED_SYS 5Hz 快闪状态便于现场目视。
 *
 * 设计要点：
 *   - 不依赖 SysTick（用纯 nop 软件延时），因为可能时钟尚未配置成功
 *   - 不调 __disable_irq，避免 SWD 调试器无法接管
 *   - 与正常 1Hz 心跳的频率比为 5:1，肉眼可立刻区分
 */
#ifndef ERROR_H
#define ERROR_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 故障兜底：永不返回，进入 LED_SYS ~5Hz 快闪
 *
 * @note  ⚠ 此函数会被 HAL 库内部（如 HAL_RCC_OscConfig 失败）和应用层共同调用，
 *           原型签名必须与 STM32Cube 模板一致：void Error_Handler(void)
 */
void Error_Handler(void);

#ifdef __cplusplus
}
#endif

#endif /* ERROR_H */
