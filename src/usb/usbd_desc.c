/**
 * @file  usbd_desc.c
 * @brief USB CDC 设备描述符 + 字符串描述符
 *
 * VID/PID 沿用 ST 默认 0483:5740（"STM32 Virtual COM Port"），
 * 这样 Linux 内核 cdc_acm 与 Windows 的 usbser.sys 都能直接识别，
 * 无需额外驱动安装。
 *
 * Serial Number 由 STM32 96-bit UID 转 24 字符 hex 串生成，
 * 保证不同板子枚举为不同 COM 设备实例。
 */

#include "usbd_desc.h"
#include "usbd_conf.h"
#include "usbd_core.h"

#define USBD_VID              0x0483
#define USBD_PID              0x5740
#define USBD_LANGID_STRING    0x0409  /* 英文（美国） */
#define USBD_MANUFACTURER_STR "Gomez"
#define USBD_PRODUCT_STR      "USB PWM Fan Ctrl"
#define USBD_CONFIG_STR       "CDC Config"
#define USBD_INTERFACE_STR    "CDC Interface"

/* ===== 描述符回调原型 ===== */
static uint8_t *USBD_FS_DeviceDescriptor(USBD_SpeedTypeDef speed, uint16_t *length);
static uint8_t *USBD_FS_LangIDStrDescriptor(USBD_SpeedTypeDef speed, uint16_t *length);
static uint8_t *USBD_FS_ManufacturerStrDescriptor(USBD_SpeedTypeDef speed, uint16_t *length);
static uint8_t *USBD_FS_ProductStrDescriptor(USBD_SpeedTypeDef speed, uint16_t *length);
static uint8_t *USBD_FS_SerialStrDescriptor(USBD_SpeedTypeDef speed, uint16_t *length);
static uint8_t *USBD_FS_ConfigStrDescriptor(USBD_SpeedTypeDef speed, uint16_t *length);
static uint8_t *USBD_FS_InterfaceStrDescriptor(USBD_SpeedTypeDef speed, uint16_t *length);

USBD_DescriptorsTypeDef FS_Desc = {
    USBD_FS_DeviceDescriptor,
    USBD_FS_LangIDStrDescriptor,
    USBD_FS_ManufacturerStrDescriptor,
    USBD_FS_ProductStrDescriptor,
    USBD_FS_SerialStrDescriptor,
    USBD_FS_ConfigStrDescriptor,
    USBD_FS_InterfaceStrDescriptor,
};

/* ===== 设备描述符（18 字节） ===== */
__ALIGN_BEGIN static uint8_t USBD_FS_DeviceDesc[USB_LEN_DEV_DESC] __ALIGN_END = {
    0x12,                       /* bLength */
    USB_DESC_TYPE_DEVICE,       /* bDescriptorType (DEVICE) */
    0x00, 0x02,                 /* bcdUSB 2.00 */
    0x02,                       /* bDeviceClass: CDC */
    0x02,                       /* bDeviceSubClass: ACM */
    0x00,                       /* bDeviceProtocol */
    USB_MAX_EP0_SIZE,           /* bMaxPacketSize0 (64) */
    LOBYTE(USBD_VID), HIBYTE(USBD_VID),
    LOBYTE(USBD_PID), HIBYTE(USBD_PID),
    0x00, 0x02,                 /* bcdDevice 2.00 */
    USBD_IDX_MFC_STR,
    USBD_IDX_PRODUCT_STR,
    USBD_IDX_SERIAL_STR,
    USBD_MAX_NUM_CONFIGURATION
};

/* LangID 描述符 */
__ALIGN_BEGIN static uint8_t USBD_LangIDDesc[USB_LEN_LANGID_STR_DESC] __ALIGN_END = {
    USB_LEN_LANGID_STR_DESC,
    USB_DESC_TYPE_STRING,
    LOBYTE(USBD_LANGID_STRING),
    HIBYTE(USBD_LANGID_STRING),
};

