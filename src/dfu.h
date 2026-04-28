/**
 * @file  dfu.h
 * @brief 通过 USB CDC 一键进入 STM32G0 ROM Bootloader（USB DFU 模式）
 *
 * 升级流程总览：
 *   ┌──────────────────────────────────────────────────────────────────┐
 *   │ 1. 应用收到 "dfu" 命令（CLI handler）                             │
 *   │ 2. 写 magic 到 TAMP->BKP0R + NVIC_SystemReset()                  │
 *   │ 3. 复位后，main 第一行 DFU_CheckAndJumpEarly() 检测 magic         │
 *   │ 4. 命中 → 清 magic + 跳 0x1FFF0000 (system memory)               │
 *   │ 5. ROM bootloader 自动打开 USB FS 设备 (VID:0483 PID:DF11)       │
 *   │ 6. PC 端 dfu-util 写 .bin 到 0x08000000 + leave                  │
 *   │ 7. ROM 跳回应用 0x08000000，CDC 重新枚举                          │
 *   └──────────────────────────────────────────────────────────────────┘
 *
 * 为什么用 TAMP backup register 存 magic 而不是普通 RAM：
 *   - TAMP_BKP0R 在软复位 / 看门狗复位 / 系统复位都保留
 *   - 在 V_DD 断电（拔 USB 重插）时清零 → 不会误进 DFU
 *   - 不依赖链接脚本里 .noinit 段，不需要改 ld
 *
 * 健壮性：
 *   - magic = 32-bit 任意常数，POR 后 BKP0R = 0 → 不命中 → 走正常应用
 *   - DFU 命令本身需要先回响应再 reset，handler 内部用 HAL_Delay 等 USB 把
 *     bulk-IN 包发出去，避免主机看不到响应就突然断连
 *   - 出现意外（用户接错文件、断电）时永远可以用 STLink 救回应用层
 */
#ifndef DFU_H
#define DFU_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 启动早期（HAL_Init 之前）检测 DFU magic，命中则跳 ROM bootloader
 *
 *        必须是 main() 的第一条语句调用。函数自己负责开 PWR/RTCAPB 时钟。
 *        正常路径下读到 magic ≠ DFU_MAGIC，立即返回，几条指令的代价。
 *        命中路径永不返回（直接跳 ROM）。
 */
void DFU_CheckAndJumpEarly(void);

/**
 * @brief 注册 CLI "dfu" 命令处理器
 *
 *        必须在 CLI_Init() 之后调用。
 */
void DFU_Init(void);

/**
 * @brief 写 magic 到 backup register 并触发软复位（永不返回）
 *
 *        一般通过 CLI "dfu" 命令间接触发，特殊场景下也可由其他模块直接调用
 *        （比如：硬件按钮、协议错误等）。
 */
void DFU_RebootToBootloader(void);

#ifdef __cplusplus
}
#endif

#endif /* DFU_H */
