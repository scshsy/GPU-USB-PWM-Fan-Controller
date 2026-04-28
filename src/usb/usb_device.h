/**
 * @file  usb_device.h
 * @brief USB 设备初始化总入口
 */

#ifndef __USB_DEVICE_H
#define __USB_DEVICE_H

#ifdef __cplusplus
extern "C" {
#endif

#include "usbd_def.h"

extern USBD_HandleTypeDef hUsbDeviceFS;

/**
 * @brief 初始化并启动 USB 设备（不阻塞等待主机枚举）
 *        必须在 SystemClock_Config 之后、主循环开始前调用一次
 */
void MX_USB_Device_Init(void);

#ifdef __cplusplus
}
#endif

#endif /* __USB_DEVICE_H */
