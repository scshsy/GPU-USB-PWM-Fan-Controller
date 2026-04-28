#!/usr/bin/env python3
"""
GPU 温控风扇守护进程（USB CDC 文本协议）

核心行为（对应 prompt.txt）：
  - 自动发现风扇控制板 CDC 串口（VID:0483 PID:5740），或用 --port 指定
  - 每轮（默认 1Hz）读取 nvidia-smi 输出，动态枚举所有 GPU
  - 决策温度 = max(所有命中 GPU 的温度)（多卡取最高温，不取平均）
  - 由温控曲线 + 3°C 滞回计算目标 duty（最低 30%）
  - 两路风扇始终同步：set fan 1 X + set fan 2 X
  - 每轮 get rpm 打印两路转速
  - 异常不退出：串口异常 / nvidia-smi 异常静默重试；连续失败超过阈值才退出，
    让板载 watchdog 自动切回 fail-safe (80%)
"""

from __future__ import annotations

import argparse
import configparser
import os
import re
import subprocess
import sys
import time
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path
from typing import Iterable, Optional

try:
    import serial
    import serial.tools.list_ports
except ImportError:
    sys.stderr.write(
        "ERROR: missing dependency 'pyserial'\n"
        "       fix: pip install pyserial\n"
    )
    raise


CDC_VID = 0x0483
CDC_PID = 0x5740

NVIDIA_SMI_CMD = [
    "nvidia-smi",
    "--query-gpu=index,name,temperature.gpu",
    "--format=csv,noheader,nounits",
]

def _default_config_path() -> Path:
    # 统一配置：Ubuntu/Linux 上以 /etc 为准（便于 systemd 与手动运行共用同一份）
    if os.name != "nt":
        return Path("/etc/pwm_fan/config")
    # Windows 上没有 /etc，仍按 home 目录约定放一份
    return Path.home() / ".config" / "pwm_fan" / "config"


DEFAULT_CONFIG_PATH = _default_config_path()


def _ts() -> str:
    return datetime.now().strftime("%Y-%m-%d %H:%M:%S")


def log(msg: str) -> None:
    print(f"{_ts()} {msg}", flush=True)


def warn(msg: str) -> None:
    print(f"{_ts()} WARN {msg}", flush=True)


def find_cdc_port() -> Optional[str]:
    for p in serial.tools.list_ports.comports():
        if p.vid == CDC_VID and p.pid == CDC_PID:
            return p.device
    return None


@dataclass(frozen=True)
class GpuTemp:
    index: int
    name: str
    temp_c: int


def _parse_nvidia_smi_csv(text: str) -> list[GpuTemp]:
    """
    解析 nvidia-smi CSV(noheader,nounits) 输出：
      index,name,temperature.gpu
    多卡会有多行，数量不固定。
    """
    out: list[GpuTemp] = []
    for raw in text.splitlines():
        line = raw.strip()
        if not line:
            continue
        parts = [p.strip() for p in line.split(",")]
        if len(parts) < 3:
            continue
        try:
            idx = int(parts[0])
            name = parts[1]
            t = int(parts[2])
        except ValueError:
            continue
        out.append(GpuTemp(index=idx, name=name, temp_c=t))
    return out


def read_gpu_temps(gpu_name_filter: Optional[re.Pattern[str]]) -> list[GpuTemp]:
    r = subprocess.run(
        NVIDIA_SMI_CMD,
        capture_output=True,
        text=True,
        timeout=2.5,
    )
    if r.returncode != 0:
        raise RuntimeError((r.stderr or r.stdout or "").strip() or "nvidia-smi failed")
    temps = _parse_nvidia_smi_csv(r.stdout)
    if gpu_name_filter:
        temps = [g for g in temps if gpu_name_filter.search(g.name)]
    return temps


def lerp(x: float, x0: float, x1: float, y0: float, y1: float) -> float:
    if x <= x0:
        return y0
    if x >= x1:
        return y1
    k = (x - x0) / (x1 - x0)
    return y0 + k * (y1 - y0)


def _parse_points(s: str) -> list[tuple[float, float]]:
    """
    解析 points 字符串：
      "40:30, 55:50, 70:75, 80:100"
    表示 (temp_c, duty_pct) 的分段线性控制点。
    """
    pts: list[tuple[float, float]] = []
    for item in s.split(","):
        item = item.strip()
        if not item:
            continue
        if ":" not in item:
            raise ValueError(f"bad point {item!r}, expected 'temp:duty'")
        a, b = item.split(":", 1)
        t = float(a.strip())
        d = float(b.strip())
        pts.append((t, d))
    pts.sort(key=lambda x: x[0])
    if len(pts) < 2:
        raise ValueError("points must have at least 2 entries")
    return pts


def duty_from_points(temp_c: float, points: list[tuple[float, float]]) -> float:
    """points: 升序 (temp, duty)；超出范围按端点值夹紧。"""
    if temp_c <= points[0][0]:
        return points[0][1]
    if temp_c >= points[-1][0]:
        return points[-1][1]
    for i in range(len(points) - 1):
        t0, d0 = points[i]
        t1, d1 = points[i + 1]
        if t0 <= temp_c <= t1:
            return lerp(temp_c, t0, t1, d0, d1)
    return points[-1][1]


