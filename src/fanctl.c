/**
 * @file  fanctl.c
 * @brief 风扇控制与安全仲裁（CLI 协议入口）
 *
 * 状态机（简化）：
 *   - DEFAULT：上电后尚未收到任何 host 合法消息（set/kick） -> 输出 default duty
 *   - HOST：USB 已枚举，且 host watchdog 未超时 -> 输出 host 设定 duty
 *   - SAFE：USB 未枚举 或 host watchdog 超时 -> 强制 safe duty
 *   - OVERRIDE：调试/故障兜底覆盖（优先级最高）
 *
 * Watchdog 刷新：
 *   - 任一合法的 set fan / kick 命令都会刷新 last_host_msg_ms
 *
 * 命令解析：
 *   - 输入来自 CLI 的单行字节（不含 CR/LF），本模块做大小写不敏感与空白分隔解析
 *   - 解析失败返回 err <reason>
 */

#include "fanctl.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "stm32g0xx_hal.h"

#include "cli.h"
#include "pwm.h"
#include "tach.h"
#include "usb_device.h"

/* =========================================================================== */
/* 内部常量 */
/* =========================================================================== */

#define FANCTL_LINE_MAX  128U

/* fault bits（给 get status 用，便于以后扩展） */
#define FAULT_USB_NOT_CFG   (1U << 0)
#define FAULT_HOST_WD_TO    (1U << 1)

/* =========================================================================== */
/* 内部状态 */
/* =========================================================================== */

static uint8_t  host_duty_1;
static uint8_t  host_duty_2;
static uint8_t  out_duty_1;
static uint8_t  out_duty_2;
static uint8_t  override_en;
static uint8_t  override_duty;

static uint32_t last_host_msg_ms;
static uint8_t  ever_host_seen;
static fan_src_t last_src;

/* =========================================================================== */
/* 小工具：tokenizer（不会修改原始 line；只在本地 buf 上操作） */
/* =========================================================================== */

static void to_lower_ascii(char *s)
{
    for (; *s; s++) {
        if (*s >= 'A' && *s <= 'Z') {
            *s = (char)(*s - 'A' + 'a');
        }
    }
}

static char *skip_ws(char *p)
{
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    return p;
}

static char *next_tok(char **pp)
{
    char *p = skip_ws(*pp);
    if (*p == '\0') {
        *pp = p;
        return NULL;
    }
    char *t = p;
    while (*p && *p != ' ' && *p != '\t') {
        p++;
    }
    if (*p) {
        *p++ = '\0';
    }
    *pp = p;
    return t;
}

static int parse_u8(const char *s, uint8_t *out)
{
    if (!s || !*s) {
        return -1;
    }
    char *end = NULL;
    long v = strtol(s, &end, 10);
    if (end == s || *end != '\0' || v < 0 || v > 255) {
        return -1;
    }
    *out = (uint8_t)v;
    return 0;
}

static uint8_t clamp_pct(uint8_t pct)
{
    if (pct > 100U) {
        return 100U;
    }
    return pct;
}

/* =========================================================================== */
/* 仲裁逻辑 */
/* =========================================================================== */

static uint8_t usb_configured(void)
{
    return (hUsbDeviceFS.dev_state == USBD_STATE_CONFIGURED) ? 1U : 0U;
}

uint32_t FanCtl_GetHostWdLeftMs(uint32_t now_ms)
{
    uint32_t dt = (uint32_t)(now_ms - last_host_msg_ms);
    if (dt >= FAN_HOST_WD_MS) {
        return 0U;
    }
    return (FAN_HOST_WD_MS - dt);
}

fan_src_t FanCtl_GetSource(void)
{
    return last_src;
}

uint8_t FanCtl_GetDuty(uint8_t ch)
{
    if (ch == 1U) return out_duty_1;
    if (ch == 2U) return out_duty_2;
    return 0U;
}

void FanCtl_SetOverride(uint8_t enable, uint8_t duty_pct)
{
    override_en = enable ? 1U : 0U;
    override_duty = clamp_pct(duty_pct);
}

void FanCtl_Service(uint32_t now_ms)
{
    uint8_t usb_ok = usb_configured();
    uint32_t wd_left = FanCtl_GetHostWdLeftMs(now_ms);

    uint8_t tgt1 = FAN_DEFAULT_DUTY_PCT;
    uint8_t tgt2 = FAN_DEFAULT_DUTY_PCT;
    fan_src_t src = FAN_SRC_DEFAULT;

    if (override_en) {
        tgt1 = override_duty;
        tgt2 = override_duty;
        src = FAN_SRC_OVERRIDE;
    } else if (!usb_ok) {
        tgt1 = FAN_SAFE_DUTY_PCT;
        tgt2 = FAN_SAFE_DUTY_PCT;
        src = FAN_SRC_SAFE;
    } else if (ever_host_seen && wd_left == 0U) {
        tgt1 = FAN_SAFE_DUTY_PCT;
        tgt2 = FAN_SAFE_DUTY_PCT;
        src = FAN_SRC_SAFE;
    } else if (ever_host_seen) {
        tgt1 = host_duty_1;
        tgt2 = host_duty_2;
        src = FAN_SRC_HOST;
    } else {
        /* DEFAULT：保持默认档，等待主机首次消息 */
    }

    /* 写到 PWM（只有变化才写，减少总线抖动） */
    if (tgt1 != out_duty_1) {
        out_duty_1 = tgt1;
        Pwm_SetDuty(PWM_CH1, out_duty_1);
    }
    if (tgt2 != out_duty_2) {
        out_duty_2 = tgt2;
        Pwm_SetDuty(PWM_CH2, out_duty_2);
    }

    last_src = src;
}

