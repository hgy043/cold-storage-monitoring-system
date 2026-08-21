# 03-网关-云 MQTT 对接（华为 IoTDA 主题语义）

> 契约方：`gateway/`（C + AgentLite SDK，内部已封装本协议）、`node-gateway/`（Node 直连实现同一协议）、`web/server.js`（演示 broker + 应用侧订阅/下发）。
> 网关是"父设备（直连）"，冷库节点是"子设备"。所有主题沿用华为云 IoTDA 的 `$oc` 体系。

## 1. 连接

- broker 地址：华为云 IoTDA（TLS 8883）或本地演示 broker（`mqtt://127.0.0.1:1883`，由 `web/server.js` 内嵌的 aedes 提供）。
- 认证：MQTT username = 网关 deviceId，password = secret（华为平台按此鉴权；演示 broker 放行全部）。
- keepalive 60s。

## 2. 上行：子设备属性上报

主题（发布）：

```
$oc/devices/{gatewayId}/sys/gateway/sub-devices/properties/report
```

载荷：

```json
{
  "devices": [
    {
      "device_id": "cold01_front",
      "services": [
        {
          "service_id": "coldstorage",
          "properties": {
            "temperature": -18.5,
            "humidity": 62.3,
            "led": 0,
            "beeper": 0,
            "alarm": 0,
            "alarm_threshold": -20
          },
          "event_time": "2025-01-01T12:00:00+08:00"
        }
      ]
    }
  ]
}
```

- 演示环境 `device_id` 直接用 node_id；上真实平台时 = `{子设备产品ID}_` + node_id（配置 `subDeviceIdPrefix`）。
- 上报节流：网关聚合 TCP 数据，默认每 2 秒批量上报一次（可配置）。

## 3. 下行：命令下发

主题（订阅）：

```
$oc/devices/{gatewayId}/sys/commands/#
```

云端发布到 `$oc/devices/{gatewayId}/sys/commands/request_id={requestId}`，载荷：

```json
{"object_device_id":"cold01_front","command_name":"set_led","service_id":"coldstorage","paras":{"on":1}}
```

命令响应（发布）：

```
$oc/devices/{gatewayId}/sys/commands/response/request_id={requestId}
```

```json
{"result_code":0,"response_name":"set_led","paras":{"result":"success","detail":"LED on"}}
```

`result_code`：0=成功，1=失败。

## 4. 下行：属性设置（用于下发报警阈值等可写属性）

主题（订阅）：

```
$oc/devices/{gatewayId}/sys/properties/set/#
```

载荷：

```json
{"object_device_id":"cold01_front","services":[{"service_id":"coldstorage","properties":{"alarm_threshold":-25}}]}
```

响应（发布）：

```
$oc/devices/{gatewayId}/sys/properties/set/response/request_id={requestId}
```

```json
{"result_code":0,"result_desc":"success"}
```

## 5. 可选：平台消息下发

主题（订阅）：`$oc/devices/{gatewayId}/sys/messages/down`，载荷含 `content` 字段，网关打印日志即可。

## 6. Web 后端（应用侧/演示平台）行为

- 内嵌 aedes MQTT broker，监听 1883。
- 订阅 `$oc/devices/+/sys/gateway/sub-devices/properties/report`、`$oc/devices/+/sys/commands/response/#`、`$oc/devices/+/sys/properties/set/response/#`。
- 收到上报 → 按 `device_id` 落库（内存环形 + `web/data/history.json` 持久化）→ WebSocket 广播。
- 下发命令：发布到 `$oc/devices/{gatewayId}/sys/commands/request_id={uuid}`，并把 `object_device_id/command/requestId` 登记为 pending；收到 response 后广播结果。
- 属性设置同理（properties/set）。

## 7. 本地演示的完整数据链（联调验证用）

```
tools/simulate_devices.js  ──TCP:18830──▶  node-gateway (或 C 网关)
node-gateway              ──MQTT:1883──▶  web/server.js (aedes broker)
web/server.js             ──REST/WS──▶   H5 大屏
H5 按钮 ──REST──▶ web/server.js ──MQTT commands──▶ node-gateway ──TCP──▶ 设备 ──ack──▶ node-gateway ──MQTT response──▶ web/server.js ──WS──▶ H5
```