/* 字符串描述符共用缓冲（USBD_GetString 写入），使用 4 字节对齐 */
__ALIGN_BEGIN static uint8_t USBD_StrDesc[USBD_MAX_STR_DESC_SIZ] __ALIGN_END;

/* Serial 缓冲：26 = 2 字节描述符头 + 24 个 hex 字符（不再 UTF-16，
 * 而是直接每字节占 2 字节，由 USBD_GetString 处理 ASCII→UTF-16） */
#define USBD_SERIAL_HEX_LEN 25
static char usbd_serial_hex[USBD_SERIAL_HEX_LEN];

/* ===== 工具：把 UID 拼成 24 个十六进制字符 + '\0' ===== */
static void int_to_unicode(uint32_t value, char *pbuf, uint8_t len)
{
    for (uint8_t idx = 0; idx < len; idx++) {
        uint8_t nibble = (value >> 28) & 0x0FU;
        pbuf[idx] = (nibble < 10U) ? ('0' + nibble) : ('A' + nibble - 10U);
        value <<= 4;
    }
}

static void Get_SerialNum(void)
{
    uint32_t deviceserial0 = *(uint32_t *)(UID_BASE);
    uint32_t deviceserial1 = *(uint32_t *)(UID_BASE + 4U);
    uint32_t deviceserial2 = *(uint32_t *)(UID_BASE + 8U);

    /* 三段 UID 各取 8 个 hex，共 24 个字符 */
    int_to_unicode(deviceserial0, &usbd_serial_hex[0],  8);
    int_to_unicode(deviceserial1, &usbd_serial_hex[8],  8);
    int_to_unicode(deviceserial2, &usbd_serial_hex[16], 8);
    usbd_serial_hex[24] = '\0';
}

/* ===== 描述符回调 ===== */
static uint8_t *USBD_FS_DeviceDescriptor(USBD_SpeedTypeDef speed, uint16_t *length)
{
    (void)speed;
    *length = sizeof(USBD_FS_DeviceDesc);
    return USBD_FS_DeviceDesc;
}

static uint8_t *USBD_FS_LangIDStrDescriptor(USBD_SpeedTypeDef speed, uint16_t *length)
{
    (void)speed;
    *length = sizeof(USBD_LangIDDesc);
    return USBD_LangIDDesc;
}

static uint8_t *USBD_FS_ManufacturerStrDescriptor(USBD_SpeedTypeDef speed, uint16_t *length)
{
    USBD_GetString((uint8_t *)USBD_MANUFACTURER_STR, USBD_StrDesc, length);
    (void)speed;
    return USBD_StrDesc;
}

static uint8_t *USBD_FS_ProductStrDescriptor(USBD_SpeedTypeDef speed, uint16_t *length)
{
    USBD_GetString((uint8_t *)USBD_PRODUCT_STR, USBD_StrDesc, length);
    (void)speed;
    return USBD_StrDesc;
}

static uint8_t *USBD_FS_SerialStrDescriptor(USBD_SpeedTypeDef speed, uint16_t *length)
{
    Get_SerialNum();
    USBD_GetString((uint8_t *)usbd_serial_hex, USBD_StrDesc, length);
    (void)speed;
    return USBD_StrDesc;
}

static uint8_t *USBD_FS_ConfigStrDescriptor(USBD_SpeedTypeDef speed, uint16_t *length)
{
    USBD_GetString((uint8_t *)USBD_CONFIG_STR, USBD_StrDesc, length);
    (void)speed;
    return USBD_StrDesc;
}

static uint8_t *USBD_FS_InterfaceStrDescriptor(USBD_SpeedTypeDef speed, uint16_t *length)
{
    USBD_GetString((uint8_t *)USBD_INTERFACE_STR, USBD_StrDesc, length);
    (void)speed;
    return USBD_StrDesc;
}
