# GPU USB PWM Fan Controller（GPU 温控 USB PWM 风扇控制器）

> **简易版（上手用这一页）**。详细版请看 `docs/PROJECT_GUIDE.md`。

## 项目做什么

- STM32G0B1 固件控制 **2 路 4-pin 风扇**：25 kHz PWM 调速 + TACH 测速
- USB CDC（虚拟串口）文本协议：主机下发占空比、回读 RPM、查询状态
- **Fail-safe 安全策略**：USB 未枚举 / 主机失联 / 脚本崩溃 → 自动回安全档
- PC 端脚本：读取 `nvidia-smi` 的 **多卡温度**，取最高温 → 计算 duty → 下发 → 回读 RPM

## 快速开始（Windows / 开发机）

### 1) 编译

```bash
pio run -e nucleo_g0b1re
```

产物：`.pio/build/nucleo_g0b1re/firmware.bin`

### 2) 一键 DFU 刷写（板子在跑应用时）

```bash
python tools/dfu_upload.py .pio/build/nucleo_g0b1re/firmware.bin
```

> dfu-util 的 exit code 74 已在脚本中兼容（STM32 ROM `:leave` 已知“假错误”）。

### 3) 串口验证（协议）

```bash
pio device monitor -b 115200
```

在 monitor 里输入：

- `get status`：看当前 duty/rpm/usb/src/wd/fault
- `get rpm`：看两路 RPM
- `set fan 1 30` + `set fan 2 30`：下发 duty
- `kick`：喂主机 watchdog（不改变 duty）
- `dfu`：进入 ROM DFU（供 `dfu_upload.py` 使用）

## 自动温控（GPU 温度→风速）

### 1) 运行（真实下发）

```bash
python tools/fan_daemon.py --port COM4
```

### 2) 配置文件（温度→duty 曲线）

默认读取：`/etc/pwm_fan/config`（Ubuntu/Linux；手动运行与 systemd 共用同一份）

配置样例见：`docs/PROJECT_GUIDE.md`（推荐用 `[curve] points = 40:30, 55:50, ...` 这种分段线性点）

### 3) 只演算不下发（dry-run）

```bash
python tools/fan_daemon.py --dry-run
```

## Ubuntu Server 一键部署（无需源码）

```bash
curl -fsSL https://raw.githubusercontent.com/scshsy/GPU-USB-PWM-Fan-Controller/main/tools/deploy.sh | sudo bash
```

脚本会自动：安装系统依赖 → 从 GitHub 拉取代码 → 安装程序和配置文件 → 启动 systemd 服务。

已有源码时也可手动执行：
```bash
sudo bash tools/deploy.sh
```

常用命令：
```bash
sudo systemctl status pwm-fan.service
sudo journalctl -u pwm-fan -f
pwm-fan-rpm
pwm-fan-status
pwm-fan-help
```

系统命令：
- `pwm-fan-rpm`
- `pwm-fan-status`
- `pwm-fan-help`

## 硬件源文件（开源入口）

- 嘉立创EDA 专业版工程（**原理图 + PCB**）：`hardware/GPU USB PWM Fan Controller.epro`

## 关键文档入口

- USB CDC（详细踩坑/架构）：`docs/USB_CDC_GUIDE.md`
- USB DFU（一页纸）：`docs/USB_DFU.md`
- USB DFU（详细版）：`docs/USB_DFU_GUIDE.md`
- 风扇控制（协议/安全策略/接线）：`docs/FAN_CONTROL.md`
- 项目总文档（详细版）：`docs/PROJECT_GUIDE.md`

