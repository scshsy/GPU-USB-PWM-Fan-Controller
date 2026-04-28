/**
 * @file  cli.c
 * @brief CLI 实现 - 见 cli.h 设计说明
 *
 * 内部状态：
 *   - line_buf / line_len   : 行装配缓冲（容量 CLI_LINE_MAX，含末尾 CRLF 余量）
 *   - handlers / handlers_n : 注册表，固定容量 CLI_MAX_HANDLERS
 *
 * 匹配/分发流程：
 *   for each handler in handlers[]:
 *       if handler(line, len) == 0: 已处理，返回
 *   default: echo 原文 + CRLF
 */

#include <string.h>

#include "stm32g0xx_hal.h"
#include "cli.h"
#include "crs_diag.h"
#include "usbd_cdc_if.h"

/* ===========================================================================
 * 编译期参数
 * ========================================================================= */
#define CLI_LINE_MAX        128U   /* 单行最长（不含末尾 CRLF），多 2 B 备用 */
#define CLI_TX_TIMEOUT_MS     5U   /* 单次发送软件超时 */
#define CLI_MAX_HANDLERS      8U   /* 命令处理器注册表容量 */
#define CLI_QUERY_RESP_MAX   80U   /* "?" 命令响应缓冲 */

/* ===========================================================================
 * 内部状态
 * ========================================================================= */
static uint8_t       line_buf[CLI_LINE_MAX + 2U];   /* +2 是为 echo 时附加 CRLF */
static uint16_t      line_len;

static cli_handler_t handlers[CLI_MAX_HANDLERS];
static uint8_t       handlers_n;

/* ===========================================================================
 * 内置 handler 前向声明
 * ========================================================================= */
static int cli_builtin_query(const uint8_t *line, uint16_t len);

/* ===========================================================================
 * 公共接口
 * ========================================================================= */
void CLI_Init(void)
{
    line_len   = 0U;
    handlers_n = 0U;
    memset(handlers, 0, sizeof(handlers));

    /* 内置：`?` 单字符行 → CRS 状态 */
    CLI_RegisterHandler(cli_builtin_query);
}

int CLI_RegisterHandler(cli_handler_t handler)
{
    if (handler == NULL) {
        return -2;
    }
    if (handlers_n >= CLI_MAX_HANDLERS) {
        return -1;
    }
    handlers[handlers_n++] = handler;
    return 0;
}

void CLI_SendBytes(const uint8_t *buf, uint16_t len)
{
    if (buf == NULL || len == 0U) {
        return;
    }

    /* 软件级有限重试：USB 未配置 / TxState busy 时让出 CPU 直到超时
     *
     * 注意：CDC_Transmit_NonBlocking 沿用 ST USB 库历史接口，签名 uint8_t*
     * 不带 const（实际只读不修改）。本地强转一次去掉编译警告，语义安全。 */
    uint32_t deadline = HAL_GetTick() + CLI_TX_TIMEOUT_MS;
    while (CDC_Transmit_NonBlocking((uint8_t *)buf, len) == USBD_BUSY) {
        if ((int32_t)(HAL_GetTick() - deadline) >= 0) {
            return; /* 超时即放弃，永不阻塞主循环 */
        }
    }
}

/* ===========================================================================
 * 主循环服务：行装配 + 调度
 * ========================================================================= */

/* 把已装好的一行送入分发管线 */
static void cli_dispatch(uint8_t *line, uint16_t len)
{
    if (len == 0U) {
        return;
    }

    /* 1) 已注册 handler 按顺序尝试 */
    for (uint8_t i = 0U; i < handlers_n; i++) {
        if (handlers[i](line, len) == 0) {
            return;
        }
    }

    /* 2) 兜底 echo：原文 + CRLF
     *    line_buf 末尾留了 2 B 用于附加，安全 */
    line[len++] = '\r';
    line[len++] = '\n';
    CLI_SendBytes(line, len);
}

void CLI_Service(void)
{
    uint8_t b;

    /* 把 RX ring 中目前可读的字节一次性消费完，避免下轮主循环再处理累积时延 */
    while (CDC_RingPop(&b)) {
        if (b == '\r' || b == '\n') {
            /* 行结束：把当前行送 dispatch（line_len 可能为 0，被 dispatch 内忽略） */
            cli_dispatch(line_buf, line_len);
            line_len = 0U;
        } else if (line_len < CLI_LINE_MAX) {
            line_buf[line_len++] = b;
            /* 行长达上限，强制 flush 防止丢字节歧义 */
            if (line_len == CLI_LINE_MAX) {
                cli_dispatch(line_buf, line_len);
                line_len = 0U;
            }
        }
        /* else: 不应到达；上面 == 时已 flush */
    }
}

/* ===========================================================================
 * 内置 handler：`?` 单字符行 → CRS 状态行
 *
 * 严格只匹配 len==1 && line[0]=='?'。`??` / `? ` / `?abc` 都不匹配，
 * 仍走默认 echo。
 * ========================================================================= */
static int cli_builtin_query(const uint8_t *line, uint16_t len)
{
    if (len != 1U || line[0] != '?') {
        return -1;
    }

    char buf[CLI_QUERY_RESP_MAX];
    int  n = CRS_FormatStatus(buf, sizeof(buf));
    if (n > 0 && (size_t)n < sizeof(buf)) {
        CLI_SendBytes((const uint8_t *)buf, (uint16_t)n);
    }
    return 0;
}
