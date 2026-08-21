# tools 目录说明

本目录包含两个纯 Node 工具，仅使用内置 net 模块，无第三方依赖，无需 npm install。

## 1. simulate_devices.js —— 模拟设备工具

模拟 N 个冷库 × 前/后 门 = 2N 个 TCP 设备节点，连接网关后按帧协议收发数据。

### 用法

    node tools/simulate_devices.js [选项]

选项：

    --count <N>        冷库数量，默认 16（节点数 = N×2）
    --host <ip>        网关地址，默认 127.0.0.1
    --port <p>         网关端口，默认 18830
    --interval <s>     上报周期秒，默认 5
    --help             显示帮助

示例：

    # 默认：16 冷库 × 前后 = 32 节点
    node tools/simulate_devices.js

    # 快速联调：单冷库 2 节点
    node tools/simulate_devices.js --count 1

    # 自定义网关与周期
    node tools/simulate_devices.js --host 127.0.0.1 --port 18830 --interval 10

### 节点行为

- node_id 命名 cold01_front / cold01_back ... cold16_back，name 形如 1号冷库-前门。
- 连接后先发 register，收到 register_ack 后按周期发 data。
- 温度基准 -18℃，随机游走 ±0.3/步；每 60~120 步随机一次尖峰（-8~5℃，模拟故障/开门）。
- 湿度 55~75 随机游走；LED/蜂鸣器偶尔随机变化。
- 本地报警：temperature > alarm_threshold（初始 -20）时 alarm=1。
- 下行帧处理：
    - set_led → 更新 LED 并回 ack
    - set_beeper → on 时 beeper=1，duration_ms 后自动回 0，回 ack
    - set_interval → 更新上报周期，回 ack
    - set_alarm_threshold → 更新阈值，回 ack
    - query → 立即补发 data 并回 ack
- 断线自动重连，退避 1s、2s、4s... 上限 30s。
- 控制台按节点打印关键事件，时间戳格式 [HH:MM:SS]。
- SIGINT（Ctrl+C）优雅退出。

## 2. tcp_probe.js —— TCP 调试工具

连接指定 host:port，把收到的每帧带时间戳打印，可发送一帧后继续接收。

### 用法

    node tools/tcp_probe.js [选项]

选项：

    --host <ip>        目标主机，默认 127.0.0.1
    --port <p>         目标端口，默认 18830
    --send '<json>'    连接后发送一帧 JSON，然后继续接收
    --help             显示帮助

示例：

    # 只监听接收
    node tools/tcp_probe.js

    # 查询状态（发送 query 后继续接收 data + ack）
    node tools/tcp_probe.js --send '{"type":"query","cmd_id":"req_abc","query":"status"}' (node_id 自动补上)

按 Ctrl+C 退出。

## 3. 帧协议

严格遵循 docs/02-设备-网关TCP协议.md：一帧一行，UTF-8 JSON，以单个换行符结尾。

上行：register / data / ack
下行：register_ack / cmd / query