# USB DFU 升级 - 快速指南

> 一页纸版本。详细原理 / 完整排错 / 实现细节请看 [USB_DFU_GUIDE.md](USB_DFU_GUIDE.md)

## 日常一键升级

```powershell
pio run
python tools\dfu_upload.py .pio\build\nucleo_g0b1re\firmware.bin
```

脚本自动完成：找串口 → 发 `dfu` 命令 → 等设备重枚举为 DFU → `dfu-util` 烧录 → 跳回应用。

**全程零手动**：不按按钮、不拔插 USB、不用 STLink。

---

## 首次配置（每台 PC 一次）

### Windows

```powershell
# 1) 装 scoop（已有可跳过）
Set-ExecutionPolicy -ExecutionPolicy RemoteSigned -Scope CurrentUser
irm get.scoop.sh | iex

# 2) 装命令行工具
scoop install dfu-util python
pip install pyserial

# 3) STLink 烧一次本工程的固件（最后一次用 STLink）
pio run -t upload
```

之后做一次 Zadig 驱动绑定（只一次）：

1. 打开串口（任意工具，如 `pio device monitor`），输入 `dfu` 回车
2. 设备会消失重枚举为 `STM32 BOOTLOADER`（PID:DF11）
3. 下载 [Zadig](https://zadig.akeo.ie/)，`Options → List All Devices`
4. 选 **STM32 BOOTLOADER**（USB ID 必须是 `0483 DF11`）→ 目标驱动选 **WinUSB** → `Replace Driver`

> ⚠ 千万**不要**选成 `USB 串行设备 / Virtual COM Port`（PID:5740），那个是 CDC，已经有正确的内置驱动。

完成后日常只需第一段那两行命令。

### Linux

```bash
sudo apt install dfu-util python3-pip
pip install pyserial

# udev 规则（一次性）
sudo tee /etc/udev/rules.d/99-stm32-dfu.rules <<'EOF'
SUBSYSTEMS=="usb", ATTRS{idVendor}=="0483", ATTRS{idProduct}=="df11", MODE="0666"
EOF
sudo udevadm control --reload-rules && sudo udevadm trigger
```

### macOS

```bash
brew install dfu-util python
pip install pyserial
```

---

## 万一砖了

```powershell
pio run -t upload
```

STLink 直刷永远是兜底。ROM bootloader 在 ROM 区不可写，应用层从不动 option byte，物理上不可能变真砖。

---

## 三句话原理速记

1. 应用收到 `dfu\r\n` → 写 magic 到 `TAMP->BKP0R` → `NVIC_SystemReset()`
2. 复位后 `main()` 第一行检测 magic → 跳 `0x1FFF0000`（ROM USB DFU bootloader）
3. PC 用 `dfu-util ... :leave` 烧 + 让 ROM 跳回 `0x08000000` 应用

代码：[`src/dfu.c`](../src/dfu.c)（约 130 行）
脚本：[`tools/dfu_upload.py`](../tools/dfu_upload.py)
