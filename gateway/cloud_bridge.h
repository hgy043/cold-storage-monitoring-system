#ifndef GATEWAY_CLOUD_BRIDGE_H
#define GATEWAY_CLOUD_BRIDGE_H

/* 网关配置（对应 ClientConf.json） */
typedef struct GatewayConfig {
    char deviceId[128];        /* 华为云设备 ID（父设备/网关） */
    char secret[128];          /* 密钥 */
    char serverUri[256];       /* MQTT 接入地址，如 ssl://host:8883 */
    int  tcpPort;              /* 设备 TCP 接入端口，默认 18830 */
    int  reportIntervalMs;     /* 属性批量上报周期，默认 2000 */
    char subDeviceIdPrefix[64];/* 子设备 device_id 前缀，可为空 */
    int  defaultAlarmThreshold;/* 默认报警阈值，默认 -20 */
} GatewayConfig;

/* 读取并解析 ClientConf.json，成功返回 0，失败返回 -1 */
int  cloud_bridge_load_config(GatewayConfig *cfg, const char *path);

/* IOTA_Init + 配置 + 回调注册；workPath 为 SDK 根目录（含 conf/rootcert.pem） */
int  cloud_bridge_init(const GatewayConfig *cfg, const char *workPath);

/* IOTA_Connect（首次连接与断线重连均调用） */
int  cloud_bridge_connect(void);

/* 启动属性批量上报线程（每 reportIntervalMs 一轮） */
void cloud_bridge_start_report_thread(void);

/* 停止（预留） */
void cloud_bridge_stop(void);

/* 设备 ack 命令后由 TCP 分发层回调，向云端回命令响应 */
void cloud_bridge_on_device_ack(const char *node_id, const char *cmd_id,
                                const char *result, const char *detail);

#endif /* GATEWAY_CLOUD_BRIDGE_H */
