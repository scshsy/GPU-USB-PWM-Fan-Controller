/**
 * @file  usbd_cdc_if.c
 * @brief CDC 接口实现 - 健壮性核心模块
 *
 * 设计要点（针对常见 USB CDC 卡死/丢包场景）：
 *
 *   1. RX 路径只搬数据：
 *      CDC_Receive_FS() 仅把字节塞入应用层环形缓冲，**立即** 调用
 *      USBD_CDC_ReceivePacket() 释放下一帧。绝对不在中断里做协议解析、
 *      不做发送、不调用任何阻塞 API。
 *
 *   2. RX 环形缓冲带溢出计数：
 *      head 追上 tail 时丢弃新字节（不阻塞 ISR、不覆盖旧数据），
 *      rxOverflowCnt 自增，便于调试观察。
 *
 *   3. TX 路径绝不死等：
 *      CDC_Transmit_NonBlocking() 先判 dev_state == CONFIGURED，再判
 *      hcdc->TxState == 0；任一不满足直接返回 USBD_BUSY，由调用者决定
 *      重试还是丢弃。
 *
 *   4. SET/GET_LINE_CODING 软响应：
 *      Linux/Windows 打开虚拟串口时必发 SET_LINE_CODING，不响应会导致
 *      打开失败。本实现保留 7 字节本地副本即可，不真正改 UART 行为。
 *
 *   5. dev_state 与 pClassData 的访问顺序：
 *      pClassData 在 USBD_CDC_DeInit 时会被释放为 NULL，先判空再用。
 */

#include "usbd_cdc_if.h"
#include "usbd_cdc.h"
#include "usbd_def.h"

/* ============================================================
 *  USB CDC 单帧缓冲（必须独立于环形缓冲，由 USB 库直接 DMA 进出）
 * ============================================================ */
#define APP_RX_DATA_SIZE  CDC_DATA_FS_OUT_PACKET_SIZE   /* 64 */
#define APP_TX_DATA_SIZE  CDC_DATA_FS_IN_PACKET_SIZE    /* 64 */

static uint8_t UserRxBufferFS[APP_RX_DATA_SIZE];
static uint8_t UserTxBufferFS[APP_TX_DATA_SIZE];

/* ============================================================
 *  应用层 RX 环形缓冲（中断写、主循环读）
 *  256 字节足以缓冲多包未消费的字节流，对小命令足够
 * ============================================================ */
#define RX_RING_SIZE  256U
static volatile uint8_t  rxRing[RX_RING_SIZE];
static volatile uint16_t rxHead;     /* 写指针：CDC_Receive_FS 中断写入 */
static volatile uint16_t rxTail;     /* 读指针：CDC_RingPop 主循环读出 */
static volatile uint32_t rxOverflowCnt;

/* RX/TX 事件标志（主循环消费一次即清零，驱动 LED 闪烁） */
static volatile uint8_t  cdcEventFlag;

/* ============================================================
 *  Line coding：CDC 标准要求维护 7 字节副本
 *  bitrate(4) | format(1) | paritytype(1) | datatype(1)
 *  默认 115200, 8N1（与 monitor_speed 一致，方便观感）
 * ============================================================ */
static USBD_CDC_LineCodingTypeDef LineCoding = {
    115200U, 0x00U, 0x00U, 0x08U
};

/* ============================================================
 *  CDC 接口回调（注册到 USBD_CDC）
 * ============================================================ */
static int8_t CDC_Init_FS(void);
static int8_t CDC_DeInit_FS(void);
static int8_t CDC_Control_FS(uint8_t cmd, uint8_t *pbuf, uint16_t length);
static int8_t CDC_Receive_FS(uint8_t *Buf, uint32_t *Len);
static int8_t CDC_TransmitCplt_FS(uint8_t *Buf, uint32_t *Len, uint8_t epnum);

USBD_CDC_ItfTypeDef USBD_Interface_fops_FS = {
    CDC_Init_FS,
    CDC_DeInit_FS,
    CDC_Control_FS,
    CDC_Receive_FS,
    CDC_TransmitCplt_FS,
};

/* ============================================================
 *  Init：USB 配置完成后由 USBD_CDC_Setup 调用一次
 * ============================================================ */
static int8_t CDC_Init_FS(void)
{
    USBD_CDC_SetTxBuffer(&hUsbDeviceFS, UserTxBufferFS, 0U);
    USBD_CDC_SetRxBuffer(&hUsbDeviceFS, UserRxBufferFS);

    /* 复位环形缓冲（断电重连场景） */
    rxHead = 0U;
    rxTail = 0U;
    rxOverflowCnt = 0U;
    cdcEventFlag = 0U;

    /* 启动首次接收：之后每包结束都要在 CDC_Receive_FS 末尾再次调用 */
    USBD_CDC_ReceivePacket(&hUsbDeviceFS);
    return USBD_OK;
}

static int8_t CDC_DeInit_FS(void)
{
    return USBD_OK;
}

/* ============================================================
 *  Control：处理 CDC 控制类请求
 *  只对 SET/GET_LINE_CODING 做实质响应，其他命令静默 ACK
 * ============================================================ */
