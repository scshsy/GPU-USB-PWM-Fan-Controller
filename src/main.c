/**
 * @file  main.c
 * @brief 应用装配层 - 启动顺序 + 主循环调度
 *
 * 本文件刻意保持精简：所有具体业务都在独立模块里实现，main.c 只负责
 *   1. 早期诊断点亮（不依赖任何库，证明 CPU 跑到 main）
 *   2. 按正确顺序调用各模块的 Init
 *   3. 主循环周期调用各模块的 Update / Service
 *
 * ┌──────────────┐
 * │ 启动序列      │
 * └──────────────┘
 *   0. DFU_CheckAndJumpEarly - 软复位 magic 命中则跳 ROM bootloader
 *   1. 寄存器级点亮 PA1     - 可视化 main 入口
 *   2. HAL_Init             - SysTick / NVIC / 默认时钟
 *   3. LED_Init             - PA1/PA2 推挽输出
 *   4. Clock_Init           - HSI16 SYSCLK + HSI48 USB + CRS
 *   5. MX_USB_Device_Init   - 启动 USB 设备栈（不阻塞）
 *   6. CLI_Init / DFU_Init  - 注册内置命令（"?" 与 "dfu"）
 *   7.（未来）Pwm_Init / FanCtl_Init / ... 在此处插入
 *
 * ┌──────────────┐
 * │ 主循环调度     │
 * └──────────────┘
 *   每一轮：
 *     - LED_Sys_Update     1Hz 心跳，证明主循环活着
 *     - LED_Usb_Update     未枚举=灭 / 枚举=亮 / 事件=反相闪
 *     - CRS_Poll           轮询 CRS 校准状态
 *     - CLI_Service        消费 USB RX，分发命令 / 默认 echo
 *     - （未来）FanCtl_Service / ProtoCol_Service ...
 *
 * 历史踩坑笔记（2026-04 排障）：
 *   STM32G0 上 USB / UCPD1 / UCPD2 共享 IRQn 28；HAL_PCD_IRQHandler 入口要读
 *   SYSCFG->IT_LINE_SR[8] 辨认是哪个外设触发。SYSCFG 时钟默认关，必须在
 *   HAL_PCD_MspInit 里 __HAL_RCC_SYSCFG_CLK_ENABLE，否则 ISR 读 0 → 立刻 return
 *   而 ISTR 标志未清 → NVIC 反复触发 → main 永久饿死，PA1/PA2 全黑。
 */

#include "stm32g0xx_hal.h"

#include "clock.h"
#include "led.h"
#include "cli.h"
#include "crs_diag.h"
#include "dfu.h"
#include "error.h"
#include "pwm.h"
#include "tach.h"
#include "fanctl.h"
#include "usb_device.h"
#include "usbd_cdc_if.h"

int main(void)
{
    /* === 阶段 -1：DFU 复位 magic 检查（必须最早，几乎零开销） ===
     * 上一轮应用如果收到 "dfu" 命令会写 magic 到 TAMP backup register
     * 然后软复位。此处复位后第一时间检查 → 命中则跳 ROM bootloader
     * （永不返回）。正常上电/复位时 magic 为 0，几条指令开销后继续走。
     */
    DFU_CheckAndJumpEarly();

    /* === 阶段 0：最早期诊断信号 ===
     * 完全不依赖 HAL/时钟，直接寄存器点亮 PA1（上电默认 HSI16 已在跑）
     *   - PA1 完全不亮  → 没跑到 main()（烧录失败 / 启动文件 / 向量表）
     *   - PA1 立刻常亮 → 已进 main()，问题在 HAL_Init 或后续配置
     *   - 后续 LED_Sys_Update 进入 1 Hz 闪烁会自然覆盖此初态
     */
    RCC->IOPENR  |= RCC_IOPENR_GPIOAEN;
    GPIOA->MODER  = (GPIOA->MODER & ~(0x3U << (1U * 2U))) | (0x1U << (1U * 2U));
    GPIOA->BSRR   = GPIO_BSRR_BS1;

    /* === 阶段 1：核心初始化 === */
    HAL_Init();
    LED_Init();
    Clock_Init();

    /* 时钟和 LED 都 OK，把早期诊断信号清掉，从此进入 1 Hz 心跳节奏 */
    LED_Sys_Set(0U);

    /* === 阶段 2：USB 设备栈启动（不阻塞，枚举由后续中断驱动） === */
    MX_USB_Device_Init();

    /* === 阶段 3：应用层模块 === */
    CLI_Init();
    DFU_Init();             /* 注册 "dfu" 命令 → 重启进 ROM bootloader */

    /* === 阶段 4：（预留）后续业务模块在此处 Init，例如：
     *    Pwm_Init();        // PWM 输出
     *    FanCtl_Init();     // 风扇 PID 控制
     *    各模块在自己的 Init 里调 CLI_RegisterHandler 注册私有命令
     */
    Pwm_Init();
    Tach_Init();
    FanCtl_Init();           /* 内部注册 set/get/kick 命令 */

    /* === 主循环：所有任务都是非阻塞的"周期 service"风格 === */
    while (1) {
        uint32_t now = HAL_GetTick();

        /* 心跳和状态指示放最前，保证即使后续 service 拖时也能维持 LED 节拍 */
        LED_Sys_Update(now);
        LED_Usb_Update(now,
                       hUsbDeviceFS.dev_state == USBD_STATE_CONFIGURED,
                       CDC_TakeEventFlag());

        /* 诊断/通信 service */
        CRS_Poll();
        CLI_Service();

        /* 未来扩展点：
         *   FanCtl_Service(now);
         *   TempSensor_Service(now);
         *   Watchdog_Refresh();
         */
        FanCtl_Service(now);
        Tach_Service(now);
    }
}
