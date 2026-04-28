# STM32G0B1 USB CDC 实战指南

> 风扇控制板的 CDC 文本协议与安全策略见：`prompt.txt` 的 “§A 固件侧”。

> 目标读者：未来回头维护这份代码的你 / 接手项目的同事 / 在别的板子上复用这套
> USB CDC 方案的人。
>
> 写在前面：这一份文档不是"USB 协议教科书"，而是把这一次实现 USB CDC 时
> **踩过的所有坑**和**最终成功的工程套路**都记下来，下一次遇到同类问题能直接
> 跳过陷阱。文档与代码 1:1 对齐，所有正面示例都来自当前 `src/` 与 `src/usb/`，
> 反面示例标注 ❌，正面示例标注 ✔。
>
> 适用对象：STM32G0B1CBT6（USB_DRD_FS，单机 Device 模式，全速 12 Mbit/s，HSI48）
> 在其他系列（F0/F4/G4/H7）上 **大部分思路通用**，但 PCD/MSP 细节不同，注意
> 区分。

---

## 目录

1. [一句话总结](#一句话总结)
2. [硬件前提与电气](#硬件前提与电气)
3. [软件架构总览](#软件架构总览)
4. [时钟方案：HSI16 + HSI48(USB) + CRS](#时钟方案hsi16--hsi48usb--crs)
5. [中断与并发模型](#中断与并发模型)
6. [PMA 内存布局](#pma-内存布局)
7. [RX/TX 数据通路](#rxtx-数据通路)
8. [描述符与 VID/PID](#描述符与-vidpid)
9. [PlatformIO 工程配置](#platformio-工程配置)
10. [踩坑大全（反面示例）](#踩坑大全反面示例)
11. [调试方法论：LED 打卡 + 二分法](#调试方法论led-打卡--二分法)
12. [上线测试 Checklist](#上线测试-checklist)
13. [常见问题速查](#常见问题速查)
14. [参考资料](#参考资料)

---

## 一句话总结

**STM32G0 系列上，USB CDC 想跑起来必须做对这三件事，缺一不可：**

1. ✅ **HSI48 + CRS 用 USB SOF 自动校准**（USB FS 协议要求时钟精度 ±0.25%，HSI48 出厂仅 ±1%~±2%）
2. ✅ **`HAL_PCD_MspInit` 里必须 `__HAL_RCC_SYSCFG_CLK_ENABLE`**（G0 把 USB/UCPD1/UCPD2 三个中断挂在同一根 IRQn，HAL ISR 入口要读 `SYSCFG->IT_LINE_SR[8]` 辨认；SYSCFG 没时钟 → 寄存器读 0 → ISR 立刻 return → ISTR 标志没清 → 反复触发 → 主循环饿死）
3. ✅ **中断里只搬数据、TX 永不死等**（不阻塞 ISR、不阻塞主循环，是"USB 通信卡死/无限重启"类问题的根治方案）

第 2 点是这次最大的坑，也是 STM32G0 USB 区别于 F4/G4 的特有陷阱。详见
[反面示例 11.1](#111-syscfg-时钟未开-本工程最大坑)。

---

## 硬件前提与电气

### 必备

| 项 | 值 | 说明 |
|---|---|---|
| MCU | STM32G0B1CBT6 | 内置 USB FS 外设（USB_DRD_FS），含 PMA SRAM 2KB |
| USB DP | PA12 | 专用引脚，**不要**当 GPIO 配置 |
| USB DM | PA11 | 同上 |
| USB VBUS | VDD | 本工程为自供电（STLink 或独立电源），不需要 VBUS 检测 |
| 时钟源 | 内部 HSI48 + CRS | **不需要外部晶振**（这是 G0 USB 相比 F1/F4 的一大优点） |

### 常见误判

- ❌ "USB 必须接外部 12/24 MHz 晶振" → **错**。G0 内置 HSI48，配合 CRS 用 USB SOF 自动校准，比外部晶振更省 BOM。
- ❌ "DP 线上需要外接 1.5kΩ 上拉电阻到 +3.3V 才能让主机识别为全速设备" → **错**。STM32G0 内置可控制的 1.5kΩ DP 上拉，由 `USB_BCDR.DPPU` 位控制；HAL 在 `HAL_PCD_Start()` 内会调 `USB_DevConnect()` 自动拉高。**额外的外部上拉只会让信号完整性变差**。
- ❌ "USB 数据线要串 22~33Ω 阻抗匹配电阻" → 在 PCB 阻抗严格控制（90Ω 差分）的板子上是好做法；在原型板上 0Ω 直连也能跑，**但布线长度别超过 10cm，且尽量等长**。

### USB 数据线 vs ST-Link 供电线

调试时一个常见混淆：板子用 ST-Link 供电时，**ST-Link 那条 USB 是接到电脑的，但电脑看到的是 ST-Link 自己**，跟 MCU 上的 PA11/PA12 USB **完全无关**。

要让 PC 识别 MCU 上的 CDC 设备，必须用**第二根独立 USB 线**接到板上 MCU 那个 USB 接口（一般是 USB-C 或 Micro-USB）。

---

## 软件架构总览

```
src/
├── main.c                 ← 应用入口、时钟配置、主循环 echo + LED + CRS 诊断
├── stm32g0xx_it.c         ← 核心异常 + USB IRQ → HAL_PCD_IRQHandler
└── usb/
    ├── usb_device.c/.h    ← MX_USB_Device_Init（USBD_Init/RegisterClass/Start）
    ├── usbd_conf.c/.h     ← PCD ↔ USBD_LL 桥接 + MSP 时钟/NVIC + PMA 布局 + 静态内存池
    ├── usbd_desc.c/.h     ← USB 描述符（设备/字符串/序列号）
    └── usbd_cdc_if.c/.h   ← CDC 应用层（RX 环形缓冲、非阻塞 TX、LineCoding）

lib/STM32_USB_Device/      ← ST 官方 USB Device Library（usbd_core/cdc/ctlreq/ioreq）
```

### 分层职责

```
┌───────────────────────────────────────────────────────┐
│  应用层  main.c                                       │
│   ├── HAL_GetTick 心跳调度                            │
│   ├── CDC_RingPop（消费 RX 环形缓冲）                 │
│   ├── CDC_Transmit_NonBlocking（非阻塞 TX）           │
│   └── update_led_usb / crs_poll / handle_query        │
├───────────────────────────────────────────────────────┤
│  CDC 接口层  usbd_cdc_if.c                            │
│   ├── CDC_Init/DeInit/Control/Receive/TransmitCplt    │
│   ├── RX 环形缓冲（256B）+ 溢出计数                   │
│   ├── LineCoding 7B 副本                              │
│   └── 中断绝不做业务 / TX 绝不死等                    │
├───────────────────────────────────────────────────────┤
│  USB Device 框架层  STM32_USB_Device + usb_device.c   │
│   ├── USBD_Init / USBD_RegisterClass(USBD_CDC)        │
│   ├── USBD_CDC_RegisterInterface(&USBD_Interface_fops)│
│   └── USBD_Start                                      │
├───────────────────────────────────────────────────────┤
│  低层桥接  usbd_conf.c                                │
│   ├── USBD_LL_*  → HAL_PCD_*                          │
│   ├── HAL_PCD_*Callback → USBD_LL_*                   │
│   ├── PMA 布局静态规划                                │
│   ├── HAL_PCD_MspInit：SYSCFG/USB CLK + NVIC          │
│   └── 静态内存池替代 malloc                           │
├───────────────────────────────────────────────────────┤
│  HAL/LL  stm32g0xx_hal_pcd.c + stm32g0xx_ll_usb.c     │
│  外设  USB_DRD_FS + PMA SRAM 2KB                      │
└───────────────────────────────────────────────────────┘
```

**唯一的回调链方向**：

- 主循环 → CDC_Transmit_NonBlocking → USBD_CDC_TransmitPacket → HAL_PCD_EP_Transmit → 写寄存器
- 硬件 IRQ → HAL_PCD_IRQHandler → HAL_PCD_*Callback → USBD_LL_* → CDC_Receive_FS / CDC_TransmitCplt_FS → 写环形缓冲
- 应用层从不主动调 HAL_PCD_*；HAL_PCD_*Callback 也从不主动调应用层

---

## 时钟方案：HSI16 + HSI48(USB) + CRS

### 设计理由

- **SYSCLK 用 HSI16（16MHz）**：G0 上 HSI48 **不能**作为 SYSCLK，只能给 USB；用 HSI16 简单稳定，FLASH 0WS，足够 LED + USB 业务。如果你确实要 64MHz，请用 PLL HSI16×4，但要注意 FLASH WS=2、CRS 仍走 HSI48 独立路径。
- **USBCLK 用 HSI48**：内置高精度 RC 振荡器，48MHz，给 USB 外设作为 48MHz 参考时钟。
- **CRS 用 USB SOF 同步**：USB 主机每 1ms 发一次 SOF（Start-Of-Frame）帧，CRS 把这个 1kHz 信号当 reference 来连续微调 HSI48 的 TRIM 值，把它实际频率拉到 ±50 ppm 以内（远小于 USB FS 要求的 ±2500 ppm）。

### 配置代码（main.c::SystemClock_Config）

```c
/* 1) 同时启用 HSI 和 HSI48 */
RCC_OscInitStruct.OscillatorType      = RCC_OSCILLATORTYPE_HSI | RCC_OSCILLATORTYPE_HSI48;
RCC_OscInitStruct.HSIState            = RCC_HSI_ON;
RCC_OscInitStruct.HSIDiv              = RCC_HSI_DIV1;
RCC_OscInitStruct.HSI48State          = RCC_HSI48_ON;
RCC_OscInitStruct.PLL.PLLState        = RCC_PLL_NONE;
HAL_RCC_OscConfig(&RCC_OscInitStruct);

/* 2) SYSCLK = HSI16，0 WS */
RCC_ClkInitStruct.SYSCLKSource   = RCC_SYSCLKSOURCE_HSI;
RCC_ClkInitStruct.AHBCLKDivider  = RCC_SYSCLK_DIV1;
RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV1;
HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_0);

/* 3) USB 外设时钟源 = HSI48 */
PeriphClkInit.PeriphClockSelection = RCC_PERIPHCLK_USB;
PeriphClkInit.UsbClockSelection    = RCC_USBCLKSOURCE_HSI48;
HAL_RCCEx_PeriphCLKConfig(&PeriphClkInit);

/* 4) CRS：USB SOF 自动校 HSI48
 *    Reload=0xBB7F=47999 (即每帧 48000 个 HSI48 cycle - 1)
 *    Source=USB（专用同步源；G0 还支持 LSE/GPIO） */
__HAL_RCC_CRS_CLK_ENABLE();
RCC_CRSInitStruct.Prescaler             = RCC_CRS_SYNC_DIV1;
RCC_CRSInitStruct.Source                = RCC_CRS_SYNC_SOURCE_USB;
RCC_CRSInitStruct.Polarity              = RCC_CRS_SYNC_POLARITY_RISING;
RCC_CRSInitStruct.ReloadValue           = RCC_CRS_RELOADVALUE_DEFAULT;
RCC_CRSInitStruct.ErrorLimitValue       = RCC_CRS_ERRORLIMIT_DEFAULT;
RCC_CRSInitStruct.HSI48CalibrationValue = RCC_CRS_HSI48CALIBRATION_DEFAULT;
HAL_RCCEx_CRSConfig(&RCC_CRSInitStruct);
```

### 校准是否真的在工作？

代码里加了 `?` 命令：在串口里发 `?\n` 会回一行实时 CRS 状态：

```
CRS TRIM=0x40 SYNCOK=12345 ERR=0 MISS=0 OVF=0
```

判读：

| 字段 | 期望 | 含义 |
|---|---|---|
| `SYNCOK` | 持续 +1000/秒 | 每帧 SOF 都对上了 |
| `ERR` | 0 或个位数 | SYNC 输入与本地差超出 ErrorLimit |
| `MISS` | 0 或个位数 | 应到的 SOF 没到（拔线/挂起会增加） |
| `OVF` | **必须 0** | TRIM 跑到边界 → HSI48 严重偏离，要换源 |
| `TRIM` | 0x40±几 | HSI48 实时微调值，随温度小幅漂动 |

间接证据：**echo 顺畅、不掉包 = HSI48 实际偏差远 < 0.25%（USB FS 阈值）= CRS 一定在工作**。

---

## 中断与并发模型

### 优先级布置

| 中断 | NVIC 优先级 | 设置位置 |
|---|---|---|
| SysTick | 0（最高） | HAL_Init 内自动 |
| **USB_UCPD1_2_IRQn** | 2 | `usbd_conf.c::HAL_PCD_MspInit` |

**Cortex-M0+ 只有 4 级（0~3），数字越小越高**。USB 在 2 是为了**永远不阻塞 SysTick**（保证 `HAL_GetTick()` 准确，是非阻塞 TX 超时机制的基础）。

### 三段式数据流

```
   主机 USB 主控                   STM32G0
       ▼                              ▲
  发 OUT 包 ───→ USB 外设硬件 ───→ ISR (USB_UCPD1_2_IRQHandler)
                                      │
                                      ▼
                          HAL_PCD_DataOutStageCallback
                                      │
                                      ▼
                              USBD_LL_DataOutStage
                                      │
                                      ▼
                                CDC_Receive_FS（仍在 ISR 上下文）
                                      │
                                      ├── 把 *Buf 字节抄进 RX 环形缓冲
                                      ├── cdcEventFlag = 1（提示 LED）
                                      └── 立刻 USBD_CDC_ReceivePacket 重开下一帧
                                              ↑↑↑ 必须同步做，否则 NAK
   主循环（main 上下文）
       │
       ▼
   CDC_RingPop → 业务处理 → CDC_Transmit_NonBlocking → 寄存器写完返回
                                                          ↓
                                                    硬件发完 → ISR
                                                          ↓
                                              CDC_TransmitCplt_FS
                                              (置 hcdc->TxState = 0)
```

### 三条铁律

1. **ISR 内只搬数据，不解析、不发送、不调任何阻塞 API**（`HAL_Delay`、`while (busy)`、`printf` 全禁）。
2. **TX 永不死等**：`CDC_Transmit_NonBlocking` 在 `dev_state != CONFIGURED` 或 `hcdc->TxState != 0` 时立刻返回 `USBD_BUSY`，由调用方决定重试或丢弃。
3. **共享变量加 `volatile`**：`rxRing`/`rxHead`/`rxTail`/`rxOverflowCnt`/`cdcEventFlag` 都被 ISR 与主循环共享，必须 `volatile` 防止编译器优化掉。`rxHead`/`rxTail` 用 16 位单字读写，单核 M0+ 上单字读写即原子，无需锁。

---

## PMA 内存布局

USB_DRD_FS 自带 2KB 的 Packet Memory Area (PMA)，CPU 通过专用总线访问。**所有 EP 缓冲、BTABLE 都必须放在 PMA 内，且互不重叠**。

本工程在 `usbd_conf.c::USBD_LL_Init` 里**手动**规划：

```
PMA 偏移   长度    用途
0x00      0x40   BTABLE   (8 EP × 8B)
0x40      0x40   EP0 OUT  (control, 64B)
0x80      0x40   EP0 IN   (control, 64B)
0xC0      0x40   EP1 IN   (CDC bulk in, 64B)
0x100     0x40   EP1 OUT  (CDC bulk out, 64B)
0x140     0x08   EP2 IN   (CDC interrupt notify, 8B)
─────────────
共占 328B，PMA 还剩 1720B 余量
```

```c
HAL_PCDEx_PMAConfig(&hpcd_USB_DRD_FS, 0x00U, PCD_SNG_BUF, 0x40U);   /* EP0 OUT */
HAL_PCDEx_PMAConfig(&hpcd_USB_DRD_FS, 0x80U, PCD_SNG_BUF, 0x80U);   /* EP0 IN  */
HAL_PCDEx_PMAConfig(&hpcd_USB_DRD_FS, 0x81U, PCD_SNG_BUF, 0xC0U);   /* EP1 IN  */
HAL_PCDEx_PMAConfig(&hpcd_USB_DRD_FS, 0x01U, PCD_SNG_BUF, 0x100U);  /* EP1 OUT */
HAL_PCDEx_PMAConfig(&hpcd_USB_DRD_FS, 0x82U, PCD_SNG_BUF, 0x140U);  /* EP2 IN  */
```

`HAL_PCDEx_PMAConfig` 第二个参数是 EP 地址（bit7=方向：0=OUT/1=IN），第四个参数是 PMA 偏移量（字节）。

### 核对清单

- [ ] BTABLE 大小 = `dev_endpoints × 8`（hpcd.Init.dev_endpoints=8 → BTABLE=64B）
- [ ] 所有 EP 缓冲不与 BTABLE 重叠
- [ ] 所有 EP 缓冲互不重叠
- [ ] 偏移地址必须是奇数地址不可用（HW 要求 word 对齐，最好按 16/32/64 对齐）
- [ ] 总占用 ≤ 2048B

---

## RX/TX 数据通路

### RX 路径（usbd_cdc_if.c）

```c
static int8_t CDC_Receive_FS(uint8_t *Buf, uint32_t *Len)
{
    /* 1. 把 Buf[0..*Len) 抄进环形缓冲，环满则 rxOverflowCnt++ */
    for (uint32_t i = 0; i < *Len; i++) {
        uint16_t next = (rxHead + 1U) % RX_RING_SIZE;
        if (next == rxTail) { rxOverflowCnt++; break; }
        rxRing[rxHead] = Buf[i];
        rxHead = next;
    }
    cdcEventFlag = 1U;

    /* 2. 立刻重开下一帧（关键！否则之后所有包 NAK） */
    USBD_CDC_SetRxBuffer(&hUsbDeviceFS, UserRxBufferFS);
    USBD_CDC_ReceivePacket(&hUsbDeviceFS);
    return USBD_OK;
}
```

主循环：

```c
uint8_t b;
while (CDC_RingPop(&b)) {
    /* ... 业务处理（行解析、命令分发） ... */
}
```

### TX 路径

```c
uint8_t CDC_Transmit_NonBlocking(uint8_t *buf, uint16_t len)
{
    if (hUsbDeviceFS.dev_state != USBD_STATE_CONFIGURED) return USBD_BUSY;

    USBD_CDC_HandleTypeDef *hcdc =
        (USBD_CDC_HandleTypeDef *)hUsbDeviceFS.pClassData;
    if (hcdc == NULL || hcdc->TxState != 0U) return USBD_BUSY;

    USBD_CDC_SetTxBuffer(&hUsbDeviceFS, buf, len);
    return USBD_CDC_TransmitPacket(&hUsbDeviceFS);   /* 立刻返回 */
}
```

主循环按行 echo 时的安全发送：

```c
/* 软件级有限重试：最多等 5 ms 让 TX 空闲，超时就丢 */
uint32_t deadline = HAL_GetTick() + 5U;
while (CDC_Transmit_NonBlocking(lineBuf, lineLen) == USBD_BUSY) {
    if ((int32_t)(HAL_GetTick() - deadline) >= 0) break;
}
```

**为什么有这个 5ms 上限？** 因为 `USBD_BUSY` 多半是上一包还没发完。USB FS 一帧 1ms，5ms 内绝大多数情况下能空出来。如果还忙 → 主机端有问题（比如阻塞读不消费），主循环不能为此卡死。

---

## 描述符与 VID/PID

```c
#define USBD_VID  0x0483   /* ST */
#define USBD_PID  0x5740   /* "STM32 Virtual COM Port" */
```

**为什么沿用 ST 默认？**

- Linux 内核 `cdc_acm` 模块、Windows 10/11 的 `usbser.sys` 都自带这对 VID/PID 的免驱列表
- 设备插上无需安装驱动，直接出现 `/dev/ttyACM0` 或 `COMx`

如果做商用产品，**必须申请自己的 VID**（USB-IF 收 6000 美元/年）；个人/原型阶段可以用 `pid.codes` 提供的免费 PID 命名空间。

### 序列号生成

`usbd_desc.c::USBD_FS_SerialStrDescriptor` 把 STM32 96-bit UID 转 24 字符 hex 串，确保不同板子的 COM 实例可区分（避免 Windows 串口号被同一 PID 的设备复用）。

---

## PlatformIO 工程配置

```ini
[env:nucleo_g0b1re]
platform = ststm32
board = nucleo_g0b1re
framework = stm32cube
board_build.mcu = stm32g0b1cbt6
board_build.flash_size = 128k

build_flags =
    -D USE_HAL_DRIVER
    -D STM32G0B1xx
    -D USBD_USE_CDC
    -I src/usb
    -I lib/STM32_USB_Device/include

; LDF：deep+ 让链接器递归扫描 USB 中间件库的依赖
lib_ldf_mode = deep+
; 不要把库打成 .a 归档，避免 USBD_LL_* 弱符号被剔除
lib_archive = no

upload_protocol = stlink
debug_tool = stlink
monitor_speed = 115200
```

### `lib_archive = no` 的必要性

ST USB Device Library 里 `USBD_LL_*` 系列函数有部分是 `__weak` 默认实现，我们在 `usbd_conf.c` 里提供了真正的实现。**默认 `lib_archive = yes` 会把库打成 `.a` 归档**，链接器只会从归档里"按需"取符号——这时弱符号优先于强符号被取走，导致我们写的 `USBD_LL_Init` 等永远不被调用，USB 启动后无任何枚举反应。

---

## 踩坑大全（反面示例）

按踩过的概率从高到低排，每条都给反面示例和正面写法。

---

### 11.1 ❌ SYSCFG 时钟未开（本工程最大坑）

**症状**：USB 中断启用后立即挂死。具体：USBD_Init/RegisterClass/Start 都返回成功，但 `USBD_Start` 一返回，主循环立刻被无尽中断打断；外部观察是"主循环完全饿死，LED 全黑"。

**根因**：

```c
/* stm32g0xx_hal_pcd.c::HAL_PCD_IRQHandler 入口 */
void HAL_PCD_IRQHandler(PCD_HandleTypeDef *hpcd)
{
  uint32_t wIstr = USB_ReadInterrupts(hpcd->Instance);

  /* G0 把 USB / UCPD1 / UCPD2 三个外设挂在同一个 IRQn=28 上，
   * 用 SYSCFG->IT_LINE_SR[8] 的 bit 来辨认是哪个外设触发 */
  if ((SYSCFG->IT_LINE_SR[8] & (0x1U << 2)) == 0U) {
    return;
  }
  /* ... 真正处理 ... */
}
```

**SYSCFG 时钟没开时，`SYSCFG->IT_LINE_SR[8]` 读取永远为 0** → HAL 误判"不是 USB 中断" → 立刻 `return` → 但是 `USB->ISTR` 标志没被清 → NVIC 立刻重新触发同一中断 → 主循环饿死。

❌ **错误**（漏配 SYSCFG）：

```c
void HAL_PCD_MspInit(PCD_HandleTypeDef *pcdHandle)
{
    if (pcdHandle->Instance == USB_DRD_FS) {
        __HAL_RCC_USB_CLK_ENABLE();   /* 只开 USB，没开 SYSCFG */
        HAL_NVIC_SetPriority(USB_UCPD1_2_IRQn, 2, 0);
        HAL_NVIC_EnableIRQ(USB_UCPD1_2_IRQn);
    }
}
```

✔ **正确**：

```c
void HAL_PCD_MspInit(PCD_HandleTypeDef *pcdHandle)
{
    if (pcdHandle->Instance == USB_DRD_FS) {
        __HAL_RCC_SYSCFG_CLK_ENABLE();   /* ⭐ G0 必须开 */
        __HAL_RCC_USB_CLK_ENABLE();
        HAL_NVIC_SetPriority(USB_UCPD1_2_IRQn, 2, 0);
        HAL_NVIC_EnableIRQ(USB_UCPD1_2_IRQn);
    }
}
```

**适用范围**：STM32G0 全系（共享 IRQn 设计）。F0/F4/G4/H7 的 PCD HAL 不读 SYSCFG，**没这个坑**。

---

### 11.2 ❌ HSI48 当 SYSCLK

❌ **错误**：

```c
RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_HSI48;   /* 编译错或运行时挂 */
```

G0 上 HSI48 是给 USB 专用的时钟分支，**不可作 SYSCLK**。即使强行配，HAL 会拒绝并返回 `HAL_ERROR`。

✔ **正确**：SYSCLK 用 HSI16 或 PLL；USB 单独走 HSI48。

---

### 11.3 ❌ `lib_archive = yes` 导致 USBD_LL_* 弱符号被剔除

❌ **错误**：默认 `platformio.ini`，没显式关 `lib_archive`。

```
[env:xxx]
platform = ststm32
framework = stm32cube
; 默认 lib_archive = yes
```

链接结果：库里的 `__weak USBD_StatusTypeDef USBD_LL_Init(...) { return USBD_FAIL; }` 直接生效；我们写的强符号被链接器忽略。USB 永不枚举。

✔ **正确**：`lib_archive = no`。

---

### 11.4 ❌ 在 ISR 里做协议解析 / 调用阻塞 API

❌ **错误**：

```c
static int8_t CDC_Receive_FS(uint8_t *Buf, uint32_t *Len)
{
    if (Buf[0] == 'P' && Buf[1] == 'I' && Buf[2] == 'N' && Buf[3] == 'G') {
        char ack[] = "PONG\r\n";
        CDC_Transmit_NonBlocking((uint8_t*)ack, 6);   /* ⚠ 在 ISR 里发包！ */
        HAL_Delay(10);                                /* ⚠ ISR 里调 HAL_Delay */
    }
    /* ... */
}
```

后果：
1. ISR 里 `CDC_Transmit_NonBlocking` 可能正好遇到 TxState=1，自旋等待。
2. `HAL_Delay` 等 SysTick 推进；但 SysTick 优先级 = 0，USB IRQ 优先级 = 2，**SysTick 能抢占 USB IRQ**，但是如果 USB IRQ 仍在调 `HAL_Delay`，ISR 就被永久挂起 → 主循环及更高优先级中断都饿死。

✔ **正确**：ISR 只搬数据 + 重开 ReceivePacket，业务全推到主循环。

---

### 11.5 ❌ TX 死等

❌ **错误**：

```c
while (CDC_Transmit_NonBlocking(buf, len) == USBD_BUSY) { /* 死等 */ }
```

主机端不读、或主机断开但 `dev_state` 还没切到 SUSPENDED 时，这个循环可能永久停不下来。

✔ **正确**：超时上限。

```c
uint32_t deadline = HAL_GetTick() + 5U;
while (CDC_Transmit_NonBlocking(buf, len) == USBD_BUSY) {
    if ((int32_t)(HAL_GetTick() - deadline) >= 0) break;
}
```

---

### 11.6 ❌ 忘记重开 ReceivePacket

❌ **错误**：

```c
static int8_t CDC_Receive_FS(uint8_t *Buf, uint32_t *Len)
{
    /* ... 抄数据到环形缓冲 ... */
    return USBD_OK;   /* 忘了重开下一帧！ */
}
```

后果：第一包正常收，之后所有 OUT 包 NAK，主机重传 3 次后报错断开。

✔ **正确**：每次 Receive_FS 末尾必须 `USBD_CDC_ReceivePacket`。

---

### 11.7 ❌ 中断优先级反了（USB 比 SysTick 高）

❌ **错误**：

```c
HAL_NVIC_SetPriority(USB_UCPD1_2_IRQn, 0, 0);   /* USB 抢 SysTick！ */
/* HAL_Init 默认 SysTick 优先级 0 */
```

后果：`HAL_Delay`、超时机制依赖 SysTick；如果 USB ISR 高优先级 + 偶尔耗时（比如处理 SETUP 或 EP_Open），SysTick 会被挡，`HAL_GetTick()` 不动 → 主循环超时机制失效。

✔ **正确**：`HAL_NVIC_SetPriority(USB_UCPD1_2_IRQn, 2, 0)`，比 SysTick(0) 低。

---

### 11.8 ❌ 把 PA11/PA12 当 GPIO 配置

❌ **错误**：

```c
GPIO_InitStruct.Pin   = GPIO_PIN_11 | GPIO_PIN_12;
GPIO_InitStruct.Mode  = GPIO_MODE_AF_PP;
GPIO_InitStruct.Alternate = GPIO_AF0_USB;
HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);   /* 多余 */
```

STM32G0 上 PA11/PA12 是 USB FS 的**专用引脚**，HAL_PCD_Init 内部会自动接到 USB PHY，**不走 GPIO 端口控制器**。多写没坏处但浪费代码；如果**配错 Mode**（比如 Output Push-Pull）会破坏 USB 信号。

✔ **正确**：完全不在 `MX_GPIO_Init` 里碰 PA11/PA12。

---

### 11.9 ❌ 外加 1.5kΩ DP 上拉电阻

❌ **错误**：原理图上 PA12 → 1.5kΩ → 3.3V。

STM32G0 内置软件可控 DP 上拉，由 `USB->BCDR.DPPU` 控制。HAL 在 `HAL_PCD_Start()` 内部自动拉高（通过 `USB_DevConnect`）。

外加上拉电阻的后果：
- 信号边沿被劣化，串扰增加
- 软断开（如 STOP 模式 USB 唤醒前要先 disconnect）失效
- 总线复位检测可能误触

✔ **正确**：直接 PA11/PA12 到 USB 接口（如有阻抗匹配需求可串 22~33Ω，但**不要**对地或对 VCC 加电阻）。

---

### 11.10 ❌ `hpcd.pData` / `pdev->pData` 没双向绑定

❌ **错误**：

```c
USBD_StatusTypeDef USBD_LL_Init(USBD_HandleTypeDef *pdev)
{
    /* 只设 hpcd.pData，没设 pdev->pData */
    hpcd_USB_DRD_FS.pData = pdev;
    /* ... HAL_PCD_Init ... */
}
```

后果：之后 `USBD_LL_Transmit` 等函数用 `pdev->pData` 当 PCD handle，结果是 NULL，调 HAL_PCD_EP_Transmit 立刻 hard fault。

✔ **正确**：双向绑定。

```c
hpcd_USB_DRD_FS.pData = pdev;
pdev->pData           = &hpcd_USB_DRD_FS;
```

---

### 11.11 ❌ 不响应 SET_LINE_CODING

❌ **错误**：

```c
static int8_t CDC_Control_FS(uint8_t cmd, uint8_t *pbuf, uint16_t length)
{
    return USBD_OK;   /* 全部当作不存在的命令吃掉 */
}
```

后果：Windows 大部分串口工具打开 COM 口前会先发 `SET_LINE_CODING` 设波特率，若 stall（USBD_FAIL）→ 用户看到"打开端口失败"。

✔ **正确**：保留 7 字节本地副本即可，无需真改硬件 UART。

---

### 11.12 ❌ 静态内存池忘记复位 used 标志

❌ **错误**：USB 拔出再插入触发 DeInit/Init 序列时，静态池 used 标志没清，导致第二次分配返回 NULL。

```c
void *USBD_static_malloc(uint32_t size)
{
    if (usbd_static_pool_used) return NULL;   /* 只设不清 → 重连后必失败 */
    usbd_static_pool_used = 1U;
    return usbd_static_pool;
}
```

✔ **正确**：

```c
void USBD_static_free(void *p)
{
    if (p == (void *)usbd_static_pool) {
        usbd_static_pool_used = 0U;   /* DeInit 时归还 */
    }
}
```

---

### 11.13 ❌ HAL_Init 之前用 HAL_GPIO

诊断阶段为了"最早期点亮 LED"会想直接调 `HAL_GPIO_WritePin`。但 `HAL_Init` 还没跑、`uwTick` 还没初始化、systick 还没起，而且 GPIO 端口时钟可能没开。

✔ **正确做法**：用裸寄存器，不依赖 HAL：

```c
int main(void)
{
    /* 完全不依赖 HAL，直接寄存器点亮 PA1 */
    RCC->IOPENR |= RCC_IOPENR_GPIOAEN;
    GPIOA->MODER = (GPIOA->MODER & ~(0x3U << 2)) | (0x1U << 2);
    GPIOA->BSRR  = GPIO_BSRR_BS1;

    HAL_Init();
    /* ... 后续才能用 HAL_GPIO_* ... */
}
```

---

### 11.14 ❌ STLink 烧录后忘记断电重启

`pio run -t upload` 默认配置可能让 OpenOCD 把 MCU 留在 halt 状态。如果 LED 不亮，**先做一次完整断电**（拔 USB 再插）再判断，不要一上来就怀疑代码。

```bash
# 烧录后必做：
pio run -t upload
# 拔 STLink USB → 等 2 秒 → 重新插上 → 观察现象
```

---

### 11.15 ❌ 调试时把 `USBD_LL_*` 改了但忘记同步

经常在调试 PMA 或描述符时改 `usbd_conf.c` 里的 PMA offset / dev_endpoints，**重新烧录前一定要 `pio run -t clean`**，不然链接器可能用旧的 .o 文件。

---

### 11.16 ❌ 在 main 里用 `printf("hello\n")` 之类写 USB

`stdio.h::printf` 默认走 newlib 的 `_write` syscall，stm32cube 框架里 `_write` 是空实现。要把 `printf` 重定向到 USB CDC 不仅要 retarget `_write`，还要解决"`printf` 是阻塞的"这个根本问题。

✔ **建议**：直接用 `snprintf` 写到本地 buf，然后 `CDC_Transmit_NonBlocking(buf, n)` 发出去（本工程 `?` 命令就是这套路）。

---

## 调试方法论：LED 打卡 + 二分法

这次 SYSCFG 坑能在 30 分钟内定位，**关键是 LED 打卡 + 二分法**。

### 1. 永远先做"最早期诊断信号"

在 `main()` 第一行用裸寄存器点亮一个 LED，覆盖一切可能的失败点：

```c
int main(void)
{
    RCC->IOPENR |= RCC_IOPENR_GPIOAEN;
    GPIOA->MODER = (GPIOA->MODER & ~(0x3U<<2)) | (0x1U<<2);
    GPIOA->BSRR  = GPIO_BSRR_BS1;
    /* ... */
}
```

如果连这都点不亮，问题在烧录/启动文件/向量表/复位向量/电源；不是你的应用逻辑。

### 2. 错误处理走"快闪"模式区分

```c
void Error_Handler(void)
{
    for (;;) {
        HAL_GPIO_TogglePin(LED_SYS_GPIO_Port, LED_SYS_Pin);
        for (volatile uint32_t i = 0; i < 200000U; i++) __NOP();
    }
}
```

正常 1Hz vs Error 5Hz 一眼可分。

### 3. 复杂初始化用"打卡灯"逐步定位

每过一步关键流程让 LED 短闪一下（200ms 亮 / 200ms 灭），数闪几下能定位到具体哪一步挂死。本次实战中就是用 PA2 闪 N 下定位到了 `USBD_Start` 内部。

### 4. 二分屏蔽 + 二分启用

定位"USB 通信卡死"时的二分思路：

| 步骤 | 做法 | 期望 |
|---|---|---|
| 1 | 全注释 USB → 单 LED 闪 | 验证硬件 + 烧录链路 |
| 2 | 加 USB 库链接但不调 `MX_USB_Device_Init` | 验证全局变量初始化无副作用 |
| 3 | 调用 `MX_USB_Device_Init` 4 步分别打卡 | 定位到哪个 USBD_* 挂 |
| 4 | 临时禁用 NVIC USB IRQ | 区分"主调用挂"还是"中断挂" |
| 5 | 把 `HAL_PCD_*Callback` 改空体 | 区分"PCD HAL 自己有问题"还是"应用层 callback" |

每步只改一个变量，结果异常就锁定到这一步。

---

## 上线测试 Checklist

### 编译与烧录

- [ ] `pio run` 编译干净通过，无 warning（除可忽略的 `-Wunused-parameter` 等）
- [ ] Flash 占用 < 50%，RAM 占用 < 20%（本工程 17832 / 524288，2980 / 147456）
- [ ] `pio run -t upload` 成功，**断电重启**

### 离线（不连 PC）

- [ ] PA1 1Hz 心跳闪
- [ ] PA2 熄灭

### 在线（连 PC）

- [ ] 1~2 秒内枚举完成，PC 设备管理器出现新 COM 口（VID 0483, PID 5740）
- [ ] PA2 变常亮
- [ ] `pio device monitor` 打开端口不报错
- [ ] 发任意行 `\n` 结尾，设备完整 echo `\r\n`
- [ ] 发 `?\n`，回 `CRS TRIM=0xXX SYNCOK=N ERR=N MISS=N OVF=N`
- [ ] 等 5 秒再发 `?\n`，`SYNCOK` 涨幅约 5000
- [ ] 连续高频敲键盘，PA2 可见反相闪烁
- [ ] 拔 USB 数据线，PA2 立刻熄灭，PA1 心跳不停
- [ ] 重新插上，1~2 秒内重新枚举，COM 口名（含序列号）保持一致

### 异常压测

- [ ] 发一次 64 字节以上的长行（如 100 字节），完整 echo
- [ ] 持续高速发送 1 分钟（如脚本 `for i in {1..1000}; do echo "$i" > COMx; done`），无死锁、无掉帧
- [ ] 在打开 COM 的情况下复位 MCU（按 NRST 或断电），PC 端工具能恢复连接

---

## 常见问题速查

| 现象 | 可能原因 | 排查方向 |
|---|---|---|
| LED 全黑（PA1 也不闪） | 主循环饿死 / Reset_Handler 之前挂 | 11.1 SYSCFG / 早期诊断信号 |
| PA1 5Hz 快闪 | 进了 Error_Handler | 时钟配置（HSI48 / CRS / PeriphCLK）|
| PA1 1Hz + PA2 一直熄灭 | USB 没枚举 | 数据线没接 / 主机不识别 / VID/PID 冲突 |
| PA1 1Hz + PA2 常亮，但 echo 不通 | 收发不工作 | 11.6 ReceivePacket / 11.10 pData 双向 |
| COM 口能开但卡顿/掉数据 | TX 阻塞 / 缓冲溢出 | 11.5 TX 死等 / 11.4 ISR 做业务 |
| 拔插后 COM 口名变了 | 序列号缺失 | usbd_desc.c::USBD_FS_SerialStrDescriptor |
| Win10 报"USB 设备无法识别" | 描述符错乱 / 时钟不准 | CRS 状态、PMA 不重叠、描述符长度 |
| 偶发掉线 | HSI48 偏离 | 看 `?` 输出的 ERR/MISS 是否在涨 |
| 主机识别为"未知设备"无 COM | CDC 配置描述符不全 | usbd_cdc.c 的 ConfigDesc 没改 |

---

## 参考资料

- **STM32G0B1 Reference Manual (RM0444)**：第 36 章 USB Device，第 8 章 RCC
- **STM32G0xx HAL Driver 源码**：`stm32g0xx_hal_pcd.c` / `stm32g0xx_ll_usb.c`
- **STMicroelectronics AN5305**：HSI48 oscillator and CRS basics
- **USB 2.0 Spec**：Chapter 5 (Pipes/EPs), Chapter 9 (Standard Requests)
- **USB CDC PSTN 1.20 Spec**：CDC ACM 子类（虚拟串口）
- 本工程的代码：`src/main.c`, `src/usb/*.c/.h`, `lib/STM32_USB_Device/`

---

## 一句话回顾踩坑历程

> 写了所有代码 → 烧上去 LED 全黑
> → 怀疑硬件，写最小验证（348B Flash）→ LED 亮，硬件 OK
> → 加回时钟和 HAL，LED 亮，OK
> → 加回 USB 库但不调用，LED 亮，OK
> → 调用 `MX_USB_Device_Init` 4 步打卡，发现挂在 `USBD_Start`
> → 禁用 USB IRQ 后 `USBD_Start` 能返回 → 锁定中断处理链
> → 把 `HAL_PCD_*Callback` 全改空体，仍挂 → 不在应用 callback，问题更深
> → **看 HAL_PCD_IRQHandler 源码**，发现读 `SYSCFG->IT_LINE_SR[8]`
> → 加 `__HAL_RCC_SYSCFG_CLK_ENABLE()` → 一切正常

整个过程 **没用 SWD 调试器**，纯靠 LED 二分法。下次遇到同类问题直接对号入座即可。