/* =========================================================================== */
/* CLI handler */
/* =========================================================================== */

static void reply_line(const char *s)
{
    if (!s) return;
    CLI_SendBytes((const uint8_t *)s, (uint16_t)strlen(s));
}

static int fanctl_cli_handler(const uint8_t *line, uint16_t len)
{
    if (len == 0U || len > FANCTL_LINE_MAX) {
        return -1;
    }

    char buf[FANCTL_LINE_MAX + 1U];
    memcpy(buf, line, len);
    buf[len] = '\0';
    to_lower_ascii(buf);

    char *p = buf;
    char *t0 = next_tok(&p);
    if (!t0) {
        return -1;
    }

    /* ---- kick ---- */
    if (strcmp(t0, "kick") == 0) {
        last_host_msg_ms = HAL_GetTick();
        ever_host_seen = 1U;
        reply_line("ok\r\n");
        return 0;
    }

    /* ---- get ... ---- */
    if (strcmp(t0, "get") == 0) {
        char *t1 = next_tok(&p);
        if (!t1) {
            reply_line("err bad_cmd\r\n");
            return 0;
        }

        if (strcmp(t1, "rpm") == 0) {
            uint32_t r1 = Tach_GetRpm(TACH_CH1);
            uint32_t r2 = Tach_GetRpm(TACH_CH2);
            char out[64];
            int n = snprintf(out, sizeof(out), "rpm 1=%lu 2=%lu\r\n",
                             (unsigned long)r1, (unsigned long)r2);
            if (n > 0 && (size_t)n < sizeof(out)) {
                CLI_SendBytes((const uint8_t *)out, (uint16_t)n);
            }
            return 0;
        }

        if (strcmp(t1, "status") == 0) {
            uint32_t now = HAL_GetTick();
            uint32_t r1 = Tach_GetRpm(TACH_CH1);
            uint32_t r2 = Tach_GetRpm(TACH_CH2);
            uint8_t usb_ok = usb_configured();
            uint32_t wd_left = FanCtl_GetHostWdLeftMs(now);

            uint32_t fault = 0U;
            if (!usb_ok) {
                fault |= FAULT_USB_NOT_CFG;
            }
            if (ever_host_seen && wd_left == 0U) {
                fault |= FAULT_HOST_WD_TO;
            }

            const char *src =
                (last_src == FAN_SRC_HOST) ? "host" :
                (last_src == FAN_SRC_SAFE) ? "safe" :
                (last_src == FAN_SRC_OVERRIDE) ? "override" :
                "default";

            char out[128];
            int n = snprintf(out, sizeof(out),
                             "duty=%u,%u rpm=%lu,%lu usb=%u src=%s wd=%lu fault=0x%lx\r\n",
                             (unsigned)out_duty_1, (unsigned)out_duty_2,
                             (unsigned long)r1, (unsigned long)r2,
                             (unsigned)usb_ok,
                             src,
                             (unsigned long)wd_left,
                             (unsigned long)fault);
            if (n > 0 && (size_t)n < sizeof(out)) {
                CLI_SendBytes((const uint8_t *)out, (uint16_t)n);
            }
            return 0;
        }

        reply_line("err bad_cmd\r\n");
        return 0;
    }

    /* ---- set fan <ch> <pct> ---- */
    if (strcmp(t0, "set") == 0) {
        char *t1 = next_tok(&p);
        char *t2 = next_tok(&p);
        char *t3 = next_tok(&p);
        if (!t1 || !t2 || !t3 || strcmp(t1, "fan") != 0) {
            reply_line("err bad_cmd\r\n");
            return 0;
        }

        uint8_t ch = 0U;
        uint8_t pct = 0U;
        if (parse_u8(t2, &ch) != 0 || (ch != 1U && ch != 2U)) {
            reply_line("err bad_ch\r\n");
            return 0;
        }
        if (parse_u8(t3, &pct) != 0 || pct > 100U) {
            reply_line("err bad_pct\r\n");
            return 0;
        }

        pct = clamp_pct(pct);
        if (ch == 1U) {
            host_duty_1 = pct;
        } else {
            host_duty_2 = pct;
        }
        last_host_msg_ms = HAL_GetTick();
        ever_host_seen = 1U;

        reply_line("ok\r\n");
        return 0;
    }

    return -1; /* 不是本模块命令，让 CLI 继续尝试其他 handler */
}

void FanCtl_Init(void)
{
    host_duty_1 = FAN_DEFAULT_DUTY_PCT;
    host_duty_2 = FAN_DEFAULT_DUTY_PCT;
    out_duty_1  = FAN_DEFAULT_DUTY_PCT;
    out_duty_2  = FAN_DEFAULT_DUTY_PCT;
    override_en = 0U;
    override_duty = FAN_SAFE_DUTY_PCT;

    last_host_msg_ms = HAL_GetTick();
    ever_host_seen = 0U;
    last_src = FAN_SRC_DEFAULT;

    /* 上电立即给默认档，避免 PWM 模块上电后保持 0 */
    Pwm_SetDuty(PWM_CH1, out_duty_1);
    Pwm_SetDuty(PWM_CH2, out_duty_2);

    (void)CLI_RegisterHandler(fanctl_cli_handler);
}

