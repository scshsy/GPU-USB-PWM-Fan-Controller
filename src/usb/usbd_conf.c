/**
 * @file  usbd_conf.c
 * @brief PCD 与 USB Device Library 之间的低层桥接
 *
 * 设计要点：
 *   1. HAL_PCD 回调全部"转手"到 USBD_LL_* 接口，不在中断里做业务逻辑
 *   2. PMA 布局静态规划，避免动态分配导致的不可预期错误
 *   3. USB 中断优先级 = 2，低于 SysTick (=0)，保证 HAL_GetTick 不会被遮蔽
 *   4. 启动时不阻塞等待主机枚举：USBD_Start 立即返回，dev_state 由后续中断推进
 */

#include "usbd_conf.h"
#include "usbd_core.h"

/* PCD 句柄：被 stm32g0xx_it.c 中的中断处理函数引用 */
PCD_HandleTypeDef hpcd_USB_DRD_FS;

/* ============================================================
 *  HAL_PCD MSP 层：开外设时钟 + NVIC（GPIO 不需要配置，
 *  因为 PA11/PA12 在 STM32G0 上是 USB FS 专用引脚，不走端口控制器）
 *
 *  ⚠ 关键：必须开 SYSCFG 时钟。
 *      STM32G0 把 USB / UCPD1 / UCPD2 三种中断挂在同一根 IRQn 上，
 *      HAL_PCD_IRQHandler 入口读 SYSCFG->IT_LINE_SR[8] 来识别"是不是 USB"。
 *      若 SYSCFG 时钟未启用，该寄存器读 0，HAL 误判后立即 return；
 *      但 USB ISTR 标志没清，NVIC 反复触发同一中断，主循环被永久饿死。
 * ============================================================ */
void HAL_PCD_MspInit(PCD_HandleTypeDef *pcdHandle)
{
    if (pcdHandle->Instance == USB_DRD_FS) {
        __HAL_RCC_SYSCFG_CLK_ENABLE();
        __HAL_RCC_USB_CLK_ENABLE();

        /* USB 中断：优先级 2（M0+ 共 0~3 级），低于 SysTick(=0)
         * → 保证 HAL_GetTick 永远不会被 USB ISR 阻塞 */
        HAL_NVIC_SetPriority(USB_UCPD1_2_IRQn, 2, 0);
        HAL_NVIC_EnableIRQ(USB_UCPD1_2_IRQn);
    }
}

void HAL_PCD_MspDeInit(PCD_HandleTypeDef *pcdHandle)
{
    if (pcdHandle->Instance == USB_DRD_FS) {
        __HAL_RCC_USB_CLK_DISABLE();
        HAL_NVIC_DisableIRQ(USB_UCPD1_2_IRQn);
    }
}

/* ============================================================
 *  HAL_PCD 事件回调 → USBD_LL_* 转发（不在 ISR 内做业务逻辑）
 * ============================================================ */
void HAL_PCD_SetupStageCallback(PCD_HandleTypeDef *hpcd)
{
    USBD_LL_SetupStage((USBD_HandleTypeDef *)hpcd->pData, (uint8_t *)hpcd->Setup);
}

void HAL_PCD_DataOutStageCallback(PCD_HandleTypeDef *hpcd, uint8_t epnum)
{
    USBD_LL_DataOutStage((USBD_HandleTypeDef *)hpcd->pData, epnum, hpcd->OUT_ep[epnum].xfer_buff);
}

void HAL_PCD_DataInStageCallback(PCD_HandleTypeDef *hpcd, uint8_t epnum)
{
    USBD_LL_DataInStage((USBD_HandleTypeDef *)hpcd->pData, epnum, hpcd->IN_ep[epnum].xfer_buff);
}

void HAL_PCD_SOFCallback(PCD_HandleTypeDef *hpcd)
{
    USBD_LL_SOF((USBD_HandleTypeDef *)hpcd->pData);
}

void HAL_PCD_ResetCallback(PCD_HandleTypeDef *hpcd)
{
    /* USB Reset：由主机发起，强制把设备拉回 Default 状态。
     * 全速设备直接报 USBD_SPEED_FULL */
    USBD_LL_SetSpeed((USBD_HandleTypeDef *)hpcd->pData, USBD_SPEED_FULL);
    USBD_LL_Reset((USBD_HandleTypeDef *)hpcd->pData);
}

void HAL_PCD_SuspendCallback(PCD_HandleTypeDef *hpcd)
{
    USBD_LL_Suspend((USBD_HandleTypeDef *)hpcd->pData);
}

void HAL_PCD_ResumeCallback(PCD_HandleTypeDef *hpcd)
{
    USBD_LL_Resume((USBD_HandleTypeDef *)hpcd->pData);
}

