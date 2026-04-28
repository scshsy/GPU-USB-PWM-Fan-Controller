#!/usr/bin/env bash

# Ubuntu Server 安装脚本（幂等 / 可重复执行）
#
# 注意：本文件必须是 LF 换行（不要用 CRLF），否则在 Linux 上会报 $'\r' 错误。
#
# 安装内容：
#   - systemd 服务：pwm-fan.service（自动重启，永久常驻）
#   - 配置文件：/etc/pwm_fan/config（若不存在则从 tools/pwm_fan_config.example 拷贝）
#   - 程序文件：
#       /usr/local/lib/pwm_fan/fan_daemon.py   （来自 tools/fan_daemon.py）
#       /usr/local/lib/pwm_fan/pwm_fan_cli.py  （来自 tools/pwm_fan_cli.py）
#   - 系统命令：
#       pwm-fan-rpm     -> 查询当前 RPM（服务在跑时从 journald 取最近值；否则直连串口）
#       pwm-fan-status  -> 查询当前状态（服务在跑时从 journald 取最近值；否则直连串口）
#       pwm-fan-help    -> 新手帮助
#
# 用法：
#   sudo bash tools/install.sh

set -eu
set -o pipefail 2>/dev/null || true

if [[ "${EUID}" -ne 0 ]]; then
  echo "ERROR: must run as root (use sudo)" >&2
  exit 1
fi

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

echo "[i] root_dir=${ROOT_DIR}"

echo "[i] installing OS packages..."
export DEBIAN_FRONTEND=noninteractive
apt-get update -y
apt-get install -y --no-install-recommends \
  python3 python3-serial python3-pip \
  systemd \
  gawk

INSTALL_LIB_DIR="/usr/local/lib/pwm_fan"
INSTALL_BIN_DIR="/usr/local/bin"
CONFIG_DIR="/etc/pwm_fan"
CONFIG_FILE="${CONFIG_DIR}/config"

echo "[i] creating directories..."
install -d -m 0755 "${INSTALL_LIB_DIR}"
install -d -m 0755 "${INSTALL_BIN_DIR}"
install -d -m 0755 "${CONFIG_DIR}"

echo "[i] installing python scripts..."
install -m 0755 "${ROOT_DIR}/tools/fan_daemon.py" "${INSTALL_LIB_DIR}/fan_daemon.py"
install -m 0755 "${ROOT_DIR}/tools/pwm_fan_cli.py" "${INSTALL_LIB_DIR}/pwm_fan_cli.py"

if [[ ! -f "${CONFIG_FILE}" ]]; then
  echo "[i] installing default config from example: ${CONFIG_FILE}"
  install -m 0644 "${ROOT_DIR}/tools/pwm_fan_config.example" "${CONFIG_FILE}"
else
  echo "[i] config exists, keep it: ${CONFIG_FILE}"
fi

echo "[i] installing helper commands..."

cat > "${INSTALL_BIN_DIR}/pwm-fan-rpm" <<'EOF'
#!/usr/bin/env bash
set -eu
set -o pipefail 2>/dev/null || true

follow=0
if [[ "${1:-}" == "-f" ]]; then
  follow=1
  shift
fi

if systemctl is-active --quiet pwm-fan.service; then
  # 注意：在 set -e 下，命令替换失败会直接退出，所以用 if ! ...; then 捕获
  if [[ $follow -eq 1 ]]; then
    # 输出最近 20 条 + 持续跟随（只打印 rpm 行）
    exec journalctl -u pwm-fan -n 20 -f -o cat \
      | awk '/rpm 1=/{p=index($0,"rpm 1="); if(p>0){print substr($0,p); fflush()}}'
  else
    if ! out="$(
      journalctl -u pwm-fan -n 300 --no-pager -o cat \
        | awk '/rpm 1=/{line=$0} END{if(line!=""){p=index(line,"rpm 1="); print substr(line,p)} else exit 2}'
    )"; then
      rc=$?
      if [[ $rc -eq 2 ]]; then
        echo "ERR: pwm-fan.service running but no rpm line in journal yet." >&2
        echo "     check: sudo systemctl status pwm-fan.service" >&2
        echo "     logs:  sudo journalctl -u pwm-fan -f" >&2
        exit 2
      fi
      exit "$rc"
    fi
    printf '%s\n' "$out"
  fi
else
  if [[ $follow -eq 1 ]]; then
    # 没有服务时：循环直连查询（默认 1Hz）
    while true; do
      python3 /usr/local/lib/pwm_fan/pwm_fan_cli.py "$@" rpm || true
      sleep 1
    done
  else
    exec python3 /usr/local/lib/pwm_fan/pwm_fan_cli.py "$@" rpm
  fi
fi
EOF
chmod 0755 "${INSTALL_BIN_DIR}/pwm-fan-rpm"

cat > "${INSTALL_BIN_DIR}/pwm-fan-status" <<'EOF'
#!/usr/bin/env bash
set -eu
set -o pipefail 2>/dev/null || true

follow=0
if [[ "${1:-}" == "-f" ]]; then
  follow=1
  shift
fi

