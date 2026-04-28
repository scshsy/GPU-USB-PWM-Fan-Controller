# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

GPU USB PWM Fan Controller: an STM32G0B1-based device that controls two 4-pin fans via USB CDC text protocol. A host PC script reads `nvidia-smi` GPU temperatures, computes duty cycles, and sends commands over CDC. The firmware enforces fail-safe: if USB disconnects or the host watchdog times out (>3s), fans automatically fall back to 80% duty.

## Repository Layout

The project root is the PlatformIO workspace. Key directories:

- `src/` — Firmware source (C). Modules are single-purpose files with paired `.c`/`.h`:
  - `main.c` — Startup sequence + non-blocking main loop (all tasks are `*_Service()` calls)
  - `fanctl.{c,h}` — Fan control arbitration (DEFAULT → HOST → SAFE → OVERRIDE state machine)
  - `pwm.{c,h}` — TIM1 CH1/CH2 25kHz PWM output (PA8/PA9)
  - `tach.{c,h}` — TIM3 input capture for TACH RPM (PA6/PA7, 2 PPR)
  - `cli.{c,h}` — Line-protocol framework over CDC (handler registry, echo fallback)
  - `dfu.{c,h}` — `dfu` command + magic-byte jump to ROM bootloader
  - `usb/*` — USB CDC device stack glue (usbd_conf, usbd_desc, usbd_cdc_if, usb_device)
- `lib/STM32_USB_Device/` — STM32 USB Device middleware (STM32Cube library)
- `tools/` — Host-side Python/shell scripts:
  - `fan_daemon.py` — GPU temp → duty daemon (reads nvidia-smi, sends CDC commands)
  - `dfu_upload.py` — One-click DFU flash (CDC trigger → ROM DFU → dfu-util)
  - `pwm_fan_cli.py` — One-shot query/set CLI
  - `install.sh` — Ubuntu systemd install script (idempotent)
- `hardware/` — JLCPC EasyEDA professional project (`GPU USB PWM Fan Controller.epro`)
- `docs/` — Detailed documentation (FAN_CONTROL.md, USB_CDC_GUIDE.md, USB_DFU_GUIDE.md, PROJECT_GUIDE.md)

## Build and Deploy

```bash
# Compile firmware
pio run -e nucleo_g0b1re

# One-click DFU flash (board must be running app, sends "dfu" over CDC to trigger bootloader)
python tools/dfu_upload.py .pio/build/nucleo_g0b1re/firmware.bin

# Serial monitor
pio device monitor -b 115200
```

Board: ST Nucleo-G0B1RE, STM32G0B1CBT6 MCU, ST-Link upload protocol.

## CDC Text Protocol

Line-terminated (`\r\n` or `\n`), case-insensitive. Commands sent/received via `pio device monitor` or the Python tools:

| Command | Response | Notes |
|---|---|---|
| `set fan <1\|2> <0..100>` | `ok` / `err <reason>` | Refreshes host watchdog |
| `kick` | `ok` | Refresh watchdog only |
| `get rpm` | `rpm 1=<n> 2=<m>` | Returns both channels |
| `get status` | `duty=<a>,<b> rpm=... usb=<0/1> src=<host\|safe\|default\|override> wd=<ms> fault=0x<bit>` | Full state |
| `?` | CRS diagnostic line | Internal clock calibration status |
| `dfu` | enters ROM DFU then resets | For `dfu_upload.py` |

## Architecture Patterns

- **Non-blocking everywhere:** All modules expose `*_Init()` and `*_Service(now_ms)` functions. The main loop calls each service once per iteration — no blocking calls.
- **CLI handler registration:** Modules register command handlers in their `Init()` via `CLI_RegisterHandler()`. First registered wins; unhandled lines fall back to echo.
- **USB CDC layering:** `usbd_cdc_if.c` bridges STM32 USB stack to the CLI. RX bytes go to a ring buffer; TX uses `CDC_Transmit_NonBlocking` with 5ms software timeout.
- **USB IRQ sharing:** STM32G0 USB/UCPD1/UCPD2 share IRQ28. `HAL_PCD_IRQHandler` reads `SYSCFG->IT_LINE_SR[8]` to identify the source. SYSCFG clock must be enabled in `HAL_PCD_MspInit`.

## Key Documentation

- `docs/FAN_CONTROL.md` — Protocol specification, safety state machine, wiring
- `docs/USB_CDC_GUIDE.md` — USB CDC architecture and troubleshooting
- `docs/USB_DFU_GUIDE.md` — DFU flashing details
- `docs/PROJECT_GUIDE.md` — Full project documentation