void HAL_PCD_ISOOUTIncompleteCallback(PCD_HandleTypeDef *hpcd, uint8_t epnum)
{
    USBD_LL_IsoOUTIncomplete((USBD_HandleTypeDef *)hpcd->pData, epnum);
}

void HAL_PCD_ISOINIncompleteCallback(PCD_HandleTypeDef *hpcd, uint8_t epnum)
{
    USBD_LL_IsoINIncomplete((USBD_HandleTypeDef *)hpcd->pData, epnum);
}

void HAL_PCD_ConnectCallback(PCD_HandleTypeDef *hpcd)
{
    USBD_LL_DevConnected((USBD_HandleTypeDef *)hpcd->pData);
}

void HAL_PCD_DisconnectCallback(PCD_HandleTypeDef *hpcd)
{
    USBD_LL_DevDisconnected((USBD_HandleTypeDef *)hpcd->pData);
}

/* ============================================================
 *  USB Device Library 低层接口实现（被 usbd_core 调用）
 * ============================================================ */

USBD_StatusTypeDef USBD_LL_Init(USBD_HandleTypeDef *pdev)
{
    /* 把 PCD 句柄与 USBD 句柄相互绑定 */
    hpcd_USB_DRD_FS.pData                   = pdev;
    pdev->pData                             = &hpcd_USB_DRD_FS;

    hpcd_USB_DRD_FS.Instance                = USB_DRD_FS;
    hpcd_USB_DRD_FS.Init.dev_endpoints      = 8;
    hpcd_USB_DRD_FS.Init.speed              = PCD_SPEED_FULL;
    hpcd_USB_DRD_FS.Init.phy_itface         = PCD_PHY_EMBEDDED;
    hpcd_USB_DRD_FS.Init.Sof_enable         = DISABLE;  /* 关 SOF 应用回调，省 1kHz 中断 */
    hpcd_USB_DRD_FS.Init.low_power_enable   = DISABLE;
    hpcd_USB_DRD_FS.Init.lpm_enable         = DISABLE;
    hpcd_USB_DRD_FS.Init.battery_charging_enable = DISABLE;  /* 硬件无 VBUS sensing */

    if (HAL_PCD_Init(&hpcd_USB_DRD_FS) != HAL_OK) {
        return USBD_FAIL;
    }

    /* PMA 布局（STM32G0 USB_DRD_FS 共 2KB PMA）：
     *   BTABLE 自 0x00 起占 8 EP × 8 B = 0x40
     *   EP0 OUT 64B @ 0x40
     *   EP0 IN  64B @ 0x80
     *   EP1 IN  64B @ 0xC0   (CDC bulk IN)
     *   EP1 OUT 64B @ 0x100  (CDC bulk OUT)
     *   EP2 IN   8B @ 0x140  (CDC interrupt notify)
     * 任何端点都用 PCD_SNG_BUF（单缓冲），简单稳定。 */
    HAL_PCDEx_PMAConfig(&hpcd_USB_DRD_FS, 0x00U, PCD_SNG_BUF, 0x40U);
    HAL_PCDEx_PMAConfig(&hpcd_USB_DRD_FS, 0x80U, PCD_SNG_BUF, 0x80U);
    HAL_PCDEx_PMAConfig(&hpcd_USB_DRD_FS, 0x81U, PCD_SNG_BUF, 0xC0U);
    HAL_PCDEx_PMAConfig(&hpcd_USB_DRD_FS, 0x01U, PCD_SNG_BUF, 0x100U);
    HAL_PCDEx_PMAConfig(&hpcd_USB_DRD_FS, 0x82U, PCD_SNG_BUF, 0x140U);

    return USBD_OK;
}

USBD_StatusTypeDef USBD_LL_DeInit(USBD_HandleTypeDef *pdev)
{
    HAL_StatusTypeDef st = HAL_PCD_DeInit(pdev->pData);
    return (st == HAL_OK) ? USBD_OK : USBD_FAIL;
}

USBD_StatusTypeDef USBD_LL_Start(USBD_HandleTypeDef *pdev)
{
    /* HAL_PCD_Start 内部会通过 BCDR.DPPU 启用 USB 外设内置的 1.5kΩ DP 上拉，
     * 主机随即检测到 DP 高电平并发起枚举。 */
    HAL_StatusTypeDef st = HAL_PCD_Start(pdev->pData);
    return (st == HAL_OK) ? USBD_OK : USBD_FAIL;
}

USBD_StatusTypeDef USBD_LL_Stop(USBD_HandleTypeDef *pdev)
{
    HAL_StatusTypeDef st = HAL_PCD_Stop(pdev->pData);
    return (st == HAL_OK) ? USBD_OK : USBD_FAIL;
}

