# 风扇控制（GPU USB PWM Fan Controller）

## 1. 硬件接线与约束

### 1.1 4-pin 风扇线序（常见配色）

> 不同厂商可能配色不同，但行业常见是：

- 黑：GND
- 黄：+12V
- 绿：PWM（控制输入，25 kHz）
- 蓝：TACH（转速输出，开漏/开集电极）

### 1.2 本控制板 IO（已定板）

- **硬件源文件（嘉立创EDA 专业版，原理图 + PCB）**
  - `hardware/GPU USB PWM Fan Controller.epro`

- **PWM 输出（推挽，25 kHz）**
  - PA8 → TIM1_CH1 → FAN1_PWM
  - PA9 → TIM1_CH2 → FAN2_PWM
- **TACH 输入（外部 4.7K 上拉到 3.3V + RC 滤波 → MCU 浮空输入）**
  - PA6 → TIM3_CH1 → FAN1_TACH
  - PA7 → TIM3_CH2 → FAN2_TACH
- **风扇脉冲规格**
  - 每转 2 个脉冲（2 PPR）

### 1.3 供电说明（重要）

- **USB 与风扇 12V 供电无关**：风扇的 +12V 由外部 12V 电源提供。
- **必须共地**：外部 12V 电源的 GND 与本板 GND 必须相连（共地），否则 PWM/TACH 参考电平漂移会导致失控或测速异常。

---

## 2. CDC 文本协议（行协议）

### 2.1 基本规则

- 传输通道：USB CDC 虚拟串口（VID `0483` PID `5740`）
- 行结束：`\r\n` 或 `\n`
- 命令大小写不敏感
- 应答一行一条，末尾带 `\r\n`

### 2.2 命令表

| 命令 | 应答 | 说明 |
|---|---|---|
| `set fan <ch> <pct>` | `ok` / `err <reason>` | `ch` ∈ {1,2}，`pct` ∈ [0,100]；合法命令会刷新 host watchdog |
| `kick` | `ok` | 仅喂狗，不改变 duty |
| `get rpm` | `rpm 1=<n> 2=<m>` | 返回两路 RPM（停转/超时视为 0） |
| `get status` | `duty=<a>,<b> rpm=<n>,<m> usb=<0/1> src=<host|safe|default|override> wd=<ms_left> fault=0x<bits>` | 综合状态 |
| `?` |（CRS 诊断行）| 见 `docs/USB_CDC_GUIDE.md` |
| `dfu` | `entering DFU` 后复位 | 进入 ROM DFU（见 `docs/USB_DFU.md`） |

> `fault` bit（当前实现）：
> - bit0: USB 未枚举（非 CONFIGURED）
> - bit1: host watchdog 超时

---

## 3. 固件安全策略（Fail-safe）

### 3.1 两个常量（已分离）

- **上电默认档**：`FAN_DEFAULT_DUTY_PCT = 80%`
- **安全档**：`FAN_SAFE_DUTY_PCT = 80%`

未来可以独立调这两个值（例如默认 80%，失联档 100%）。

### 3.2 触发条件

1. **上电/复位**：先进入默认档（80%）
2. **USB 未枚举**：强制安全档（80%）
3. **主机失联 watchdog**：任意合法 `set fan` / `kick` 会刷新时间戳；超过 3000ms 未刷新 → 强制安全档（80%）

### 3.3 状态机（ASCII）

```
                +------------------+
   reset/power  |     DEFAULT      |
   ------------>+ duty=DEFAULT(80) |
                +------------------+
                        |
                        | (USB configured) + first host msg
                        v
                +------------------+
                |       HOST       |
                | duty=host target |
                +------------------+
                   |          |
     wd timeout     |          | USB not configured
  (>3000ms no kick) |          |
                   v          v
                +------------------+
                |       SAFE       |
                | duty=SAFE(80)    |
                +------------------+
                        ^
                        |
                        | (USB configured) + host msg resumes
                        |

  OVERRIDE：调试/应急覆盖（优先级最高），覆盖上面所有状态
```

---

## 4. PC 端温控脚本（`tools/fan_daemon.py`）

### 4.1 设计要点

- **多卡规则**：枚举所有 GPU，取温度最大值作为决策温度
- **双通道同步**：每轮都发 `set fan 1 X` + `set fan 2 X`
- **滞回**：3°C，避免频繁上下跳
- **最低保活档**：任何时候 duty < 30% 会被抬到 30%
- **失败兜底**：连续 3 次读不到温度时先改为 75%；再持续失败会退出，让板载 watchdog 接管安全档

### 4.2 用法

```bash
python tools/fan_daemon.py
```

常用参数：

```bash
python tools/fan_daemon.py --port COM5 --interval 1.0
python tools/fan_daemon.py --gpu-filter "RTX|A100" --curve preset:aggressive
python tools/fan_daemon.py --dry-run
```

---

## 5. 多卡日志样例（1/2/4 卡）

### 5.1 1 卡

```
2026-04-28 22:00:00 gpus=[0:52C] max=52C duty=44% rpm 1=12345 2=12340
```

### 5.2 2 卡（取最高温）

```
2026-04-28 22:00:01 gpus=[0:48C 1:76C] max=76C duty=90% rpm 1=17800 2=17720
```

### 5.3 4 卡（任意一张卡热就拉高风量）

```
2026-04-28 22:00:02 gpus=[0:41C 1:43C 2:44C 3:79C] max=79C duty=98% rpm 1=19500 2=19460
```

---

## 6. 调参方法与排错

### 6.1 先用 `get status` 看当前处于哪一档

示例：

```
duty=80,80 rpm=0,0 usb=0 src=safe wd=0 fault=0x1
```

判读：
- `usb=0`：未枚举（或已断开）
- `src=safe`：安全档生效
- `fault=0x1`：bit0=USB 未枚举

### 6.2 常见问题

- **插着 USB 但 `usb=0`**
  - 确认 PC 设备管理器已出现 CDC COM 口（VID:PID 0483:5740）
  - 确认数据线不是“仅充电线”
- **`get rpm` 始终 0**
  - 确认风扇 TACH 线接对
  - 确认外部 12V 与本板共地
  - 确认该风扇的 TACH 规格为 2 PPR（若不同需要改固件常量）

