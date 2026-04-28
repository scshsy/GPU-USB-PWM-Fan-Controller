# USB DFU 升级 - 详细参考手册

> **日常使用看简洁版** → [USB_DFU.md](USB_DFU.md)（一页纸搞定）
> 本文是详尽参考：原理、排错、所有踩坑、实现细节。
>
> 对应实现：`src/dfu.{c,h}` + `tools/dfu_upload.py`
> 对应 ROM：STM32G0B1 出厂 System Memory（地址 `0x1FFF0000`）
> 协议：USB Device Firmware Upgrade（DFU 1.1，VID:0483 PID:DF11）

## 0. TL;DR（已配置过的环境）

```powershell
pio run
python tools\dfu_upload.py .pio\build\nucleo_g0b1re\firmware.bin
```

脚本全自动：找串口 → 发 `dfu` → 等 ROM bootloader → `dfu-util` 烧 + leave → 应用回归。
不按 BOOT0、不拔插 USB、不用 STLink。

---

## 1. 第一次配置（每台 PC 配一次即可）

### 1.1 安装 dfu-util

| 系统 | 命令 |
|---|---|
| Windows | `scoop install dfu-util`（首次需先 `irm get.scoop.sh \| iex` 装 scoop） |
| Linux (Debian/Ubuntu) | `sudo apt install dfu-util` |
| macOS | `brew install dfu-util` |

> ⚠ winget 仓库里**没有** dfu-util；Windows 推荐用 scoop。

装完后**新开一个 PowerShell 窗口**让 PATH 生效，验证：

```powershell
dfu-util -V
```

应该看到 `dfu-util 0.11` 或更高。

### 1.2 安装 Python 与依赖

PlatformIO 自带一个内嵌 Python（`~/.platformio/penv/`），但它是 PIO 私有的，升级 PIO 时可能被重建。**推荐装一个独立 Python** 长期使用：

| 系统 | 命令 |
|---|---|
| Windows | `scoop install python` |
| Linux (Debian/Ubuntu) | `sudo apt install python3 python3-pip` |
| macOS | `brew install python` |

> ⚠ Windows 上不要用 Microsoft Store 的 Python "App execution alias"——它有时不能正确把 `python` 加到 PATH，导致脚本运行时报 "Python was not found"。
> 用 scoop 装的版本通过 `~\scoop\shims\python.exe` 注册，PATH 干净可靠。

装完后 **新开一个终端**，验证 + 装 pyserial：

```powershell
python --version          # 应看到 Python 3.x
pip install pyserial      # 装到独立 Python 的 site-packages
```

> 备选（不装独立 Python）：直接用 PIO 内嵌的那个跑 dfu_upload.py：
> ```powershell
> & "$HOME\.platformio\penv\Scripts\python.exe" tools\dfu_upload.py .pio\build\nucleo_g0b1re\firmware.bin
> ```
> PIO 内嵌 Python 已经自带 pyserial，能跑，但命令冗长且 PIO 升级可能丢包。

### 1.3 Windows：用 Zadig 装 WinUSB 驱动（仅一次）