static int8_t CDC_Control_FS(uint8_t cmd, uint8_t *pbuf, uint16_t length)
{
    switch (cmd) {
    case CDC_SET_LINE_CODING:
        /* 主机下发 7 字节，保存本地副本即可（本工程不真正配置 UART） */
        if (length >= 7U) {
            LineCoding.bitrate    = (uint32_t)pbuf[0]
                                  | ((uint32_t)pbuf[1] << 8)
                                  | ((uint32_t)pbuf[2] << 16)
                                  | ((uint32_t)pbuf[3] << 24);
            LineCoding.format     = pbuf[4];
            LineCoding.paritytype = pbuf[5];
            LineCoding.datatype   = pbuf[6];
        }
        break;

    case CDC_GET_LINE_CODING:
        /* 主机查询：必须按相同 7 字节格式回填 */
        pbuf[0] = (uint8_t)(LineCoding.bitrate);
        pbuf[1] = (uint8_t)(LineCoding.bitrate >> 8);
        pbuf[2] = (uint8_t)(LineCoding.bitrate >> 16);
        pbuf[3] = (uint8_t)(LineCoding.bitrate >> 24);
        pbuf[4] = LineCoding.format;
        pbuf[5] = LineCoding.paritytype;
        pbuf[6] = LineCoding.datatype;
        break;

    case CDC_SET_CONTROL_LINE_STATE:
    case CDC_SEND_BREAK:
    case CDC_SEND_ENCAPSULATED_COMMAND:
    case CDC_GET_ENCAPSULATED_RESPONSE:
    case CDC_SET_COMM_FEATURE:
    case CDC_GET_COMM_FEATURE:
    case CDC_CLEAR_COMM_FEATURE:
    default:
        /* 静默 ACK，不影响枚举 */
        break;
    }

    (void)length;
    return USBD_OK;
}

/* ============================================================
 *  Receive：USB ISR 上下文，已收到一包数据
 *  ⚠ 严格规则：只搬数据 + 立即 ReceivePacket，绝不做其他事
 * ============================================================ */
static int8_t CDC_Receive_FS(uint8_t *Buf, uint32_t *Len)
{
    uint16_t head = rxHead;
    uint16_t tail = rxTail;
    uint32_t n    = *Len;

    for (uint32_t i = 0; i < n; i++) {
        uint16_t next = (uint16_t)((head + 1U) % RX_RING_SIZE);
        if (next == tail) {
            /* 环满 → 丢弃新数据，自增溢出计数（由主循环监控） */
            rxOverflowCnt++;
            break;
        }
        rxRing[head] = Buf[i];
        head = next;
    }
    rxHead = head;

    /* 通知主循环有 RX 事件 */
    cdcEventFlag = 1U;

    /* 重置 RX 缓冲并立刻重新打开下一帧接收，否则主机后续包会全部 NAK */
    USBD_CDC_SetRxBuffer(&hUsbDeviceFS, UserRxBufferFS);
    USBD_CDC_ReceivePacket(&hUsbDeviceFS);
    return USBD_OK;
}

/* ============================================================
 *  TransmitCplt：USB ISR 上下文，本帧 IN 端点已发送完成
 * ============================================================ */
static int8_t CDC_TransmitCplt_FS(uint8_t *Buf, uint32_t *Len, uint8_t epnum)
{
    (void)Buf;
    (void)Len;
    (void)epnum;
    cdcEventFlag = 1U;
    return USBD_OK;
}

/* ============================================================
 *  应用层 API
 * ============================================================ */

uint8_t CDC_RingPop(uint8_t *out)
{
    if (rxHead == rxTail) {
        return 0U;   /* 空 */
    }
    *out = rxRing[rxTail];
    rxTail = (uint16_t)((rxTail + 1U) % RX_RING_SIZE);
    return 1U;
}

uint8_t CDC_Transmit_NonBlocking(uint8_t *buf, uint16_t len)
{
    /* 第一道闸：USB 必须已枚举到 Configured，否则直接拒绝 */
    if (hUsbDeviceFS.dev_state != USBD_STATE_CONFIGURED) {
        return USBD_BUSY;
    }

    /* 第二道闸：CDC 上一包必须已发完。pClassData 可能在 DeInit 时为 NULL */
    USBD_CDC_HandleTypeDef *hcdc =
        (USBD_CDC_HandleTypeDef *)hUsbDeviceFS.pClassData;
    if (hcdc == NULL || hcdc->TxState != 0U) {
        return USBD_BUSY;
    }

    /* 安全推送：USBD_CDC_TransmitPacket 只设置寄存器，不阻塞 */
    USBD_CDC_SetTxBuffer(&hUsbDeviceFS, buf, len);
    return USBD_CDC_TransmitPacket(&hUsbDeviceFS);
}

uint32_t CDC_GetRxOverflowCount(void)
{
    return rxOverflowCnt;
}

uint8_t CDC_TakeEventFlag(void)
{
    /* 简单"读取并清零"，主循环单线程消费，无需原子操作 */
    if (cdcEventFlag) {
        cdcEventFlag = 0U;
        return 1U;
    }
    return 0U;
}
