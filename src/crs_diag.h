/**
 * @file  crs_diag.h
 * @brief CRS（Clock Recovery System）实时状态诊断
 *
 * STM32G0 的 HSI48 由 CRS 用 USB SOF 反向校准，本模块提供：
 *   1. 主循环周期轮询，累加 SYNCOK / SYNCERR / SYNCMISS / TRIMOVF 计数
 *   2. 把当前 TRIM + 各计数器格式化成一行 ASCII，供 CLI "?" 命令回显
 *
 * 计数语义（关键判据）：
 *   - SYNCOK   ≈ 1000/s（USB 在线时每 1ms 一次 SOF）→ 校准在工作
 *   - SYNCERR  本地频率偏差超出 ErrorLimit；偶发可忽略，持续累加要警惕
 *   - SYNCMISS 应到的 SOF 没到（USB 挂起/拔线/暂停发包），断网时正常累加
 *   - TRIMOVF  TRIM 撞到边界（实测应永远 0）；非 0 表示 HSI48 严重偏离，要查 LDO/温度
 */
#ifndef CRS_DIAG_H
#define CRS_DIAG_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 主循环周期调用：扫一次 CRS->ISR，累加各计数并清标志
 *
 * @note  - 必须 1 ms 内被调用一次以上，否则可能漏 SOF 标志（实测主循环 << 1ms）
 *        - 不在中断里轮询，因为 CRS 标志读+清不是原子操作，主循环单线程更稳
 */
void CRS_Poll(void);

/**
 * @brief 把当前 CRS 状态格式化成 ASCII 行（不含 \r\n 之外的特殊字符）
 *
 *        输出格式（约 60 字节）：
 *            CRS TRIM=0xXX SYNCOK=N ERR=N MISS=N OVF=N\r\n
 *
 * @param  buf      输出缓冲（至少 80 字节安全）
 * @param  bufsize  缓冲大小
 * @return 写入字节数（不含末尾 '\0'）；< 0 或 ≥ bufsize 表示失败/截断
 *
 * @note  使用 snprintf 实现，不会越界。调用方负责把返回的字节通过 USB 发出去。
 */
int CRS_FormatStatus(char *buf, size_t bufsize);

#ifdef __cplusplus
}
#endif

#endif /* CRS_DIAG_H */