Windows 默认会给 ST DFU 设备装一个 ST 私有驱动，`dfu-util` 用不了。需要用 [Zadig](https://zadig.akeo.ie/) 替换为 WinUSB 驱动。

**只对 DFU 模式的设备装一次**，不影响应用模式 CDC：

1. 让设备进入 DFU 模式：先用 STLink 烧一次本工程固件 → 串口 monitor 发 `dfu\r\n` → 设备会重枚举为 DFU 模式（VID:0483 PID:DF11）
2. 打开 Zadig：`Options → List All Devices`
3. 在下拉列表里找到 **STM32 BOOTLOADER**（注意 USB ID 必须是 `0483 DF11`，**不要选 CDC 的那个**）
4. 右侧目标驱动选 **WinUSB**（不是 libusb-win32！）
5. 点 `Replace Driver`
6. 完成后重新插一次 USB

> **不要用 Zadig 替换 CDC（VID:0483 PID:5740）的驱动**，否则正常的虚拟串口会失效，需要在设备管理器里卸载并让 Windows 重装。

Linux/macOS 不需要这步，但 Linux 下可能需要 udev 规则（见 §6 排错）。

---

## 2. 升级流程（日常用）

### 2.1 应用层在跑的状态下

```bash
python tools/dfu_upload.py .pio/build/nucleo_g0b1re/firmware.bin
```

脚本内部步骤：
1. `serial.tools.list_ports` 找到 VID:0483 PID:5740 的虚拟串口
2. 打开串口、发 `dfu\r\n`
3. 读到 `entering DFU` 响应
4. 设备 100ms 后软复位 → ROM bootloader 启动 → 重新枚举为 VID:0483 PID:DF11
5. 轮询 `dfu-util -l`，最多等 8 秒
6. `dfu-util -a 0 -s 0x08000000:leave -D firmware.bin` 烧 + leave
7. ROM 跳回 `0x08000000` → 应用启动 → CDC 重新枚举

整个过程 USB 线一直插着，不需要任何物理操作。

### 2.2 应用挂死、串口不通时（兜底）

应用如果崩溃到无法响应 `dfu` 命令：

**首选：STLink 直刷**

```bash
pio run -t upload
```

PlatformIO 默认配置 `reset_config srst_only` 即 connect-under-reset，几乎所有"看起来砖了"的情况（HardFault / 死循环 / 中断风暴 / SWD 被复用）都能强行接管。永远是最可靠的兜底——ROM bootloader 在 0x1FFF0000 ROM 区不可写，本工程也从不修改 option byte / RDP，硬件级安全。

如果 PIO 默认连不上，用 **STM32CubeProgrammer GUI** 强解：
1. ST-LINK 模式选 **"Under reset"**（不是 "Normal"）
2. Connect → Erasing & Programming → 选 `firmware.elf` → Start

**关于硬件 BOOT0**

> ⚠ STM32G0 出厂默认 `nBOOT_SEL=1`，BOOT0 **引脚不生效**，启动模式由 option byte 决定。
> 因此即便接出 BOOT0 跳线，短到 VDD 也不会进 ROM bootloader。
> 想用硬件 BOOT0 触发的话，需要先用 STLink 把 option byte `nBOOT_SEL` 改成 0；
> 但既然 STLink 都连上了，直接刷固件更省事，所以本工程不推荐这条路。

---

## 3. 内部实现要点

### 3.1 启动流程

```
上电 / 任何复位
    │
    ▼
main() 第 1 行：DFU_CheckAndJumpEarly()
    │
    ├─ 读 TAMP->BKP0R
    │     │
    │     ├─ != magic → return → 走正常应用启动 (HAL_Init / Clock / USB / ...)
    │     │
    │     └─ == magic → 清 magic、关中断、SYSCFG remap、跳 0x1FFF0000 (永不返回)
```

### 3.2 magic 为什么放 TAMP backup register

| 选项 | 软复位保留 | 上电(POR)保留 | 实现复杂度 |
|---|---|---|---|
| 普通 RAM 变量 | 是* | 否 | 低 |
| `.noinit` 段变量 | 是 | 否 | 中（要改链接脚本） |
| **TAMP_BKP0R** | **是** | **否** | **低** |
| Flash | 是 | 是（不想要） | 高 |

\* 普通 RAM 软复位保留是"未定义行为"，靠不住

TAMP backup 在 V_DD 断电时清零，避免 magic 残留导致一直进 DFU；而我们想要的"软复位时跳 DFU"恰好命中。

### 3.3 跳转代码必须做的事

| 步骤 | 为什么 |
|---|---|
| `__disable_irq()` | 防 PCD/SysTick 中断打扰 ROM |
| `SysTick->CTRL = 0` | 关 SysTick，避免 ROM 启动后 1ms 误触发我们 SysTick_Handler |
| `SYSCFG MEM_MODE=01` | 把 system memory 重映射到 0x00000000，ROM 通过此地址取 SP/PC |
| `__set_MSP(*0x1FFF0000)` | 切换到 ROM 的栈 |
| 函数指针跳 `*0x1FFF0004` | 进入 ROM Reset_Handler |

### 3.4 dfu-util 的 `:leave` 参数

```
-s 0x08000000:leave
```

`:leave` 让 ROM bootloader 烧完之后自动执行 USB DFU detach 流程，并跳到 0x08000000 应用入口。如果不加 `:leave`，烧完之后设备还停在 DFU 模式，必须手动断电或用 `dfu-util -e` 触发 detach。

---

## 4. 常见命令速查

```bash
# 查看当前所有 DFU 设备
dfu-util -l

# 不烧录，只触发 ROM bootloader leave（让设备从 DFU 跳回应用）
dfu-util -e

# 详细日志（-v 多次叠加）
dfu-util -v -v -a 0 -s 0x08000000:leave -D firmware.bin

# 直接传 .bin（不要 .hex 不要 .elf）
# PIO 编译产物路径：.pio/build/<env_name>/firmware.bin
```

---

## 5. 排错 / 已知坑

### 5.1 `Error during download get_status` + dfu-util exit 74（**假错误**）

末尾出现：

```
File downloaded successfully
Submitting leave request...
Error during download get_status
```

→ **烧录已经成功**，可以无视。

原因：ROM bootloader 收到 `:leave` 后**立即**从 USB 总线 detach 并跳应用，dfu-util 还在等最后一次 USB get_status 应答，因此报 IO 错误并以 EX_IOERR (74) 退出。

验证：观察 PA1 是否恢复 1Hz 心跳 + CDC（PID:5740）是否重新出现即可。

`tools/dfu_upload.py` 已对此做容错处理：检测到 exit 74 时会自动等 2 秒确认 DFU 设备已消失，确认后视为成功。所以这条假错误不再阻塞自动化流程。

### 5.2 Windows: `dfu-util: Cannot open DFU device 0483:df11`

→ 没装 WinUSB 驱动。返回 §1.3 用 Zadig 处理。

### 5.2 `STM32 CDC device [0483:5740] not found`

可能原因：
- 设备没插好 / Windows 还没枚举（再等 1 秒重试）
- 旧固件用了不同的 PID（修改过 `usbd_desc.c` 的 USBD_PID 时要同步改 `dfu_upload.py`）
- 设备已经在 DFU 模式（用 `--skip-trigger`）

### 5.3 `timeout: DFU device did not appear`

可能原因：
1. 应用没收到 / 没正确响应 `dfu` 命令 → 串口 monitor 看实际响应
2. ROM bootloader 启动了但 DP 上拉没起作用 → 等 2-3 秒；STM32G0 内置上拉应该没问题
3. Windows 设备管理器里有黄色感叹号 → Zadig 装驱动

### 5.4 `dfu-util: ERROR: bytes_per_hash=0`

老版本 dfu-util bug，升级到 0.11+。`winget` / `apt` 装的一般够新。

### 5.5 烧完之后 LED 不闪 / CDC 不出来

ROM bootloader `:leave` 跳应用时，向量表已经在 `0x08000000`。如果你的应用没正确设置 VTOR，会跳挂。本工程在 `system_stm32g0xx.c` 里默认正确设置 VTOR=0x08000000，无需手动处理。

如果排查不下来，用 STLink 重烧最近一次稳定固件即可恢复。

### 5.6 Linux: `Cannot open DFU device 0483:df11` (Permission denied)

写一个 udev 规则：

```bash
sudo tee /etc/udev/rules.d/99-stm32-dfu.rules <<'EOF'
SUBSYSTEMS=="usb", ATTRS{idVendor}=="0483", ATTRS{idProduct}=="df11", MODE="0666"
EOF

sudo udevadm control --reload-rules
sudo udevadm trigger
```

重新插 USB 即可。

---

## 6. Flash 是否会被破坏？

**不会**。

- ROM bootloader 在 `0x1FFF0000`（System memory，独立的 ROM 区），不可写
- 应用 Flash 在 `0x08000000`，dfu-util 写入；写入失败也只是应用区损坏，下次再写就修复
- 永远可以用 STLink 重烧（兜底）

唯一不能干的：用 dfu-util 写 option byte（除非真的要改 nBOOT_SEL 等），不熟悉的话**别动**。

---

## 7. 后续可能的扩展

| 想法 | 实现思路 |
|---|---|
| `pio run -t upload` 直接走 USB DFU | platformio.ini 加 `upload_protocol = custom` + `upload_command` 调用 `dfu_upload.py` |
| 校验固件版本（防降级 / 防误烧） | 应用 .bin 末尾写 magic + version；dfu_upload.py 烧前先比对 |
| 按按键进 DFU | GPIO 中断 → 调 `DFU_RebootToBootloader()` |
| 双 image A/B 升级 | STM32G0B1 有 dual bank 选项，但 G0B1**CB**（128KB）单 bank，不适用 |
