# Node 网关（node-gateway）

冷库数据监控系统的 Node.js 跨平台网关，与 C 网关（AgentLite SDK 封装）实现同一套协议，
用于本机演示与协议教学，Windows / Linux / macOS 均可运行。

## 目录结构

    node-gateway/
        package.json   依赖声明（仅 mqtt ^5）
        conf.json      运行配置
        gateway.js     网关主程序
        README.md      本文件

## 运行方式

前置：Node.js >= 18。

    cd node-gateway
    npm install      # 安装 mqtt 依赖
    npm start        # 等价于 node gateway.js

启动前确保本地 broker 已就绪（web/server.js 内嵌 aedes，监听 1883），
并按需启动设备模拟器（tools/simulate_devices.js 连接 TCP:18830）。

## 配置说明（conf.json）

    broker             MQTT 地址；本机演示 mqtt://127.0.0.1:1883，真实平台填 TLS 8883
    deviceId           网关（父设备）ID，同时作为 MQTT username
    secret             MQTT password（华为云按此鉴权，演示 broker 放行全部）
    tcpPort            网关 TCP 监听端口，默认 18830
    reportIntervalMs   批量上报周期（毫秒），默认 2000
    subDeviceIdPrefix  子设备云端 device_id 前缀；演示留空，真实平台 = 子设备产品ID_ + node_id
    offlineTimeoutMs   设备无数据判离线超时（毫秒），默认 60000

## 与 C 网关的对应关系

| 环节 | C 网关 | Node 网关 |
| --- | --- | --- |
| TCP 服务端 | gateway/ 监听 18830 | net.createServer 监听 conf.tcpPort |
| 帧协议 | 02 文档逐行 JSON | 同（register/register_ack/data/ack/cmd/query） |
| MQTT 接入 | AgentLite SDK 封装 | mqtt 包直连 broker |
| 认证 | deviceId / secret | username=deviceId，password=secret |
| 子设备上报 | 聚合发布 sub-devices/properties/report | 同主题、同载荷 |
| 命令下发 | commands/# -> TCP cmd -> ack -> commands/response | 同 |
| 属性设置 | properties/set/# -> set_alarm_threshold -> set/response | 同 |

## MQTT 主题实现自检

上行发布：

    $oc/devices/{deviceId}/sys/gateway/sub-devices/properties/report

下行订阅：

    $oc/devices/{deviceId}/sys/commands/#
    $oc/devices/{deviceId}/sys/properties/set/#
    $oc/devices/{deviceId}/sys/messages/down

上行响应发布：

    $oc/devices/{deviceId}/sys/commands/response/request_id={requestId}
    $oc/devices/{deviceId}/sys/properties/set/response/request_id={requestId}

## 健壮性说明

    - TCP 半包 / 粘包：按 \n 切分缓冲，不完整帧留在缓冲，一次处理多帧。
    - 帧超长（>4096 字节）丢弃并告警。
    - MQTT 断线：mqtt 包自动重连，reconnect 事件打印日志。
    - dirty 数据：上报失败或断线时保留 dirty，重连后补报；上报期间来新数据不丢。
    - 设备 60s 无数据自动判离线，连接断开立即判离线。
    - SIGINT / SIGTERM 优雅退出，关闭 MQTT、TCP 服务与所有连接。