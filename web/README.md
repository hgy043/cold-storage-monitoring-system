# 冷库数据监控系统 - Web 后端与 H5 大屏

单文件 Node 后端（内嵌 aedes MQTT broker + REST + WebSocket + 数据存储），
以及一个深色冷库风格的可视化大屏（原生 JS + ECharts，中文 UI，B/S 架构）。

## 目录

- server.js          单文件后端（broker + REST + WS + 存储 + 告警）
- package.json       依赖与启动脚本
- public/index.html  大屏页面
- public/style.css   深色大屏样式
- public/app.js      前端逻辑（WS 连接、卡片、图表、日志）
- data/history.json  运行时自动生成的持久化快照（首次运行后出现）

## 运行

要求 Node.js >= 18。

    cd web
    npm install
    npm start

打开浏览器访问 http://127.0.0.1:8080

默认端口与环境变量：

| 项 | 默认值 | 环境变量 |
|---|---|---|---|
| MQTT broker | 1883 | MQTT_PORT |
| HTTP 大屏 | 8080 | PORT |
| 网关 deviceId | demo_gateway_01 | GATEWAY_ID |

也可在 web/config.json 中写入 gatewayId / mqttPort / httpPort（可选，缺省用默认值）。

## REST API 一览

| 方法 | 路径 | 说明 |
|---|---|---|
| GET | /api/status | 系统状态：gatewayOnline / deviceCount / onlineCount / brokerPort / uptime |
| GET | /api/devices | 已知设备列表（含最新数据与 online 状态） |
| GET | /api/devices/:id | 单设备最新数据 + 汇总统计（样本数、温湿度 avg/max/min） |
| GET | /api/devices/:id/history?points=60 | 温度/湿度时间序列，按 points 桶聚合出 avg/max/min 三元组 |
| GET | /api/devices/:id/agg?window=3600 | 窗口内温湿度 max/avg/min |
| GET | /api/stats | 16 冷库当前温度聚合（供柱状图，含 avg/threshold/alarm） |
| POST | /api/devices/:id/commands | 下发命令，body: {command, paras}，返回 {requestId} |
| PUT | /api/devices/:id/properties | 设置属性，body: {alarm_threshold}，返回 {requestId} |
| GET | /api/events?limit=100 | 事件日志（设备上线/下线/数据/命令/告警，新事件在前） |

支持的命令（command）：set_led、set_beeper、set_interval、set_alarm_threshold。

示例：

    # 下发 LED 开（object_device_id = cold01_front）
    curl -X POST http://127.0.0.1:8080/api/devices/cold01_front/commands \
      -H "Content-Type: application/json" \
      -d "{\"command\":\"set_led\",\"paras\":{\"on\":1}}"

    # 设置报警阈值
    curl -X PUT http://127.0.0.1:8080/api/devices/cold01_front/properties \
      -H "Content-Type: application/json" \
      -d "{\"alarm_threshold\":-25}"

## WebSocket 事件（路径 /ws）

连接后服务端立即推送 status、最新数据快照、最近事件；随后实时广播：

| type | 说明 |
|---|---|
| status | 网关在线 / 设备数 / 在线数变化 |
| data | devices 列表 + stats 聚合，用于刷新卡片与柱状图 |
| ack | 命令/属性设置回执（requestId / result_code / response） |
| event | 单条事件（data/status/ack/event 之外的业务事件，如上线/告警） |

## 与 node-gateway 联调

完整链路（本机演示，见 docs/05-部署与联调.md）：

    node web/server.js                 # 1) broker + 后端 + 大屏 http://127.0.0.1:8080
    node node-gateway/gateway.js       # 2) 网关连接 broker（1883）+ TCP:18830
    node tools/simulate_devices.js     # 3) 模拟 16 冷库 × 前后门 = 32 节点

数据流：设备 TCP → 网关聚合 → MQTT 属性上报（$oc 主题）→ 本后端订阅落库 →
WebSocket 推送到大屏。大屏按钮 → REST → 本后端 → MQTT 命令下发 → 网关 → TCP → 设备 →
ack 回网关 → MQTT 命令响应 → 本后端 → WS 回执 → 大屏事件日志。

主题契约见 docs/03-网关-云MQTT对接.md：

- 上行订阅：$oc/devices/+/sys/gateway/sub-devices/properties/report
- 命令响应：$oc/devices/+/sys/commands/response/#
- 属性响应：$oc/devices/+/sys/properties/set/response/#
- 命令下发：$oc/devices/{gatewayId}/sys/commands/request_id={requestId}
- 属性下发：$oc/devices/{gatewayId}/sys/properties/set/request_id={requestId}

## 说明

- 后端仅依赖 aedes / ws / mqtt，HTTP 使用原生 http 模块路由。
- 演示 broker 放行所有连接（不校验 username/password）。
- 每 30 秒把最新数据快照与近期事件写入 data/history.json，启动时自动加载。
- 每个节点保留最新数据 + 历史环形数组（上限 3000 条）。
- 设备 60 秒无数据判定离线；温度超过 alarm_threshold 产生告警并广播。
