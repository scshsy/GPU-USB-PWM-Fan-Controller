/**
 * @file  dfu.c
 * @brief DFU 模块实现 - 见 dfu.h 设计说明
 *
 * 关键技术点：
 *
 * 1) magic 存储位置 = TAMP->BKP0R
 *    - 直接寄存器访问，不依赖任何 HAL 函数（早期跳转时 HAL 还没初始化）
 *    - 读不需要 PWR_CR1_DBP；写需要 DBP=1
 *
 * 2) 时钟前置条件
 *    - 访问 TAMP 必须先开 RCC_APBENR1.RTCAPBEN
 *    - 写 BKP 必须先开 RCC_APBENR1.PWREN，且把 PWR_CR1.DBP 置 1
 *
 * 3) 跳转 ROM bootloader 之前要做的事
 *    - 关全局中断（防止 PCD 中断打扰 ROM）
 *    - 关 SysTick（防止 ROM 启动后 1ms 误触我们的 SysTick_Handler）
 *    - SYSCFG MEM_MODE=01 把 system memory 映射到 0x00000000，
 *      取 SP/PC 时硬件认 0x00000000；ROM bootloader 内部假设此映射
 *    - __set_MSP + 通过函数指针跳到 ROM Reset_Handler
 *
 * 4) ROM bootloader 的 USB DFU
 *    - VID:0483 PID:DF11
 *    - 用 PA11/PA12（和我们的 USB 相同管脚）
 *    - 内置 1.5kΩ DP 上拉，自动控制
 *    - dfu-util `:leave` 后缀让 ROM 烧完自动跳应用
 */

#include <string.h>

#include "stm32g0xx_hal.h"
#include "dfu.h"
#include "cli.h"

/* ===========================================================================
 * 编译期常量
 * ========================================================================= */

/* magic 任意非零 32-bit；越特殊越好（避开 0xFFFFFFFF / 0x00000000 等常见值）
 * 'DFU 启动' 的 ASCII：D=0x44 F=0x46 U=0x55 - 取 0xDF 0xB0 0x07 0x10 = "DFU BOOT 1.0"
 */
#define DFU_MAGIC_VALUE     0xDFB00710UL

/* STM32G0 system memory（ROM bootloader）入口 - 见 RM0444 / AN2606 */
#define DFU_SYSMEM_BASE     0x1FFF0000UL

/* 等待 USB BULK-IN 真正把响应包发出去的延时；典型 < 1ms，加余量 */
#define DFU_REPLY_DRAIN_MS  100U

/* CLI 命令名（小写，3 字符精确匹配） */
#define DFU_CMD_STR         "dfu"
#define DFU_CMD_LEN         3U

/* ===========================================================================
 * 早期检测 + 跳转
 * ========================================================================= */

void DFU_CheckAndJumpEarly(void)
{
    /* 开 PWR + RTCAPB 时钟，让 TAMP 寄存器可访问。
     * 这两个 enable bit 上电默认为 0；多次写不影响后续 HAL_Init。 */
    RCC->APBENR1 |= RCC_APBENR1_PWREN | RCC_APBENR1_RTCAPBEN;
    /* 屏障：确保后续读 TAMP->BKP0R 之前时钟已开 */
    __DSB();

    if (TAMP->BKP0R != DFU_MAGIC_VALUE) {
        return;     /* 正常上电/复位路径，几条指令开销 */
    }

    /* 命中 magic：清掉自己（写 BKP 需要 DBP=1）防止下次复位还跳 DFU */
    PWR->CR1 |= PWR_CR1_DBP;
    TAMP->BKP0R = 0U;
    __DSB();

    /* === 切换到 ROM bootloader === */

    /* 1) 屏蔽中断，防止 PCD/SysTick 残留状态打扰 ROM */
    __disable_irq();

    /* 2) SysTick 完全关停（HAL_Init 还没被调用，理论上不会启用，但保险） */
    SysTick->CTRL = 0U;
    SysTick->LOAD = 0U;
    SysTick->VAL  = 0U;

    /* 3) SYSCFG remap：把 system memory 映射到 0x00000000
     *    ROM bootloader 通过 0x00000000 取自己的 SP/PC，必须先 remap。
     *    注意先打开 SYSCFG 时钟。 */
    RCC->APBENR2 |= RCC_APBENR2_SYSCFGEN;
    __DSB();
    SYSCFG->CFGR1 = (SYSCFG->CFGR1 & ~SYSCFG_CFGR1_MEM_MODE_Msk)
                  | (0x1UL << SYSCFG_CFGR1_MEM_MODE_Pos);
    __DSB();

    /* 4) 取 ROM bootloader 的 SP（0x1FFF0000 处）和 Reset_Handler（0x1FFF0004 处） */
    uint32_t bl_sp = *(volatile uint32_t *)(DFU_SYSMEM_BASE);
    uint32_t bl_pc = *(volatile uint32_t *)(DFU_SYSMEM_BASE + 4U);

    /* 5) 切栈 + 跳转。__set_MSP 必须在跳转前一刻，避免后续局部变量分配 */
    __set_MSP(bl_sp);
    /* 函数指针跳转；不会返回 */
    void (*bl_entry)(void) = (void (*)(void))bl_pc;
    bl_entry();

    /* 不可达；保留死循环防御编译器优化误判 */
    while (1) { __NOP(); }
}

/* ===========================================================================
 * 触发 reset 进 DFU
 * ========================================================================= */

void DFU_RebootToBootloader(void)
{
    /* 写 magic 前必须使能 backup 域写入 */
    RCC->APBENR1 |= RCC_APBENR1_PWREN | RCC_APBENR1_RTCAPBEN;
    __DSB();
    PWR->CR1   |= PWR_CR1_DBP;
    TAMP->BKP0R = DFU_MAGIC_VALUE;
    __DSB();

    /* AIRCR.SYSRESETREQ：触发系统级软复位（不复位 backup 域，magic 保留） */
    NVIC_SystemReset();

    /* 不可达 */
    while (1) { __NOP(); }
}

/* ===========================================================================
 * CLI handler
 *
 * 严格只匹配 len==3 && line=="dfu"。其他类似 "dfu " / "DFU" / "dfut" 都不匹配
 * （让默认 echo 帮忙暴露用户的拼写错误）。
 * ========================================================================= */
static int dfu_cli_handler(const uint8_t *line, uint16_t len)
{
    if (len != DFU_CMD_LEN || memcmp(line, DFU_CMD_STR, DFU_CMD_LEN) != 0) {
        return -1;
    }

    /* 给主机一个明确的响应，便于 Python 脚本据此判断"已收到命令"
     * 注意：响应必须在 reset 之前真正离开 PMA → USB 总线 */
    static const char reply[] = "entering DFU\r\n";
    CLI_SendBytes((const uint8_t *)reply, (uint16_t)(sizeof(reply) - 1U));

    /* 阻塞 ~100ms 等待 BULK-IN 包飞出去。
     * 此阶段已经决定要 reset，不再回主循环，阻塞主循环没有副作用。 */
    HAL_Delay(DFU_REPLY_DRAIN_MS);

    DFU_RebootToBootloader();   /* 永不返回 */
    return 0;                   /* 形式上的返回，到不了 */
}

void DFU_Init(void)
{
    CLI_RegisterHandler(dfu_cli_handler);
}
