/**
 * @file  usbd_conf.h
 * @brief USB Device Library 用户配置（被 usbd_def.h 强制包含）
 *
 * 本工程为单接口、单配置的 CDC 设备：
 *   - 关闭 LPM / 复合设备 / 用户字符串扩展，缩小代码与 RAM 占用
 *   - 关闭 USBD_*Log，避免在中断上下文调用 printf
 *   - USBD_malloc/USBD_free 走静态池，全程零 heap
 */

#ifndef __USBD_CONF_H
#define __USBD_CONF_H

#ifdef __cplusplus
extern "C" {
#endif

#include "stm32g0xx_hal.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* ===== USB Device 资源参数（必须在 usbd_def.h 之前定义） ===== */
#define USBD_MAX_NUM_INTERFACES                 1U
#define USBD_MAX_NUM_CONFIGURATION              1U
#define USBD_MAX_STR_DESC_SIZ                   128U
#define USBD_SUPPORT_USER_STRING_DESC           0U
#define USBD_CLASS_USER_STRING_DESC             0U
#define USBD_DEBUG_LEVEL                        0U
#define USBD_LPM_ENABLED                        0U
#define USBD_SELF_POWERED                       1U

/* CDC 类相关默认值（不接 audio/MSC/HID） */
#define USBD_CDC_INTERVAL                       2000U

/* ===== 内存分配（静态池，永不返回 NULL，零碎片） =====
 * USBD_static_malloc 一次只为 USBD_CDC_HandleTypeDef 分配一次（USBD_RegisterClass 时），
 * 池大小取 sizeof(USBD_CDC_HandleTypeDef) 的上界，留余量保证对齐。 */
#define USBD_malloc                             (void *)USBD_static_malloc
#define USBD_free                               USBD_static_free
#define USBD_memset                             memset
#define USBD_memcpy                             memcpy

/* 调试宏全部关闭：避免 ISR 中调用 printf 造成不可预期的卡顿 */
#define USBD_UsrLog(...)                        do { } while (0)
#define USBD_ErrLog(...)                        do { } while (0)
#define USBD_DbgLog(...)                        do { } while (0)

/* HAL_Delay 不在 ISR 调用，但 USB lib 会在初始化时用到，保持映射 */
#define USBD_Delay                              HAL_Delay

/* ===== 静态分配函数 ===== */
void *USBD_static_malloc(uint32_t size);
void  USBD_static_free(void *p);

#ifdef __cplusplus
}
#endif

#endif /* __USBD_CONF_H */
