@echo off
chcp 65001 >nul
setlocal

rem 冷库监控系统一键启动（Windows）：三个独立窗口
rem 先切换到项目根目录（本脚本位于 scripts\ 目录下）
cd /d "%~dp0.."

start "Web大屏" cmd /k "node web\server.js"
start "Node网关" cmd /k "node node-gateway\gateway.js"
start "模拟设备" cmd /k "node tools\simulate_devices.js"

echo 已启动三个窗口：Web大屏 / Node网关 / 模拟设备
endlocal
