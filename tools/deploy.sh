#!/usr/bin/env bash
#
# 一键部署脚本 - GPU USB PWM Fan Controller
#
# 用法：
#   curl -fsSL https://raw.githubusercontent.com/scshsy/GPU-USB-PWM-Fan-Controller/main/tools/deploy.sh | sudo bash
#
# 或者手动：
#   sudo bash tools/deploy.sh
#
# 可选参数（通过环境变量传递）：
#   REPO_URL    - 自定义仓库地址（默认 GitHub）
#   BRANCH      - 自定义分支（默认 main）
#   INSTALL_DIR - 自定义安装目录（默认 /opt/pwm-fan）
#
set -eu
set -o pipefail 2>/dev/null || true

if [[ "${EUID}" -ne 0 ]]; then
  echo "ERROR: must run as root (use sudo)" >&2
  exit 1
fi

# --- 配置 ---
REPO_URL="${REPO_URL:-https://github.com/scshsy/GPU-USB-PWM-Fan-Controller.git}"
BRANCH="${BRANCH:-main}"
INSTALL_DIR="${INSTALL_DIR:-/opt/pwm-fan}"

echo "============================================"
echo " GPU USB PWM Fan Controller - 一键部署"
echo "============================================"
echo ""
echo "[i] repo:  ${REPO_URL}"
echo "[i] branch: ${BRANCH}"
echo "[i] install dir: ${INSTALL_DIR}"
echo ""

# --- 1. 安装系统依赖 ---
echo "[1/4] installing OS packages..."
export DEBIAN_FRONTEND=noninteractive
apt-get update -y
apt-get install -y --no-install-recommends \
  git python3 python3-serial python3-pip \
  systemd \
  gawk

# --- 2. 从 GitHub 拉取代码 ---
echo "[2/4] cloning repository..."
if [[ -d "${INSTALL_DIR}" ]]; then
  echo "[i] directory exists, pulling latest..."
  cd "${INSTALL_DIR}"
  git fetch origin
  git reset --hard "origin/${BRANCH}"
else
  rm -rf "${INSTALL_DIR}"
  git clone -b "${BRANCH}" "${REPO_URL}" "${INSTALL_DIR}"
fi

TOOL_DIR="${INSTALL_DIR}/tools"

if [[ ! -f "${TOOL_DIR}/fan_daemon.py" ]] || [[ ! -f "${TOOL_DIR}/pwm_fan_cli.py" ]]; then
  echo "ERROR: required scripts not found in repository" >&2
  exit 1
fi

# --- 3. 安装程序文件 ---
echo "[3/4] installing files..."
INSTALL_LIB_DIR="/usr/local/lib/pwm_fan"
INSTALL_BIN_DIR="/usr/local/bin"
CONFIG_DIR="/etc/pwm_fan"
CONFIG_FILE="${CONFIG_DIR}/config"

install -d -m 0755 "${INSTALL_LIB_DIR}"
install -d -m 0755 "${INSTALL_BIN_DIR}"
install -d -m 0755 "${CONFIG_DIR}"

install -m 0755 "${TOOL_DIR}/fan_daemon.py" "${INSTALL_LIB_DIR}/fan_daemon.py"
install -m 0755 "${TOOL_DIR}/pwm_fan_cli.py" "${INSTALL_LIB_DIR}/pwm_fan_cli.py"

if [[ ! -f "${CONFIG_FILE}" ]]; then
  if [[ -f "${TOOL_DIR}/pwm_fan_config.example" ]]; then
    echo "[i] installing default config: ${CONFIG_FILE}"
    install -m 0644 "${TOOL_DIR}/pwm_fan_config.example" "${CONFIG_FILE}"
  else
    echo "[i] no example config found, skipping"
  fi
else
  echo "[i] config exists, keeping: ${CONFIG_FILE}"
fi

# --- 4. 安装系统命令 ---
echo "[4/4] installing systemd service and commands..."

# pwm-fan-rpm
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
  if [[ $follow -eq 1 ]]; then
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

# pwm-fan-status
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

# pwm-fan-help
cat > "${INSTALL_BIN_DIR}/pwm-fan-help" <<'EOF'
#!/usr/bin/env bash
set -eu

cat <<'HELP'
PWM Fan Controller
==================

做什么：读取 GPU 温度 (nvidia-smi) → 计算 PWM duty → USB CDC 下发到风扇控制板
主机失联时板子 3 秒后自动回安全档（80%）

重要文件
- 配置：/etc/pwm_fan/config
- 日志：journalctl -u pwm-fan -f
- 代码：/opt/pwm-fan

常用命令
- pwm-fan-rpm           查询转速
- pwm-fan-status        查询综合状态
- pwm-fan-rpm -f        持续打印转速
- pwm-fan-status -f     持续打印状态
- pwm-fan-help          显示此帮助

systemd 管理
- sudo systemctl status pwm-fan.service
- sudo systemctl restart pwm-fan.service   （改配置后）
- sudo systemctl stop pwm-fan.service
- sudo systemctl enable pwm-fan.service    （开机自启）
HELP
EOF
chmod 0755 "${INSTALL_BIN_DIR}/pwm-fan-help"

# systemd service
cat > /etc/systemd/system/pwm-fan.service <<'EOF'
[Unit]
Description=PWM Fan Daemon (GPU temp -> USB PWM fan controller)
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
ExecStart=/usr/bin/python3 /usr/local/lib/pwm_fan/fan_daemon.py --config /etc/pwm_fan/config
Restart=always
RestartSec=1
Environment=PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin:/usr/lib/wsl/lib
StandardOutput=journal
StandardError=journal

[Install]
WantedBy=multi-user.target
EOF

systemctl daemon-reload
systemctl enable pwm-fan.service
systemctl restart pwm-fan.service

echo ""
echo "============================================"
echo " 部署完成"
echo "============================================"
echo ""
echo " 查看帮助:   pwm-fan-help"
echo " 查看状态:   pwm-fan-status"
echo " 查看转速:   pwm-fan-rpm"
echo " 实时日志:   journalctl -u pwm-fan -f"
echo " 修改配置:   vi /etc/pwm_fan/config"
echo " 重启服务:   sudo systemctl restart pwm-fan.service"
echo ""
