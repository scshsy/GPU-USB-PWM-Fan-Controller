/**
 * @file  usbd_cdc_if.h
 * @brief CDC 应用接口：注册到 USBD_CDC，并向主循环导出收/发 API
 */

#ifndef __USBD_CDC_IF_H
#define __USBD_CDC_IF_H

#ifdef __cplusplus
extern "C" {
#endif

#include "usbd_cdc.h"

/* USBD_CDC_RegisterInterface 用 */
extern USBD_CDC_ItfTypeDef USBD_Interface_fops_FS;

/* USB 设备主句柄（在 usb_device.c 定义），主循环判 dev_state 用 */
extern USBD_HandleTypeDef hUsbDeviceFS;

/* ===== 应用层 API（主循环线程使用，非中断安全） ===== */

/**
 * @brief  从 RX 环形缓冲取 1 字节
 * @param  out  输出字节
 * @retval 1 取到；0 缓冲为空
 */
uint8_t CDC_RingPop(uint8_t *out);

/**
 * @brief  非阻塞发送：dev_state 未配置或 TxState 忙均立即返回 USBD_BUSY
 * @retval USBD_OK / USBD_BUSY / USBD_FAIL
 */
uint8_t CDC_Transmit_NonBlocking(uint8_t *buf, uint16_t len);

/**
 * @brief  RX 缓冲溢出累计计数（仅供调试观察）
 */
uint32_t CDC_GetRxOverflowCount(void);

/**
 * @brief  收/发事件标志：每收到一帧或发送完一帧自加，主循环消费
 *         用来驱动 LED_USB 闪烁，主循环读后清零
 */
uint8_t  CDC_TakeEventFlag(void);

#ifdef __cplusplus
}
#endif

#endif /* __USBD_CDC_IF_H */