USBD_StatusTypeDef USBD_LL_OpenEP(USBD_HandleTypeDef *pdev, uint8_t ep_addr,
                                  uint8_t ep_type, uint16_t ep_mps)
{
    HAL_StatusTypeDef st = HAL_PCD_EP_Open(pdev->pData, ep_addr, ep_mps, ep_type);
    return (st == HAL_OK) ? USBD_OK : USBD_FAIL;
}

USBD_StatusTypeDef USBD_LL_CloseEP(USBD_HandleTypeDef *pdev, uint8_t ep_addr)
{
    HAL_StatusTypeDef st = HAL_PCD_EP_Close(pdev->pData, ep_addr);
    return (st == HAL_OK) ? USBD_OK : USBD_FAIL;
}

USBD_StatusTypeDef USBD_LL_FlushEP(USBD_HandleTypeDef *pdev, uint8_t ep_addr)
{
    HAL_StatusTypeDef st = HAL_PCD_EP_Flush(pdev->pData, ep_addr);
    return (st == HAL_OK) ? USBD_OK : USBD_FAIL;
}

USBD_StatusTypeDef USBD_LL_StallEP(USBD_HandleTypeDef *pdev, uint8_t ep_addr)
{
    HAL_StatusTypeDef st = HAL_PCD_EP_SetStall(pdev->pData, ep_addr);
    return (st == HAL_OK) ? USBD_OK : USBD_FAIL;
}

USBD_StatusTypeDef USBD_LL_ClearStallEP(USBD_HandleTypeDef *pdev, uint8_t ep_addr)
{
    HAL_StatusTypeDef st = HAL_PCD_EP_ClrStall(pdev->pData, ep_addr);
    return (st == HAL_OK) ? USBD_OK : USBD_FAIL;
}

uint8_t USBD_LL_IsStallEP(USBD_HandleTypeDef *pdev, uint8_t ep_addr)
{
    PCD_HandleTypeDef *hpcd = (PCD_HandleTypeDef *)pdev->pData;
    if ((ep_addr & 0x80U) == 0x80U) {
        return hpcd->IN_ep[ep_addr & 0x7FU].is_stall;
    }
    return hpcd->OUT_ep[ep_addr & 0x7FU].is_stall;
}

USBD_StatusTypeDef USBD_LL_SetUSBAddress(USBD_HandleTypeDef *pdev, uint8_t dev_addr)
{
    HAL_StatusTypeDef st = HAL_PCD_SetAddress(pdev->pData, dev_addr);
    return (st == HAL_OK) ? USBD_OK : USBD_FAIL;
}

USBD_StatusTypeDef USBD_LL_Transmit(USBD_HandleTypeDef *pdev, uint8_t ep_addr,
                                    uint8_t *pbuf, uint32_t size)
{
    HAL_StatusTypeDef st = HAL_PCD_EP_Transmit(pdev->pData, ep_addr, pbuf, size);
    return (st == HAL_OK) ? USBD_OK : USBD_FAIL;
}

USBD_StatusTypeDef USBD_LL_PrepareReceive(USBD_HandleTypeDef *pdev, uint8_t ep_addr,
                                          uint8_t *pbuf, uint32_t size)
{
    HAL_StatusTypeDef st = HAL_PCD_EP_Receive(pdev->pData, ep_addr, pbuf, size);
    return (st == HAL_OK) ? USBD_OK : USBD_FAIL;
}

uint32_t USBD_LL_GetRxDataSize(USBD_HandleTypeDef *pdev, uint8_t ep_addr)
{
    return HAL_PCD_EP_GetRxCount(pdev->pData, ep_addr);
}

void USBD_LL_Delay(uint32_t Delay)
{
    HAL_Delay(Delay);
}

/* ============================================================
 *  静态内存池：USB Device Library 通过 USBD_malloc/free 申请
 *  USBD_CDC_HandleTypeDef 仅一份，用编译期最大尺寸保证够用
 * ============================================================ */
#include "usbd_cdc.h"
#define USBD_STATIC_POOL_SIZE  (sizeof(USBD_CDC_HandleTypeDef) + 16U)

static uint32_t usbd_static_pool[(USBD_STATIC_POOL_SIZE + 3U) / 4U];
static uint8_t  usbd_static_pool_used = 0U;

void *USBD_static_malloc(uint32_t size)
{
    /* 单分配池：当前实现仅 CDC 一种类，调用一次即够。
     * size 校验仅做防御，超出立即返回 NULL → 上层会 USBD_FAIL */
    if (size > USBD_STATIC_POOL_SIZE || usbd_static_pool_used) {
        return NULL;
    }
    usbd_static_pool_used = 1U;
    return (void *)usbd_static_pool;
}

void USBD_static_free(void *p)
{
    if (p == (void *)usbd_static_pool) {
        usbd_static_pool_used = 0U;
    }
}
