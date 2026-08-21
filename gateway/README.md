# gateway —— Ubuntu 边缘网关（C + 华为 AgentLite SDK）

基于华为云 IoT AgentLite SDK 实现的边缘网关程序：
通过 TCP(18830) 聚合 gec6818 冷库监测节点数据，经 MQTT 上报华为云 IoTDA，
并把云端命令/属性设置转成 TCP 下行帧下发给设备。

## 1. 目录结构

    gateway/
    ├── gateway.c         主程序：读配置 → IOTA_Init → 回调注册 → IOTA_Connect → TCP server
    ├── cloud_bridge.c/h  华为 SDK 封装：配置解析 / 连接重连 / 属性批量上报 / 命令与属性回调
    ├── tcp_server.c/h    TCP 服务端：accept / 每连接读线程 / 按行解析帧 / 分发
    ├── protocol.c/h      设备↔网关 TCP 帧协议（register/data/ack 上行，cmd/query 下行）
    ├── device_table.c/h  在线设备表（node_id→fd/name/lastSeen/lastData，互斥锁保护）
    ├── ClientConf.json   网关配置
    ├── Makefile          编译脚本
    └── start.sh          一键编译运行脚本

## 2. 依赖

- 目标平台：Ubuntu x86-64，gcc、make。
- 华为 IoT AgentLite SDK：与本目录同级的 huaweicloud-iot-device-quickstart。
  目录关系固定为：

      huaweicloud-iot-device-quickstart/   （SDK：include/ lib/ conf/rootcert.pem）
      冷库监控系统/
      └── gateway/                         （本程序，须在 gateway/ 目录内运行）

- SDK 动态库：libHWMQTT.so、libpaho-mqtt3as.so、libssl.so.1.1、libcrypto.so.1.1，
  运行时由 start.sh 通过 LD_LIBRARY_PATH 指定。

## 3. 配置说明（ClientConf.json）

    {
      "deviceId": "f6cd4bbb1a8ab53acbb595efd0e90199_gateway01",
      "secret": "your_secret_here",
      "serverUri": "ssl://<接入地址>.iotda-device.cn-south-4.myhuaweicloud.com:8883",
      "tcpPort": 18830,
      "reportIntervalMs": 2000,
      "subDeviceIdPrefix": "",
      "defaultAlarmThreshold": -20
    }

| 字段 | 说明 |
|---|---|
| deviceId | 华为云网关（父设备）设备 ID，形如 产品ID_自定义名 |
| secret | 网关设备密钥 |
| serverUri | 华为云 MQTT 接入地址（TLS 8883），支持 ssl://host:8883 形式 |
| tcpPort | 设备 TCP 接入端口，默认 18830 |
| reportIntervalMs | 属性批量上报周期（毫秒），默认 2000 |
| subDeviceIdPrefix | 子设备 device_id 前缀，可为空（真实平台填 子设备产品ID_） |
| defaultAlarmThreshold | 新设备默认报警阈值（℃），默认 -20 |

## 4. 编译与运行

    cd 冷库监控系统/gateway
    bash start.sh              # 或 chmod +x start.sh 后 ./start.sh

等价的手动步骤：

    export LD_LIBRARY_PATH=../../huaweicloud-iot-device-quickstart/lib/:$LD_LIBRARY_PATH
    make
    ./gateway

命令行参数：./gateway [ClientConf.json 路径]，缺省使用 ./ClientConf.json。

    ./gateway /etc/coldstore/ClientConf.json

## 5. 数据流

上行：设备 TCP data 帧 → gateway_on_frame → device_table 更新 → 报告线程按
reportIntervalMs 批量调用 IOTA_BatchPropertiesReport（service_id=coldstorage，
properties 含 temperature/humidity/led/beeper/alarm/alarm_threshold）。

下行：云端命令回调 handleCommandRequest → 解析 object_device_id/command_name/paras
→ TCP cmd 帧（cmd_id=requestId）下发 → 设备 ack → cloud_bridge_on_device_ack →
IOTA_CommandResponse。属性设置 handlePropertiesSet → set_alarm_threshold 命令 +
IOTA_PropertiesSetResponse。

## 6. 真实华为云接入步骤摘要（详见 docs/04-华为云平台配置指南.md）

1. 开通 IoTDA 设备接入服务，记录 MQTT 接入地址（形如 `<接入地址>.iotda-device.cn-south-4.myhuaweicloud.com`，端口 8883）。
2. 创建产品：协议 MQTT、数据格式 JSON；模型定义添加服务 coldstorage（service_id 必须一致）：
   - 属性：temperature、humidity、led、beeper、alarm、alarm_threshold；
   - 命令：set_led、set_beeper、set_interval、set_alarm_threshold。
3. 注册网关设备：认证方式选密钥，记录 deviceId/secret，填入 ClientConf.json。
4. 子设备：device_id = subDeviceIdPrefix + node_id；可在平台静态注册，或由网关动态注册。
5. 修改 ClientConf.json 的 deviceId/secret/serverUri 后运行 start.sh。

## 7. 注意事项

- C 网关使用 SDK 自带的 paho-mqtt3as（TLS）库，面向华为云 ssl://host:8883。
  本地演示（明文 mqtt://127.0.0.1:1883）请使用 node-gateway/（协议与 C 网关一致）。
- IOTA_Init 通过 workPath（默认 ../../huaweicloud-iot-device-quickstart）读取 conf/rootcert.pem，
  故必须在 gateway/ 目录内运行，且 SDK 目录结构保持不动。
- 设备 60s 无上行判定离线并从设备表移除；连接断开按 fd 清理。
- 云连接断开由 handleConnectionLost / handleAuthFailure 回调自动重连（2s 退避）。

## 8. 常见问题

- 鉴权失败：核对 deviceId/secret 是否与平台一致、格式是否含下划线。
- 数据不上屏：核对产品模型 service_id 是否为 coldstorage、属性名是否一致。
- 命令无响应：确认设备已注册且在线；request_id 必须回填到响应 topic。
- 库加载失败：确认 LD_LIBRARY_PATH 指向 SDK 的 lib/ 目录。
