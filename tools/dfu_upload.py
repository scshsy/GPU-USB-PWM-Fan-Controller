#!/usr/bin/env python3
"""
USB CDC 一键 DFU 升级脚本

工作流：
  1. 自动找到运行中的 STM32 CDC 虚拟串口（VID:0483 PID:5740）
  2. 通过该串口发 "dfu\r\n" 命令
  3. 应用回 "entering DFU"、写 magic、软复位、跳 ROM bootloader
  4. 等待 PC 端枚举到 ST DFU 设备（VID:0483 PID:DF11）
  5. 调用 dfu-util 烧 .bin 到 0x08000000，并通过 :leave 让 ROM 跳回应用
  6. 应用启动，CDC 重新枚举可用

依赖：
    Python:   pip install pyserial
    系统:     dfu-util  (Windows: scoop install dfu-util ; Linux: apt install dfu-util)
              Windows 第一次还需要用 Zadig 给 STM32 BOOTLOADER 装 WinUSB 驱动
              如果 dfu-util 在 PATH 里找不到, 用 --dfu-util <path> 或 $DFU_UTIL 指定

典型用法：
    python tools/dfu_upload.py .pio/build/nucleo_g0b1re/firmware.bin

集成进 PlatformIO（见 platformio.ini 的 custom upload command）：
    pio run -t upload
"""
from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

try:
    import serial
    import serial.tools.list_ports
except ImportError:
    sys.stderr.write(
        "ERROR: missing dependency 'pyserial'\n"
        "       fix: pip install pyserial\n"
    )
    sys.exit(2)


# ---- 设备身份 -----------------------------------------------------------------

CDC_VID = 0x0483
CDC_PID = 0x5740   # 应用模式（usbd_desc.c 里写死的）

DFU_VID = 0x0483
DFU_PID = 0xDF11   # ST ROM bootloader DFU 模式

# ---- 时序参数 -----------------------------------------------------------------

WAIT_DFU_TIMEOUT_S = 8.0    # 等设备从 CDC 重枚举为 DFU 的最大时长
DFU_REPLY_READ_S   = 1.0    # 等 "entering DFU" 响应的时间
POLL_INTERVAL_S    = 0.3    # 轮询 dfu-util -l 的间隔

# dfu-util 可执行路径 - 由 main() 在启动时解析（PATH / 环境变量 / --dfu-util 参数）
DFU_UTIL: str = "dfu-util"


def _resolve_dfu_util(explicit: str | None) -> str | None:
    """按优先级查找 dfu-util：--dfu-util > $DFU_UTIL > PATH > 常见 scoop 路径"""
    candidates = []
    if explicit:
        candidates.append(explicit)
    env = os.environ.get("DFU_UTIL")
    if env:
        candidates.append(env)
    candidates.append("dfu-util")
    if sys.platform.startswith("win"):
        # 对 PATH 没刷新到当前 shell 的情况兜底（scoop 默认安装路径）
        userprofile = os.environ.get("USERPROFILE", "")
        if userprofile:
            candidates.append(
                os.path.join(userprofile, "scoop", "shims", "dfu-util.exe")
            )
            candidates.append(
                os.path.join(userprofile, "scoop", "apps", "dfu-util",
                             "current", "dfu-util.exe")
            )

    for c in candidates:
        # shutil.which 既会找 PATH，也会判断绝对路径文件是否可执行
        found = shutil.which(c) or (c if os.path.isfile(c) else None)
        if found:
            return found
    return None


# ---- 终端着色（可选，无 colorama 也能跑） -------------------------------------

def _c(code: str, text: str) -> str:
    if os.environ.get("NO_COLOR") or not sys.stdout.isatty():
        return text
    return f"\x1b[{code}m{text}\x1b[0m"


def info(msg: str) -> None:
    print(_c("36", "[i] ") + msg, flush=True)


def ok(msg: str) -> None:
    print(_c("32", "[+] ") + msg, flush=True)


def warn(msg: str) -> None:
    print(_c("33", "[!] ") + msg, flush=True)


def err(msg: str) -> None:
    print(_c("31", "[x] ") + msg, file=sys.stderr, flush=True)


# ---- 步骤实现 -----------------------------------------------------------------

def find_cdc_port() -> str | None:
    """扫描所有 COM 口，返回第一个 VID:PID 匹配的设备名"""
    for p in serial.tools.list_ports.comports():
        if p.vid == CDC_VID and p.pid == CDC_PID:
            return p.device
    return None


def trigger_dfu(port_name: str) -> bool:
    """打开 CDC 串口，发 'dfu\\r\\n'，读回响应；返回是否收到 'entering DFU'"""
    info(f"opening {port_name} (CDC application mode)")
    try:
        # Baudrate 对 USB CDC 是虚拟值，写多少都行；timeout 用于 readline
        s = serial.Serial(port_name, 115200, timeout=DFU_REPLY_READ_S)
    except serial.SerialException as e:
        err(f"failed to open {port_name}: {e}")
        return False

    try:
        # 清掉残留，避免上次 echo 干扰判断
        try:
            s.reset_input_buffer()
        except (OSError, serial.SerialException):
            pass

        info('sending "dfu\\r\\n"')
        s.write(b"dfu\r\n")
        s.flush()

        # 期望响应是固定字节串 "entering DFU\r\n"
        line = s.readline().decode("ascii", errors="replace").strip()
        if "entering DFU" in line:
            ok(f"device replied: {line!r}")
            return True
        warn(f"unexpected reply: {line!r} (will still wait for DFU enumeration)")
        return True   # 即便响应异常也继续，可能是用户上版固件没接 dfu 命令
    finally:
        try:
            s.close()
        except OSError:
            pass


