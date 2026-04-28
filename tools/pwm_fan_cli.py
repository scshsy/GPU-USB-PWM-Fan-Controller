#!/usr/bin/env python3
"""
PWM Fan 控制板命令行工具（Ubuntu/Windows 都可用）

用途：
  - 查询转速：get rpm
  - 查询综合状态：get status
  - 下发占空比：set fan <ch> <pct>
  - 喂狗：kick

默认行为：
  - 自动发现串口（VID:0483 PID:5740）
  - 一次执行一条命令，打印设备返回的一行
"""

from __future__ import annotations

import argparse
import sys
import time
from typing import Optional

try:
    import serial
    import serial.tools.list_ports
except ImportError:
    sys.stderr.write(
        "ERROR: missing dependency 'pyserial'\n"
        "       Ubuntu: sudo apt install python3-serial\n"
        "       or:     pip install pyserial\n"
    )
    raise


CDC_VID = 0x0483
CDC_PID = 0x5740


def find_cdc_port() -> Optional[str]:
    for p in serial.tools.list_ports.comports():
        if p.vid == CDC_VID and p.pid == CDC_PID:
            return p.device
    return None


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
    ap = argparse.ArgumentParser(description="CLI for STM32 USB PWM fan controller (CDC text protocol).")
    ap.add_argument("--port", help="Override serial port (e.g. /dev/ttyACM0)")
    ap.add_argument("--timeout", type=float, default=1.0, help="Read timeout seconds (default: 1.0)")

    sub = ap.add_subparsers(dest="cmd", required=True)

    sub.add_parser("rpm", help="Read current RPMs (get rpm)")
    sub.add_parser("status", help="Read composite status (get status)")
    sub.add_parser("kick", help="Kick host watchdog (kick)")

    p_set = sub.add_parser("set", help="Set duty percent for a fan channel (set fan <ch> <pct>)")
    p_set.add_argument("ch", type=int, choices=[1, 2])
    p_set.add_argument("pct", type=int)

    args = ap.parse_args()

    port = args.port or find_cdc_port()
    if not port:
        sys.stderr.write(f"ERR: CDC device [{CDC_VID:04x}:{CDC_PID:04x}] not found\n")
        return 1

    if args.cmd == "rpm":
        line = "get rpm"
    elif args.cmd == "status":
        line = "get status"
    elif args.cmd == "kick":
        line = "kick"
    elif args.cmd == "set":
        pct = max(0, min(100, int(args.pct)))
        line = f"set fan {args.ch} {pct}"
    else:
        sys.stderr.write("ERR: unknown command\n")
        return 2

    with serial.Serial(port, 115200, timeout=0.2) as s:
        try:
            s.reset_input_buffer()
        except Exception:
            pass
        ser_write_line(s, line)
        resp = ser_readline(s, float(args.timeout))
        print(resp)
        return 0 if resp else 1


if __name__ == "__main__":
    raise SystemExit(main())

