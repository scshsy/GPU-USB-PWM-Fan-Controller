/**
 * @file  cli.h
 * @brief 文本命令行框架 / Command Line Interface over USB CDC
 *
 * 数据流：
 *   USB CDC RX 环形缓冲 → CLI 行解析（按 \r 或 \n 分行）→ 命令分发 → 响应
 *
 * 三层职责：
 *   1. CLI 框架本身：字节装行 + 长度上限保护 + 调度 handler
 *   2. 内置 handler：`?` 单字符行 → 回 CRS 诊断状态
 *   3. 默认兜底：所有未识别行 → 原文 echo + CRLF（验证收发链路）
 *
 * 扩展方式（后续 PWM/状态查询/参数设置等命令）：
 *   1. 在自己的模块中实现一个 cli_handler_t 函数
 *   2. 在自己模块的 Init() 里调 CLI_RegisterHandler() 注册
 *   3. 注册顺序决定匹配优先级（先注册先匹配）；任一 handler 命中即返回，
 *      其他不再调用；全部不命中时进入默认 echo
 *
 * 健壮性：
 *   - 行缓冲固定 128 B，超长强制 flush，永不堆栈溢出
 *   - 发送走 CDC_Transmit_NonBlocking + 5 ms 软件超时，永不阻塞主循环
 *   - 不在中断里跑，全部在主循环 CLI_Service() 内同步推进
 */
#ifndef CLI_H
#define CLI_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 命令处理器函数类型
 *
 * @param line  完整一行（不含 \r/\n）；handler 内部可只读，框架保证有效到返回
 * @param len   行字节数
 * @return  0 = 已识别并处理（CLI 不再尝试其他 handler 也不 echo）
 *          非 0 = 不是本 handler 的命令，让框架继续向后匹配
 *
 * @note  - 多个 handler 可同时存在，先注册先匹配
 *        - handler 内部如要发送响应，调用 CLI_SendBytes()
 *        - handler 不应阻塞超过 1 ms，否则会拖慢主循环和 CRS 轮询
 */
typedef int (*cli_handler_t)(const uint8_t *line, uint16_t len);

/**
 * @brief 初始化 CLI 框架
 *
 *        清空行缓冲，注册内置 `?` 命令。必须在 MX_USB_Device_Init 之后、
 *        其他模块调 CLI_RegisterHandler 之前调用。
 */
void CLI_Init(void);

/**
 * @brief 主循环周期调用：消费 RX 环形缓冲、装行、命中 handler 或 echo
 *
 *        每轮主循环调一次即可，内部把 RX ring 里所有可读字节一次清空。
 */
void CLI_Service(void);

/**
 * @brief 注册一个命令处理器
 *
 * @param  handler  要注册的回调（不能为 NULL）
 * @return  0  注册成功
 *         -1 表已满（默认上限 8 个，需要更多请改 cli.c 中 CLI_MAX_HANDLERS）
 *         -2 参数无效
 *
 * @note  在自己模块的 Init() 里调用一次即可；框架不持有 handler 指针之外的
 *        任何状态，handler 卸载暂未支持（一般也不需要）。
 */
int CLI_RegisterHandler(cli_handler_t handler);

/**
 * @brief 通过 USB CDC 发送一段字节流（带 5ms 软件超时）
 *
 *        所有 handler 应优先调用此函数发送响应，统一超时策略。USB 未枚举或
 *        TX busy 超过 5ms 时直接放弃，主机端会自然超时重试，主循环不阻塞。
 *
 * @param  buf  字节缓冲（CLI 拷贝走 PMA 后立即返回，调用方可释放/复用）
 * @param  len  字节数（推荐 ≤ 64，单 USB FS bulk packet）
 */
void CLI_SendBytes(const uint8_t *buf, uint16_t len);

#ifdef __cplusplus
}
#endif

#endif /* CLI_H */
