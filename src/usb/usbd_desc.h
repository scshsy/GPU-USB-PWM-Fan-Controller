/**
 * @file  usbd_desc.h
 * @brief USB CDC 设备描述符表 + 字符串描述符回调
 */

#ifndef __USBD_DESC_H
#define __USBD_DESC_H

#ifdef __cplusplus
extern "C" {
#endif

#include "usbd_def.h"

/* 让 USBD_Init 拿到的回调表 */
extern USBD_DescriptorsTypeDef FS_Desc;

#ifdef __cplusplus
}
#endif

#endif /* __USBD_DESC_H */
