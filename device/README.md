# device/ — 冷库数据监控系统 设备端

gec6818 开发板与 PC（x86 Ubuntu）双模式设备端程序。纯 C99 + POSIX（socket/pthreads），
零第三方依赖，可在 ARM Linux 与 x86 Linux 上编译运行。

## 一、文件清单

| 文件 | 职责 |
|---|---|
| Makefile | gcc 编译（-Wall -O2 -pthread），支持 make clean |
| device.conf | 运行配置（网关地址/端口、node_id、上报周期、报警阈值、模拟模式开关等） |
| protocol.h / protocol.c | TCP 帧协议：register/data/ack 上行帧构造 + cmd/query 下行帧解析（手写 JSON，零依赖） |
| sensor.h / sensor.c | 温湿度采集：真实 sysfs 优先，否则模拟（围绕 -18℃ 随机游走 + 偶发尖峰，湿度 55~75） |
| actuator.h / actuator.c | LED 与蜂鸣器：真实模式 sysfs GPIO，模拟模式打印日志并维护内部状态 |
| tcp_client.h / tcp_client.c | TCP 客户端：连接、断线自动重连（1s/2s/4s…上限 30s）、发送、独立接收线程 |
| device.c | 主程序：读配置 → 初始化 → 连网关 → 注册 → 周期上报 → 处理下行命令 |
| README.md | 本文件 |

## 二、编译

### Ubuntu 本机编译

```bash
cd device
make          # 生成可执行文件 device
make clean    # 清理目标文件与可执行文件
```

### gec6818 ARM 交叉编译

```bash
cd device
make clean
make CC=arm-linux-gcc          # 厂商 soft-float 工具链（推荐）
# 产物 device 为 ARM 可执行文件，拷贝到开发板运行
```

> ⚠️ 必须用板子系统配套的**厂商工具链**（soft float）：gec6818 教程常见 `arm-linux-gcc`。
> 勿用 apt 的 `arm-linux-gnueabihf-gcc`（hard float），ABI 不匹配会在板端运行崩溃。
> 验证产物：`file device` 应显示 "ELF 32-bit LSB executable, ARM"；`readelf -A` 检查 float ABI。

## 三、配置说明（device.conf）

键值对格式 `key=value`，`#` 开头为注释，空行忽略，便于解析。

| 键 | 默认值 | 说明 |
|---|---|---|
| gateway_ip | 127.0.0.1 | 网关 TCP 服务端地址 |
| gateway_port | 18830 | 网关端口（协议约定 18830） |
| node_id | cold01_front | 节点标识，规范 cold<NN>_<front/back> |
| name | 1号冷库-前门 | 显示名称 |
| interval | 5 | 数据上报周期（秒，1~600） |
| alarm_threshold | -15 | 本地报警阈值（℃），temperature > 该值则 alarm=1 |
| sim_mode | 1 | 模拟模式开关：1=模拟，0=真实硬件 |
| led_gpio | 17 | 真实模式 LED GPIO 编号（模拟模式忽略，-1 禁用） |
| beeper_gpio | 18 | 真实模式蜂鸣器 GPIO 编号（模拟模式忽略，-1 禁用） |
| sensor_temp_path | /sys/class/hwmon/hwmon0/temp1_input | 真实模式温度 sysfs 文件 |
| sensor_humi_path | /sys/class/hwmon/hwmon0/humidity1_input | 真实模式湿度 sysfs 文件 |

运行时可指定其它配置文件：`./device /path/to/device.conf`（缺省读当前目录 device.conf）。

## 四、协议要点（契约见 docs/02-设备-网关TCP协议.md）

- 传输层：设备为 TCP 客户端，主动连网关 18830 端口；一帧一行 UTF-8 JSON，以单个 `\n` 结尾，单帧最大 4096 字节。
- 上行帧：
  - register：连接建立后第一条消息，必带 type/node_id；sim_mode=1 时 model=gec6818-sim，否则 gec6818。
  - data：带 ts（ISO8601 带时区，如 2025-01-01T12:00:00+08:00）、temperature、humidity、led、beeper、alarm。
  - ack：执行下行命令后回执，cmd_id 原样带回，result=success/failed，detail 为可读说明。
- 下行帧：
  - cmd：set_led（paras.on）、set_beeper（paras.on / duration_ms，默认 3000ms 自动关）、set_interval（paras.seconds，1~600）、set_alarm_threshold（paras.value）。
  - query：立即回一帧 data 并回 ack。
- 健壮性：
  - 接收线程按 `\n` 切帧，内部缓冲容忍半包/粘包；超长帧（>4096）丢弃并告警；非法帧（缺 type）丢弃。
  - 断线自动重连，退避 1s、2s、4s…上限 30s。
  - 本地报警：temperature > alarm_threshold 时 data 帧 alarm=1。

## 五、运行示例

```bash
cd device
./device
# 输出示例：
# [config] gateway=127.0.0.1:18830 node_id=cold01_front interval=5s alarm_threshold=-15.0℃ sim_mode=1
# [sensor] 模拟模式: 围绕 -18.0℃ 随机游走, 湿度 55~75
# [actuator] 模拟模式: LED/蜂鸣器仅打印日志并维护内部状态
# [tcp] 已连接网关 127.0.0.1:18830
# [device] 已发送注册帧 node_id=cold01_front model=gec6818-sim
```