def duty_from_curve(temp_c: float, preset: str) -> int:
    """
    业界常用阶梯 + 线性段：
      <40 -> 30
      40-55 -> 30..50
      55-70 -> 50..75
      70-80 -> 75..100
      >=80 -> 100

    preset 只改变“整体倾向”，不改变边界形状（避免和固件安全策略冲突）。
    """
    # 基础曲线（balanced）
    if temp_c < 40.0:
        base = 30.0
    elif temp_c < 55.0:
        base = lerp(temp_c, 40.0, 55.0, 30.0, 50.0)
    elif temp_c < 70.0:
        base = lerp(temp_c, 55.0, 70.0, 50.0, 75.0)
    elif temp_c < 80.0:
        base = lerp(temp_c, 70.0, 80.0, 75.0, 100.0)
    else:
        base = 100.0

    # 预设微调：quiet 偏低、aggressive 偏高（但都不低于 30、不高于 100）
    if preset == "quiet":
        base -= 8.0
    elif preset == "aggressive":
        base += 8.0

    base = max(30.0, min(100.0, base))
    return int(round(base))


class HysteresisController:
    """
    3°C 滞回：升档立即生效；降档需要温度跌破(阈值 - 3°C)才允许下降。
    """

    def __init__(self, hysteresis_c: float = 3.0):
        self.h = float(hysteresis_c)
        self._last_duty: Optional[int] = None
        self._last_temp: Optional[float] = None

    def apply(self, temp_c: float, proposed_duty: int) -> int:
        if self._last_duty is None:
            self._last_duty = proposed_duty
            self._last_temp = temp_c
            return proposed_duty

        last = self._last_duty

        # 升档：允许立即提高 duty
        if proposed_duty >= last:
            self._last_duty = proposed_duty
            self._last_temp = temp_c
            return proposed_duty

        # 降档：必须确实“更冷”才允许下降
        # 这里用简单规则：只有当 temp <= (上一次温度 - h) 才允许降档。
        # 这比“段边界滞回”更稳，且不需要记曲线区间。
        assert self._last_temp is not None
        if temp_c <= (self._last_temp - self.h):
            self._last_duty = proposed_duty
            self._last_temp = temp_c
            return proposed_duty

        # 否则保持不变
        self._last_temp = temp_c
        return last


def ser_write_line(s: serial.Serial, line: str) -> None:
    s.write(line.encode("ascii") + b"\r\n")
    s.flush()


def ser_readline(s: serial.Serial, timeout_s: float) -> str:
    deadline = time.monotonic() + timeout_s
    buf = bytearray()
    while time.monotonic() < deadline:
        b = s.read(1)
        if not b:
            continue
        if b == b"\n":
            break
        if b != b"\r":
            buf += b
    return buf.decode("ascii", errors="replace").strip()


