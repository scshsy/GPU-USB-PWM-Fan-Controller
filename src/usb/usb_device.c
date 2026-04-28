/**
 * @file  usb_device.c
 * @brief MX_USB_Device_Init - USB Device 总装入口
 *
 * 调用顺序固定：
 *   USBD_Init                  → 关联描述符 + 初始化 PCD
 *   USBD_RegisterClass         → 注册 CDC 类
 *   USBD_CDC_RegisterInterface → 注册应用层 fops
 *   USBD_Start                 → 打开 D+ 上拉，触发主机枚举
 *
 * USBD_Start 立即返回，不等枚举完成。dev_state 会随中断推进到
 * USBD_STATE_CONFIGURED，主循环通过该状态决定是否能收发。
 */

#include "usb_device.h"
#include "usbd_core.h"
#include "usbd_desc.h"
#include "usbd_cdc.h"
#include "usbd_cdc_if.h"

USBD_HandleTypeDef hUsbDeviceFS;

void MX_USB_Device_Init(void)
{
    /* id=0 仅是 USBD 库内部标识值，单设备工程随便给 */
    if (USBD_Init(&hUsbDeviceFS, &FS_Desc, 0U) != USBD_OK) {
        /* 静默失败：返回让主循环至少能继续闪 LED，不挂死系统 */
        return;
    }
    if (USBD_RegisterClass(&hUsbDeviceFS, &USBD_CDC) != USBD_OK) {
        return;
    }
    if (USBD_CDC_RegisterInterface(&hUsbDeviceFS, &USBD_Interface_fops_FS) != USBD_OK) {
        return;
    }
    USBD_Start(&hUsbDeviceFS);
}
