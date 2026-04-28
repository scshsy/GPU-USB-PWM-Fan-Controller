/**
 * @file  crs_diag.c
 * @brief CRS 状态诊断实现 - 见 crs_diag.h 设计说明
 */

#include <stdio.h>
#include "stm32g0xx_hal.h"
#include "crs_diag.h"

/* ===========================================================================
 * 内部累计计数器
 *
 * 32-bit 无符号；以 1000 cnt/s 计算回卷周期 ≈ 49 天，远超调试场景。
 * 真要长时间在线监测时再改成 64-bit。
 * ========================================================================= */
static volatile uint32_t crs_sync_ok   = 0U;
static volatile uint32_t crs_sync_err  = 0U;
static volatile uint32_t crs_sync_miss = 0U;
static volatile uint32_t crs_trim_ovf  = 0U;

void CRS_Poll(void)
{
    uint32_t isr = CRS->ISR;

    if ((isr & CRS_ISR_SYNCOKF) != 0U) {
        crs_sync_ok++;
        CRS->ICR = CRS_ICR_SYNCOKC;
    }

    /* SYNCERR / SYNCMISS / TRIMOVF 在硬件上共用 ERRC 一次性清掉 */
    uint8_t need_clear_err = 0U;
    if ((isr & CRS_ISR_SYNCERR)  != 0U) { crs_sync_err++;  need_clear_err = 1U; }
    if ((isr & CRS_ISR_SYNCMISS) != 0U) { crs_sync_miss++; need_clear_err = 1U; }
    if ((isr & CRS_ISR_TRIMOVF)  != 0U) { crs_trim_ovf++;  need_clear_err = 1U; }
    if (need_clear_err) {
        CRS->ICR = CRS_ICR_ERRC;
    }
}

int CRS_FormatStatus(char *buf, size_t bufsize)
{
    uint32_t trim = (CRS->CR & CRS_CR_TRIM_Msk) >> CRS_CR_TRIM_Pos;

    return snprintf(buf, bufsize,
                    "CRS TRIM=0x%02lX SYNCOK=%lu ERR=%lu MISS=%lu OVF=%lu\r\n",
                    (unsigned long)trim,
                    (unsigned long)crs_sync_ok,
                    (unsigned long)crs_sync_err,
                    (unsigned long)crs_sync_miss,
                    (unsigned long)crs_trim_ovf);
}