if systemctl is-active --quiet pwm-fan.service; then
  if [[ $follow -eq 1 ]]; then
    # 输出最近 20 条 + 持续跟随（只打印 board 行，去掉前缀）
    exec journalctl -u pwm-fan -n 20 -f -o cat \
      | awk '/board /{p=index($0,"board "); if(p>0){print substr($0,p+6); fflush()}}'
  else
    if ! out="$(
      journalctl -u pwm-fan -n 300 --no-pager -o cat \
        | awk '/board /{line=$0} END{if(line!=""){p=index(line,"board "); print substr(line,p+6)}else exit 2}'
    )"; then
      rc=$?
      if [[ $rc -eq 2 ]]; then
        echo "ERR: pwm-fan.service running but no board status line in journal yet." >&2
        echo "     check: sudo systemctl status pwm-fan.service" >&2
        echo "     logs:  sudo journalctl -u pwm-fan -f" >&2
        exit 2
      fi
      exit "$rc"
    fi
    printf '%s\n' "$out"
  fi
else
  if [[ $follow -eq 1 ]]; then
    # 没有服务时：循环直连查询（默认 1Hz）
    while true; do
      python3 /usr/local/lib/pwm_fan/pwm_fan_cli.py "$@" status || true
      sleep 1
    done
  else
    exec python3 /usr/local/lib/pwm_fan/pwm_fan_cli.py "$@" status
  fi
fi
EOF
chmod 0755 "${INSTALL_BIN_DIR}/pwm-fan-status"

cat > "${INSTALL_BIN_DIR}/pwm-fan-help" <<'EOF'
#!/usr/bin/env bash
set -eu

cat <<'HELP'
PWM Fan Controller（新手帮助）
=========================

这套系统做什么？
- 读取 GPU 温度（nvidia-smi）→ 计算 PWM duty（%）→ 通过 USB CDC 下发到 STM32 风扇控制板
- 若主机脚本崩溃/失联，板子 3 秒后自动回安全档（默认 80%）

重要文件
- 服务：pwm-fan.service
- 配置：/etc/pwm_fan/config
- 日志：journalctl -u pwm-fan -f

常用命令（服务）
- 看状态：
  sudo systemctl status pwm-fan.service
- 启动：
  sudo systemctl start pwm-fan.service
- 停止：
  sudo systemctl stop pwm-fan.service
- 重启（改完配置后用这个）：
  sudo systemctl restart pwm-fan.service
- 开机自启：
  sudo systemctl enable pwm-fan.service
- 取消开机自启：
  sudo systemctl disable pwm-fan.service

常用命令（看日志）
- 实时滚动：
  sudo journalctl -u pwm-fan -f
- 看最近 200 行：
  sudo journalctl -u pwm-fan -n 200 --no-pager

常用命令（查询）
- 查询转速（服务在跑时从日志取最近值）：
  pwm-fan-rpm
- 查询综合状态（服务在跑时从日志取最近值）：
  pwm-fan-status
- 持续打印状态（类似 tail -f）：
  pwm-fan-status -f   （输出最近 20 条 + 持续跟随）
- 持续打印转速（类似 tail -f）：
  pwm-fan-rpm -f      （输出最近 20 条 + 持续跟随）

提示：
- 如果你手动运行 fan_daemon.py，会占用串口；与 pwm-fan.service 同时运行会冲突。
- 推荐只用 systemd 服务常驻，然后用 pwm-fan-status / pwm-fan-rpm 看最近状态。
- 如果日志提示找不到 CDC 设备（0483:5740）：
  - 先确认风扇控制板已插入，并能看到 /dev/ttyACM0
  - 若你在 WSL2 里运行 Ubuntu，需要先在 Windows 用 usbipd 把设备 attach 进 WSL，否则 WSL 永远看不到该 USB 设备

HELP
EOF
chmod 0755 "${INSTALL_BIN_DIR}/pwm-fan-help"

echo "[i] installing systemd service..."
SERVICE_FILE="/etc/systemd/system/pwm-fan.service"
cat > "${SERVICE_FILE}" <<'EOF'
[Unit]
Description=PWM Fan Daemon (GPU temp -> USB PWM fan controller)
After=network-online.target
Wants=network-online.target
# 兼容旧版 systemd：不要使用 StartLimitIntervalSec 这类较新的键名

[Service]
Type=simple
ExecStart=/usr/bin/python3 /usr/local/lib/pwm_fan/fan_daemon.py --config /etc/pwm_fan/config
Restart=always
RestartSec=1

# WSL2 的 nvidia-smi 通常在 /usr/lib/wsl/lib/，systemd 默认 PATH 未必包含
Environment=PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin:/usr/lib/wsl/lib

StandardOutput=journal
StandardError=journal

[Install]
WantedBy=multi-user.target
EOF

systemctl daemon-reload
systemctl enable pwm-fan.service
systemctl restart pwm-fan.service

echo "[+] installed."
echo "    - help:     pwm-fan-help"
echo "    - status:   pwm-fan-status"
echo "    - rpm:      pwm-fan-rpm"
echo "    - logs:     journalctl -u pwm-fan -f"