def find_dfu_with_pyusb_or_listcmd() -> bool:
    """轻量探测：调 dfu-util -l 看输出里是否有 [0483:df11]

    main() 启动时已经验证过 dfu-util 存在；这里只做一次性子进程调用，
    不再处理 FileNotFoundError（让其向上抛出，避免循环里反复打印）。"""
    try:
        r = subprocess.run(
            [DFU_UTIL, "-l"],
            capture_output=True,
            text=True,
            timeout=3,
        )
    except subprocess.TimeoutExpired:
        return False

    needle = f"[{DFU_VID:04x}:{DFU_PID:04x}]"
    return needle in r.stdout.lower()


def wait_for_dfu(timeout_s: float = WAIT_DFU_TIMEOUT_S) -> bool:
    info(f"waiting for DFU device [{DFU_VID:04x}:{DFU_PID:04x}] (max {timeout_s:.0f}s)")
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        if find_dfu_with_pyusb_or_listcmd():
            ok("DFU device enumerated")
            return True
        time.sleep(POLL_INTERVAL_S)
    err("timeout: DFU device did not appear")
    err("  - 第一次跑必须用 Zadig 给 'STM32 BOOTLOADER' 装 WinUSB 驱动 (Windows)")
    err("  - 检查应用是否真的进了 ROM：observe LED + run dfu-util -l 手动确认")
    return False


def flash_firmware(bin_path: Path, alt: int = 0, addr: int = 0x08000000) -> bool:
    cmd = [
        DFU_UTIL,
        "-a", str(alt),
        "-s", f"0x{addr:08X}:leave",
        "-D", str(bin_path),
    ]
    info("exec: " + " ".join(cmd))
    r = subprocess.run(cmd)

    if r.returncode == 0:
        ok("flash completed; ROM bootloader has issued :leave -> app should be running")
        return True

    # Known false positive on STM32 ROM bootloader:
    #   烧录已经 100% 完成 + leave 指令已提交后，ROM 立即从 USB 总线消失去跳应用，
    #   dfu-util 还在等最后一次 get_status 应答 → 报 "Error during download get_status"
    #   并以 EX_IOERR (74) 退出。
    # 验证方式：等几秒后查看 DFU 设备是否已消失（消失 = 真烧录成功 + 已跳应用）。
    if r.returncode == 74:
        info("dfu-util exit code 74 detected; verifying via device state...")
        time.sleep(2.0)
        if not find_dfu_with_pyusb_or_listcmd():
            warn("known harmless STM32 ':leave' quirk (DFU detached too fast for "
                 "final get_status); treating as success")
            ok("DFU device gone -> firmware downloaded & app running")
            return True
        err("DFU device still present after exit 74; flash likely failed")
        return False

    err(f"dfu-util exited with code {r.returncode}")
    return False


# ---- main --------------------------------------------------------------------

def main() -> int:
    p = argparse.ArgumentParser(
        description="One-shot USB DFU upgrade helper for STM32G0 (CDC + ROM DFU).",
    )
    p.add_argument("binfile", type=Path, help="Path to firmware .bin")
    p.add_argument(
        "--port",
        help="Override CDC COM/tty (default: auto-discover by VID:PID)",
    )
    p.add_argument(
        "--skip-trigger",
        action="store_true",
        help="Skip sending 'dfu' command (assume device already in DFU mode, "
             "e.g. you forced system memory boot via STM32CubeProgrammer)",
    )
    p.add_argument(
        "--addr",
        type=lambda s: int(s, 0),
        default=0x08000000,
        help="Flash base address (default: 0x08000000)",
    )
    p.add_argument(
        "--dfu-util",
        help="Override dfu-util executable path (also reads $DFU_UTIL)",
    )
    args = p.parse_args()

    if not args.binfile.is_file():
        err(f"firmware not found: {args.binfile}")
        return 2

    # Fail-fast: 启动阶段就解析 dfu-util，找不到立即报错
    global DFU_UTIL
    resolved = _resolve_dfu_util(args.dfu_util)
    if not resolved:
        err("dfu-util not found")
        err("  Windows: scoop install dfu-util  (推荐)")
        err("  Linux:   sudo apt install dfu-util")
        err("  macOS:   brew install dfu-util")
        err("  或手动指定: --dfu-util <abs-path> / 设置 DFU_UTIL 环境变量")
        err("  注意: scoop / 新装的工具改的是用户级 PATH,")
        err("        当前 shell 进程必须重新启动才能生效")
        return 2
    DFU_UTIL = resolved
    info(f"using dfu-util: {DFU_UTIL}")

    if not args.skip_trigger:
        port = args.port or find_cdc_port()
        if not port:
            err(f"STM32 CDC device [{CDC_VID:04x}:{CDC_PID:04x}] not found")
            err("  is the board plugged in and running the application firmware?")
            err("  if you've already entered DFU manually, run with --skip-trigger")
            return 1

        if not trigger_dfu(port):
            return 1

        # 给 ROM bootloader 时间起来 + 主机重新枚举
        time.sleep(1.0)

    if not wait_for_dfu():
        return 1

    if not flash_firmware(args.binfile, addr=args.addr):
        return 1

    return 0


if __name__ == "__main__":
    sys.exit(main())
