# 华为云IoT冷库数据监控系统

> 端-边-云-网-用 四层架构的物联网综合教学项目。依据《项目介绍及流程框架.md》与华为云 IoT AgentLite SDK 落地。

## 系统架构

```
┌─────────────┐   TCP:18830   ┌──────────────────┐  MQTT($oc)  ┌──────────────────────┐
│ 感知执行层    │ ───────────▶ │ 边缘网关层         │ ─────────▶ │ 云平台层              │
│ Linux 板 ×32  │   JSON 帧    │ C网关(华为SDK)     │            │ 华为云IoTDA           │
│ 温湿度/LED/  │              │ 或 Node网关(演示)  │ ◀───────── │ 或 web/server.js 内嵌  │
│ 蜂鸣器        │ ◀─────────── │                  │  命令/属性   │ aedes broker           │
└─────────────┘               └──────────────────┘            └──────────┬───────────┘
                                                              REST/WS    │
                                                              ┌─────────▼───────────┐
                                                              │ 应用展示层            │
                                                              │ H5 大屏(web/public/) │
                                                              └─────────────────────┘
```

> 注：上图是"多设备 → 网关 → 云 → 大屏"的通用架构。**使用精简链路**：
> Ubuntu 上的 `virtual-server/` 转发程序充当"虚拟数据源 + 转发器"（每 3s 生成温区数据上报华为云设备 yihao，
> 同时经 TCP 推送给 gec6818 终端）， ARM Cortex-A53 Linux 板 是"终端 + 报警器"（显示 + 蜂鸣/LED + 确认报警），
> web 大屏只展示 1 号冷库。详见下文"当前验收链路"。

## 目录

| 目录 | 内容 |
|---|---|
| `docs/` | 架构、TCP 协议、MQTT 对接、华为云配置、部署联调、阶段验收方案、部署实操、开发与质量记录、部署排障记录（01-05、07-10，06 号未使用） |
| `device/` | gec6818 设备端 C 程序（真实板 / PC 模拟双模式，历史网关模式） |
| `device/ui/` | ** ARM Cortex-A53 Linux 板 板端 LVGL 界面（800×480，5 页触摸 SCADA）**：主菜单/实时监测/温区总览/设备控制/能耗与设置 |
| `gateway/` | Ubuntu 边缘网关 C 程序（基于华为 AgentLite SDK，历史网关模式） |
| `node-gateway/` | Node.js 跨平台网关（历史网关模式演示/协议教学，协议与 C 网关一致） |
| `web/` | Node 后端（内嵌 MQTT broker + REST + WebSocket）+ H5 大屏 |
| `tools/` | 模拟设备（16 冷库 × 前后，历史网关模式）、TCP 调试工具 |
| `virtual-server/` | **Ubuntu 虚拟数据转发程序（当前验收核心）**：每 3s 数据/每 30s 报警，上报华为云 yihao + TCP 推 gec6818 |
| `scripts/` | 一键启动脚本（start-all.bat/sh）、自动验证（verify-demo.js / verify-fixes.js，历史网关模式） |

## 当前验收链路（阶段验收，任务改动.txt）

```
华为云 IoTDA (设备 yihao)  ← MQTT上报 ← Ubuntu virtual-server（每3s虚拟数据/每30s报警）
        │ 数据/命令                        │ TCP virt_data 帧 (18830)
        └─────────────────── 转发 ─────────┤
                                           ▼
                              ARM Cortex-A53 Linux 板 终端（LVGL界面 + /dev/beep 蜂鸣 + LED）
                                ├─ 显示 4 温区温度/曲线/状态（实时监测/温区总览/设备控制/能耗与设置）
                                ├─ 报警 → 蜂鸣器响 + LED 亮 → 巡查触摸"取消报警" → 停蜂鸣并上报 confirm
                                └─ 云端 confirm_alarm / set_beeper / set_led 命令亦可控制
```

** ARM Cortex-A53 Linux 板 运行**：`./coldstorage_ui <转发程序IP> 1号冷库 NO.001 -20`（终端整合版，含 TCP 接收/蜂鸣/确认）。
**Ubuntu 运行**：`cd virtual-server && npm install && cp conf.example.json conf.json && node virtual_bridge.js`（填入华为云凭证；`enabled: false` 走本地 broker 演示）。
**web 大屏**：`cd web && npm install && node server.js` → http://127.0.0.1:8080（展示 1 号冷库）。

> ⚠️ 已知限制：当前 `web/server.js` 只订阅**本地** aedes broker（127.0.0.1:1883），而 `virtual_bridge.js` 只上报**华为云**，
> 两者之间尚无自动数据通路。验收时若大屏需要显示云端数据，需二选一：
> ① 给 `web/server.js` 增加华为云 mqtts 订阅（按 docs/03 主题）；② 或让 `virtual_bridge.js` 在 enabled=true 时同时转发一份到本地 broker。
> （当前未实现，属待办项。）

## 安全说明（配置文件）

华为云 IoTDA 真实凭证（deviceId/secret/接入地址）**不入库**，三个真实配置已在 `.gitignore` 排除：

| 真实配置（本地填写，不入库） | 模板（入库） |
|---|---|
| `gateway/ClientConf.json` | `gateway/ClientConf.example.json` |
| `node-gateway/conf.json` | `node-gateway/conf.example.json` |
| `virtual-server/conf.json` | `virtual-server/conf.example.json` |

- 克隆后复制 `*.example.json` 为实际配置并填入自己的设备凭证。
- `virtual-server` 模板默认 `enabled: false`（走本地 broker 演示，可与 web 大屏直连）；接真实云时改 `enabled: true` 并填 host/deviceId/secret/clientId。
- web 大屏网关标识默认 `demo_gateway_01`，真实部署用环境变量 `GATEWAY_ID` 或 `web/config.json` 注入。

## 快速开始（历史网关演示，无需华为云账号，备选）

```bash
# 1. 安装依赖
cd web && npm install
cd ../node-gateway && npm install

# 2. 启动（三个终端）
node web/server.js               # Web 大屏 http://127.0.0.1:8080
node node-gateway/gateway.js     # Node 网关
node tools/simulate_devices.js   # 模拟 32 个设备节点
```

Windows 可一键：`scripts\start-all.bat`。详见 `docs/05-部署与联调.md`。

## 真实部署（历史网关模式）

Ubuntu 网关（C + 华为 SDK）与 gec6818 设备端见 `docs/04-华为云平台配置指南.md`、`docs/05-部署与联调.md`。
gec6818 板端屏幕界面（LVGL）：`device/ui/`（`cd device/ui && make LVGL_ROOT=/home/gec/lvgl9`，依赖 `lvgl_linux_framebuffer-9` 移植工程）。

## 核心契约

- 设备 ↔ 网关：`docs/02-设备-网关TCP协议.md`（含 4.3 virt_data / 4.4 confirm 新架构帧）
- 网关 ↔ 云：`docs/03-网关-云MQTT对接.md`

## 开发与质量

- 架构决策、设计约束（改代码前必读）、缺陷修复记录：`docs/09-开发与质量记录.md`
- 真实部署排障实录（12 项）：`docs/10-部署排障记录.md`
- 端到端自动验证（历史网关模式）：`node scripts/verify-demo.js`（8 项）、`node scripts/verify-fixes.js`（2 项）
