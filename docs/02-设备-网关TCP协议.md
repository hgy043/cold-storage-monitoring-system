# 02-设备-网关 TCP 协议 v1

> 契约方：`device/`、`gateway/`、`node-gateway/`、`tools/simulate_devices.js`、`virtual-server/`、`device/ui/`
> 本协议同时被 C 网关与 Node 网关实现，二者可互换（**仅限 §3 register/data/ack 与 §4.1/4.2 cmd/query 帧**）。
> **§4.3 virt_data / §4.4 confirm 是"终端 ↔ 转发程序"链路的扩展帧，仅由 `virtual-server/virtual_bridge.js` 与
> `device/ui/ui_main.c`（gec6818 终端）实现**；C 网关与 Node 网关收到会按"未知帧类型"忽略——终端请勿直连 C/Node 网关。

## 1. 传输层

- 网关侧 TCP 监听端口：**18830**（可在配置中修改）。
- 设备侧主动连接网关（设备为 TCP 客户端，网关为 TCP 服务端）。
- **一帧一行**：每帧是一个 UTF-8 编码的 JSON 对象，以单个 `\n` 结尾。
- 单帧最大长度 4096 字节，超长丢弃并告警。
- 连接断开后，设备应自动重连（退避重试 1s、2s、4s… 上限 30s）。

## 2. 节点标识

- 每个监测节点一个 `node_id`，命名规范 `cold<NN>_<front|back>`。
- 子设备在云平台的 `device_id` = `subDeviceIdPrefix + node_id`（prefix 可为空，见 03 文档）。

## 3. 上行帧（设备 → 网关）

### 3.1 注册 register

设备连接建立后第一条消息必须是注册帧。

```json
{"type":"register","node_id":"cold01_front","name":"1号冷库-前门","interval":5,"model":"gec6818-sim"}
```

| 字段 | 必填 | 说明 |
|---|---|---|
| type | 是 | 固定 "register" |
| node_id | 是 | 节点标识 |
| name | 否 | 显示名称 |
| interval | 否 | 数据上报周期（秒），默认 5 |
| model | 否 | 设备型号（真实板子填 "gec6818"，模拟填 "gec6818-sim"） |

网关收到后回复 **register_ack**：

```json
{"type":"register_ack","node_id":"cold01_front","result":"success","server_time":"2025-01-01T12:00:00+08:00"}
```

### 3.2 数据上报 data

```json
{"type":"data","node_id":"cold01_front","ts":"2025-01-01T12:00:00+08:00","temperature":-18.5,"humidity":62.3,"led":0,"beeper":0,"alarm":0}
```

| 字段 | 类型 | 说明 |
|---|---|---|
| temperature | number | 温度（℃） |
| humidity | number | 相对湿度（%） |
| led | 0/1 | LED 当前状态 |
| beeper | 0/1 | 蜂鸣器当前状态 |
| alarm | 0/1 | 本机超阈值告警标志（设备本地判定 temperature > alarm_threshold 时置 1） |

### 3.3 命令回执 ack

设备执行完下行命令后回执；网关据此向云端回命令响应。

```json
{"type":"ack","node_id":"cold01_front","cmd_id":"req_9f3a","result":"success","detail":"LED on"}
```

`result` 取值：success / failed。`detail` 为人类可读说明。

## 4. 下行帧（网关 → 设备）

### 4.1 命令下发 cmd

```json
{"type":"cmd","cmd_id":"req_9f3a","command":"set_led","paras":{"on":1}}
```

支持的命令集（云端 command_name 与此一致）：

| command | paras | 说明 |
|---|---|---|
| set_led | {"on": 0|1} | 开关 LED |
| set_beeper | {"on": 0|1, "duration_ms": 3000} | 开关蜂鸣器；on=1 时可选时长，默认 3000ms 后自动关 |
| set_interval | {"seconds": 5} | 修改数据上报周期（1~600 秒） |
| set_alarm_threshold | {"value": -20} | 修改本机报警阈值（℃） |

> **长度约定**：`cmd_id` 最长 64 字符（华为云 IoTDA 的 request_id 为 36 字符 UUID，演示环境为 "req_" + UUID 共 40 字符），设备端回 ack 时**必须原样带回**，网关按完整字符串匹配待回执表。

设备收到 cmd 后必须回 ack（即使失败），cmd_id 原样带回。

### 4.2 状态查询 query

```json
{"type":"query","cmd_id":"req_abc","query":"status"}
```

设备收到后立即回一帧 data（含最新状态），并回 ack。

## 4.3 虚拟数据下发 virt_data（云端 → 终端）

> 新架构（gec6818 作终端/报警器）：Ubuntu 转发程序（virtual-server/virtual_bridge.js）接收华为云虚拟数据后，
> 经本帧下发给终端显示与报警判定。**本帧及 §4.4 confirm 仅由 virtual-server 与 device/ui 终端实现，C/Node 网关不支持。**

```json
{"type":"virt_data","ts":"2026-08-18T12:00:00+08:00","outdoor":32.5,"alarm":0,"alarm_threshold":-20,
 "z1_t":-25.1,"z1_h":60,"z1_s":-25,"z1_st":0,
 "z2_t":3.2,"z2_h":62,"z2_s":2,"z2_st":0,
 "z3_t":10.5,"z3_h":65,"z3_s":10,"z3_st":0,
 "z4_t":15.2,"z4_h":66,"z4_s":15,"z4_st":0}
```

| 字段 | 说明 |
|---|---|
| z1~z4_t / _h / _s / _st | 冷冻/冷藏/预冷/穿堂：温度℃、湿度%、设定值℃、状态（0正常 1预警 2报警） |
| outdoor / alarm / alarm_threshold | 室外温度、报警标志（0/1）、报警阈值 |

终端行为：更新显示 → 本地判定（alarm=1 或任一 status=2）→ 蜂鸣器(/dev/beep)响 + LED 亮 → 等待确认。

### 4.4 报警确认 confirm（终端 → 转发程序）

巡查人员到场后（点"确认报警"按钮或云端 confirm_alarm 命令）：
```json
{"type":"confirm","node_id":"cold01","ts":"2026-08-18T12:01:00+08:00"}
```
终端停蜂鸣/灭 LED 并上报本帧；转发程序记录并停止报警状态。

## 5. 异常约定

- 帧不是合法 JSON、缺 type、node_id 不匹配 → 网关忽略并打日志，不回帧。
- 设备 60s 无任何上行 → 网关标记离线（心跳机制由 data 帧承担；interval 最大 600s 时靠此兜底）。
- 网关收到云端指令但目标节点不在线 → 网关直接向云端回 result_code=1（failed），detail 说明节点离线。