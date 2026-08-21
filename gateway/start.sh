#!/bin/bash
# =====================================================================
#  冷库监控系统 - Ubuntu 边缘网关启动脚本
#  用法：./start.sh [ClientConf.json 路径]
#  说明：先导出 SDK 动态库路径，再编译并运行网关
# =====================================================================

# 切到脚本所在目录，保证相对路径 ../../huaweicloud-iot-device-quickstart 正确
cd "$(dirname "$0")"

# 导出华为 AgentLite SDK 动态库路径（libHWMQTT.so / libpaho-mqtt3as.so / libssl / libcrypto）
export LD_LIBRARY_PATH=../../huaweicloud-iot-device-quickstart/lib/:$LD_LIBRARY_PATH

# 编译
make

# 运行；argv[1] 可指定 ClientConf.json 路径
./gateway "$@"
