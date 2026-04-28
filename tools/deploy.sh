#!/usr/bin/env bash
#
# 一键部署脚本 - GPU USB PWM Fan Controller
#
# 用法：
#   curl -fsSL https://raw.githubusercontent.com/scshsy/GPU-USB-PWM-Fan-Controller/main/tools/deploy.sh | sudo bash
#
# 或者手动（有源码时）：
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

# --- 1. 从 GitHub 拉取代码 ---
echo "[1/2] cloning repository..."
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

if [[ ! -f "${TOOL_DIR}/install.sh" ]]; then
  echo "ERROR: install.sh not found in repository" >&2
  exit 1
fi

# --- 2. 调用 install.sh 完成安装 ---
echo "[2/2] running install.sh..."
cd /
bash "${TOOL_DIR}/install.sh"

# 安装完成，清理拉取的代码
echo "[i] cleaning up cloned repository..."
rm -rf "${INSTALL_DIR}"

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