def main() -> int:
    ap = argparse.ArgumentParser(
        description="Fan control daemon: reads NVIDIA GPU temps and drives STM32 USB PWM fan board.",
    )
    ap.add_argument(
        "--config",
        default=str(DEFAULT_CONFIG_PATH),
        help=f"Config file path (default: {DEFAULT_CONFIG_PATH})",
    )
    ap.add_argument("--port", help="Override serial port (e.g. COM5, /dev/ttyACM0)")
    ap.add_argument("--interval", type=float, default=1.0, help="Loop interval seconds (default: 1.0)")
    ap.add_argument("--gpu-filter", default="", help="Regex filter for GPU name (default: include all)")
    ap.add_argument("--dry-run", action="store_true", help="Print decisions only; do not send commands")
    ap.add_argument("--curve", default="preset:balanced",
                    help="Curve preset: preset:quiet|balanced|aggressive (default: preset:balanced)")
    ap.add_argument("--max-fail", type=int, default=10,
                    help="Consecutive failures before exit (default: 10)")
    ap.add_argument("--temp-fail-duty", type=int, default=75,
                    help="Fallback duty when temp read fails 3 times (default: 75)")
    args = ap.parse_args()

    # ---- 配置文件（命令行优先；配置文件仅提供默认值） -------------------------
    cfg = configparser.ConfigParser()
    cfg_path = Path(args.config).expanduser()
    if cfg_path.is_file():
        try:
            cfg.read(cfg_path, encoding="utf-8")
            log(f"loaded config: {cfg_path}")
        except Exception as e:
            warn(f"failed to read config {cfg_path}: {e} (ignored)")

    def cfg_get(section: str, key: str) -> Optional[str]:
        if cfg.has_section(section) and cfg.has_option(section, key):
            return cfg.get(section, key)
        return None

    # 从配置文件补默认值（仅当命令行还是默认值时才覆盖）
    if args.interval == 1.0:
        v = cfg_get("general", "interval")
        if v:
            args.interval = float(v)
    if not args.gpu_filter:
        v = cfg_get("general", "gpu_filter")
        if v:
            args.gpu_filter = v
    if args.curve == "preset:balanced":
        v = cfg_get("general", "curve")
        if v:
            args.curve = v
    if args.max_fail == 10:
        v = cfg_get("general", "max_fail")
        if v:
            args.max_fail = int(v)
    if args.temp_fail_duty == 75:
        v = cfg_get("general", "temp_fail_duty")
        if v:
            args.temp_fail_duty = int(v)

    min_duty = 30
    v = cfg_get("general", "min_duty")
    if v:
        min_duty = int(v)

    hysteresis_c = 3.0
    v = cfg_get("general", "hysteresis_c")
    if v:
        hysteresis_c = float(v)

    # curve.points：自定义曲线（优先级高于 preset）
    points: Optional[list[tuple[float, float]]] = None
    v = cfg_get("curve", "points")
    if v:
        try:
            points = _parse_points(v)
        except Exception as e:
            warn(f"bad [curve] points in {cfg_path}: {e} (ignored)")
            points = None

    if args.curve.startswith("preset:"):
        preset = args.curve.split(":", 1)[1].strip()
    else:
        preset = args.curve.strip()
    if preset not in ("quiet", "balanced", "aggressive"):
        if points is None:
            raise SystemExit("bad --curve; expected preset:quiet|balanced|aggressive (or use [curve] points)")

    gpu_re = re.compile(args.gpu_filter) if args.gpu_filter else None

    port = args.port or find_cdc_port()
    if not port:
        warn(f"CDC device [{CDC_VID:04x}:{CDC_PID:04x}] not found; retrying...")
        # 持续等到出现，别让用户手动重启脚本
        while not port:
            time.sleep(1.0)
            port = args.port or find_cdc_port()
        log(f"found port={port}")
    else:
        log(f"using port={port}")

    ctrl = HysteresisController(hysteresis_c=hysteresis_c)

    # GPU 温度读取失败计数（用于 3 次失败兜底 75%）
    temp_fail_streak = 0
    # 总失败计数（用于最终退出）
    hard_fail_streak = 0

    # 串口异常时自动重连
    ser: Optional[serial.Serial] = None

    def ensure_serial() -> serial.Serial:
        nonlocal ser
        if ser and ser.is_open:
            return ser
        ser = serial.Serial(port, 115200, timeout=0.2)
        try:
            ser.reset_input_buffer()
        except Exception:
            pass
        return ser

    while True:
        t0 = time.monotonic()
        try:
            temps = read_gpu_temps(gpu_re)
            if not temps:
                raise RuntimeError("no GPU temps (filter excluded all?)")

            max_temp = max(g.temp_c for g in temps)
            if points is not None:
                proposed = int(round(duty_from_points(float(max_temp), points)))
            else:
                proposed = duty_from_curve(float(max_temp), preset=preset)

            # 最低保活档
            proposed = max(min_duty, min(100, proposed))
            duty = ctrl.apply(float(max_temp), proposed)

            temp_fail_streak = 0

            if not args.dry_run:
                s = ensure_serial()
                ser_write_line(s, f"set fan 1 {duty}")
                _ = ser_readline(s, 0.6)  # ok / err...
                ser_write_line(s, f"set fan 2 {duty}")
                _ = ser_readline(s, 0.6)

                ser_write_line(s, "get rpm")
                rpm_line = ser_readline(s, 0.8)

                ser_write_line(s, "get status")
                status_line = ser_readline(s, 0.8)
            else:
                rpm_line = "rpm 1=? 2=?"
                status_line = "duty=?,? rpm=?,? usb=? src=? wd=? fault=?"

            g_list = " ".join([f"{g.index}:{g.temp_c}C" for g in temps])
            log(f"gpus=[{g_list}] max={max_temp}C duty={duty}% {rpm_line}")
            # 为 systemd/journald 解析提供稳定前缀
            log(f"{rpm_line}")
            log(f"board {status_line}")
            hard_fail_streak = 0

        except Exception as e:
            hard_fail_streak += 1
            temp_fail_streak += 1
            warn(f"loop error ({hard_fail_streak}/{args.max_fail}): {e}")

            # 串口异常：关闭让下次重连
            try:
                if ser:
                    ser.close()
            except Exception:
                pass
            ser = None

            # 温度连续 3 次失败 -> 先用 75%（比板载 80% 稍保守，但仍可接受）
            if temp_fail_streak >= 3:
                duty = max(30, min(100, int(args.temp_fail_duty)))
                if not args.dry_run:
                    try:
                        s = ensure_serial()
                        ser_write_line(s, f"set fan 1 {duty}")
                        _ = ser_readline(s, 0.6)
                        ser_write_line(s, f"set fan 2 {duty}")
                        _ = ser_readline(s, 0.6)
                        log(f"temp unavailable -> fallback duty={duty}%")
                    except Exception as e2:
                        warn(f"fallback write failed: {e2}")

            if hard_fail_streak >= args.max_fail:
                warn("too many failures; exiting so board-side watchdog can take over")
                return 1

        # 保持固定周期
        dt = time.monotonic() - t0
        sleep_s = max(0.0, float(args.interval) - dt)
        time.sleep(sleep_s)


if __name__ == "__main__":
    raise SystemExit(main())

