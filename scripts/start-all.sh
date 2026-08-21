#!/usr/bin/env bash
# 冷库监控系统一键启动（Linux/macOS）：后台运行 + 日志文件 + trap 清理
set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT" || exit 1

LOG_DIR="$ROOT/logs"
mkdir -p "$LOG_DIR"

PIDS=""

cleanup() {
  echo ""
  echo "正在停止全部进程..."
  for pid in $PIDS; do
    kill "$pid" 2>/dev/null || true
  done
  wait 2>/dev/null || true
  echo "已停止。"
  exit 0
}

trap cleanup INT TERM

node web/server.js > "$LOG_DIR/web.log" 2>&1 &
PIDS="$PIDS $!"
echo "已启动 Web大屏 (pid $!)"

node node-gateway/gateway.js > "$LOG_DIR/gateway.log" 2>&1 &
PIDS="$PIDS $!"
echo "已启动 Node网关 (pid $!)"

node tools/simulate_devices.js > "$LOG_DIR/devices.log" 2>&1 &
PIDS="$PIDS $!"
echo "已启动 模拟设备 (pid $!)"

echo ""
echo "日志目录: $LOG_DIR"
echo "按 Ctrl+C 停止全部进程。"
wait
