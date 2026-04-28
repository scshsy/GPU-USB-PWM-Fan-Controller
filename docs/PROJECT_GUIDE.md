# 项目文档（详细版）- GPU USB PWM Fan Controller（GPU 温控 USB PWM 风扇控制器）

> 本文是“详细版”。一页纸简易版见仓库根目录 `README.md`。

## 1. 总体架构

### 1.1 数据流

```
Host (Python)
  ├─ nvidia-smi 读 GPU 温度（多卡取 max）
  ├─ 温控曲线 + 滞回 => duty(%)
  └─ CDC 文本协议下发：
       set fan 1 <duty>
       set fan 2 <duty>
       get rpm
         ▲
         │
STM32G0B1 (Firmware)
  ├─ USB CDC RX -> CLI 行解析 -> fanctl 命令处理
  ├─ fanctl 安全仲裁 -> pwm 写 TIM1 CCR
  └─ tach 从 TIM3 输入捕获计算 RPM -> get rpm/status 回传
```

### 1.2 固件状态机（安全策略）

- **DEFAULT**：上电/复位默认 duty（`FAN_DEFAULT_DUTY_PCT`）
- **HOST**：USB 已枚举 + 主机 watchdog 未超时 → 使用 host 下发 duty
- **SAFE**：USB 未枚举 或 主机 watchdog 超时 → 强制 `FAN_SAFE_DUTY_PCT`
- **OVERRIDE**：调试覆盖（优先级最高）

详见：`docs/FAN_CONTROL.md`。

---

## 2. 目录与模块

### 2.1 固件（`src/`）

- `src/pwm.{c,h}`：TIM1 CH1/CH2 25kHz PWM（PA8/PA9）
- `src/tach.{c,h}`：TIM3 输入捕获 TACH（PA6/PA7，4 点平均，>1s 无脉冲=0）
- `src/fanctl.{c,h}`：仲裁、安全策略、命令处理（set/get/kick/status）
- `src/cli.{c,h}`：CDC 上的行协议框架（handler 注册、echo 兜底）
- `src/dfu.{c,h}`：`dfu` 命令 + magic + 跳 ROM bootloader
- `src/usb/*`：USB CDC 设备栈 glue

### 2.2 工具（`tools/`）

- `tools/dfu_upload.py`：一键 DFU（CDC 触发 → ROM DFU → dfu-util 烧录）
- `tools/fan_daemon.py`：GPU 温控守护（读温度 → 算 duty → 下发 → 回读 RPM）
- `tools/pwm_fan_cli.py`：一条命令做一次查询/设置（rpm/status/set/kick）
- `tools/install.sh`：Ubuntu Server 安装 systemd 服务 + 系统命令（幂等）

---

## 3. 固件协议（CDC 文本）

行结束：`\r\n` 或 `\n`，命令大小写不敏感。

常用命令：
- `set fan <1|2> <0..100>`
- `kick`
- `get rpm`
- `get status`
- `dfu`
- `?`（CRS 诊断）

协议与安全策略详见：`docs/FAN_CONTROL.md`。

---

## 4. 编译 / 刷写 / 串口验证

### 4.1 编译

```bash
pio run -e nucleo_g0b1re
```

### 4.2 一键 DFU 刷写（推荐）

```bash
python tools/dfu_upload.py .pio/build/nucleo_g0b1re/firmware.bin
```

### 4.3 串口 monitor 验证

```bash
pio device monitor -b 115200
```

建议验证顺序：
1) `get status`
2) `get rpm`
3) `set fan 1 30` + `set fan 2 30`
4) `get status` 确认 `src=host duty=30,30`
5) 停止发送任何命令 > 3s，再 `get status`，应回 `src=safe duty=80,80`

---

## 5. PC 温控脚本（`tools/fan_daemon.py`）

### 5.1 核心规则（必须记住）

- **多卡取最高温**：决策温度 = `max(所有命中 GPU 的温度)`
- **双通道同步**：每轮必发 `set fan 1 X` 与 `set fan 2 X`
- **主机 watchdog**：每轮 `set fan` 本身就会“喂狗”（保持 `src=host`）
- **异常兜底**：脚本异常退出 → 板子 3s 后自动回 SAFE（80%）

### 5.2 dry-run 是什么

`--dry-run` 表示“只演算不下发”：会读温度、算 duty、打日志，但不会打开串口，也不会读取 RPM。

### 5.3 配置文件（推荐）

默认配置路径：`/etc/pwm_fan/config`（Ubuntu/Linux；手动运行与 systemd 共用同一份）

也可用 `--config` 指定其他路径。

示例（带注释）：

```ini
[general]
interval = 1.0
gpu_filter =
curve = preset:balanced
hysteresis_c = 3.0
min_duty = 30
max_fail = 10
temp_fail_duty = 75

[curve]
; points = temp:duty, temp:duty, ...
points = 40:30, 55:50, 70:75, 80:100
```

说明：
- 若提供了 `[curve] points`，则 **points 优先于 preset**
- points 是分段线性插值：温度在两点之间按比例插值；超出范围按端点夹紧

---

## 6. Ubuntu Server 部署（systemd 常驻）

### 6.1 安装（幂等，可重复执行）

```bash
sudo bash tools/install.sh
```

安装后：
- 服务：`pwm-fan.service`
- 配置：`/etc/pwm_fan/config`
- 日志（推荐）：`journalctl -u pwm-fan -f`
- 系统命令：
  - `pwm-fan-rpm`
  - `pwm-fan-status`
  - `pwm-fan-help`

### 6.2 修改配置后生效

```bash
sudo systemctl restart pwm-fan.service
```

---

## 7. WSL2 测试（USB 透传）

WSL2 默认看不到 Windows COM，需要 `usbipd-win` attach。

Windows（管理员 PowerShell）：

```powershell
usbipd list
usbipd bind --busid <BUSID>
usbipd attach --wsl --busid <BUSID>
```

WSL2：

```bash
ls -l /dev/ttyACM* /dev/ttyUSB* 2>/dev/null
python3 tools/pwm_fan_cli.py --port /dev/ttyACM0 status
```

---

## 8. 常见问题排错（最短路径）

- **刷完 DFU 后 CDC 不回来**
  - 先等 2~3 秒（Windows 枚举有时慢）
  - 仍不行 → `pio run -t upload` 走 STLink 救砖
- **`get rpm` 始终 0**
  - 确认 TACH 线接对、外部 12V 与板子共地
  - 确认风扇是 2 PPR（若不同需改固件常量）
- **脚本跑着跑着进入 SAFE**
  - 看 `get status` 的 `wd=` 是否归零、`fault` bit1 是否置位
  - 检查主机脚本是否卡死/串口是否被别的程序占用

