/*
 * cloud_bridge.c —— 华为云 AgentLite SDK 封装
 *
 * - 读取 ClientConf.json 配置
 * - IOTA_Init / 配置 / 回调注册 / 连接 / 断线重连（参考 AgentLiteDemo）
 * - 子设备属性批量上报（IOTA_BatchPropertiesReport，service_id="coldstorage"）
 * - 云端命令下发 -> TCP 下行 cmd 帧 -> 设备 ack -> IOTA_CommandResponse
 * - 属性设置 -> set_alarm_threshold 命令 + IOTA_PropertiesSetResponse
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <stdarg.h>

#include "hw_type.h"
#include "iota_init.h"
#include "iota_cfg.h"
#include "iota_login.h"
#include "iota_datatrans.h"

#include "LogUtil.h"
#include "JsonUtil.h"
#include "StringUtil.h"

#include "cloud_bridge.h"
#include "device_table.h"
#include "protocol.h"
#include "tcp_server.h"

#define MAX_DEVICES   64   /* 快照设备数上限 */
#define REPORT_BATCH  16   /* 每次批量上报的设备数上限 */

/* ------------------------- 全局状态 ------------------------- */
static GatewayConfig g_cfg;
static volatile int  g_cloud_connected = 0;
static volatile int  g_stop = 0;

/* 待回执命令表：requestId -> command_name / node_id */
typedef struct PendingCmd {
    char requestId[64];
    char command_name[64];
    char node_id[64];
    time_t ts;                  /* 登记时间，超时清理用 */
    struct PendingCmd *next;
} PendingCmd;

#define PENDING_TIMEOUT_S  30  /* 设备 ack 超时（秒），超时回 result_code=1 并移除 */

static PendingCmd      *g_pending = NULL;
static pthread_mutex_t  g_pending_mutex = PTHREAD_MUTEX_INITIALIZER;

/* ------------------------- 配置解析 ------------------------- */
int cloud_bridge_load_config(GatewayConfig *cfg, const char *path)
{
    FILE *f;
    long  len;
    char *content;
    JSON *json;

    if (cfg == NULL || path == NULL) {
        return -1;
    }
    memset(cfg, 0, sizeof(*cfg));

    f = fopen(path, "rb");
    if (f == NULL) {
        printfLog(EN_LOG_LEVEL_ERROR, "cloud_bridge: open config failed: %s\n",
                  path);
        return -1;
    }
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    fseek(f, 0, SEEK_SET);
    content = (char *)malloc((size_t)len + 1);
    if (content == NULL) {
        fclose(f);
        return -1;
    }
    if (fread(content, 1, (size_t)len, f) != (size_t)len) {
        fclose(f);
        free(content);
        return -1;
    }
    content[len] = '\0';
    fclose(f);

    json = JSON_Parse(content);
    free(content);
    if (json == NULL) {
        printfLog(EN_LOG_LEVEL_ERROR, "cloud_bridge: parse config failed\n");
        return -1;
    }

    {
        const char *deviceId = JSON_GetStringFromObject(json, "deviceId", "");
        const char *secret   = JSON_GetStringFromObject(json, "secret", "");
        const char *serverUri = JSON_GetStringFromObject(json, "serverUri", "");
        const char *prefix   = JSON_GetStringFromObject(json, "subDeviceIdPrefix", "");

        if (deviceId != NULL) {
            strncpy(cfg->deviceId, deviceId, sizeof(cfg->deviceId) - 1);
        }
        if (secret != NULL) {
            strncpy(cfg->secret, secret, sizeof(cfg->secret) - 1);
        }
        if (serverUri != NULL) {
            strncpy(cfg->serverUri, serverUri, sizeof(cfg->serverUri) - 1);
        }
        if (prefix != NULL) {
            strncpy(cfg->subDeviceIdPrefix, prefix, sizeof(cfg->subDeviceIdPrefix) - 1);
        }

        cfg->tcpPort = JSON_GetIntFromObject(json, "tcpPort", 18830);
        cfg->reportIntervalMs = JSON_GetIntFromObject(json, "reportIntervalMs", 2000);
        cfg->defaultAlarmThreshold = JSON_GetIntFromObject(json, "defaultAlarmThreshold", -20);
    }

    JSON_Delete(json);
    return 0;
}

/* 解析 serverUri：去掉协议头，拆分 host 与 port */
static void parse_server_uri(const char *uri, char *host, size_t hostlen, int *port)
{
    const char *p = uri;
    const char *colon;

    *port = 8883;

    if (strncmp(p, "ssl://", 6) == 0) {
        p += 6;
    } else if (strncmp(p, "tcp://", 6) == 0) {
        p += 6;
        *port = 1883;
    } else if (strncmp(p, "mqtt://", 7) == 0) {
        p += 7;
        *port = 1883;
    }

    colon = strrchr(p, ':');
    if (colon != NULL) {
        *port = atoi(colon + 1);
        if ((size_t)(colon - p) < hostlen) {
            memcpy(host, p, (size_t)(colon - p));
            host[colon - p] = '\0';
        } else {
            strncpy(host, p, hostlen - 1);
            host[hostlen - 1] = '\0';
        }
    } else {
        strncpy(host, p, hostlen - 1);
        host[hostlen - 1] = '\0';
    }
}

/* 从 object_device_id 剥离子设备前缀得到 node_id */
static void strip_prefix(const char *device_id, char *out, size_t outlen)
{
    const char *p = device_id != NULL ? device_id : "";
    size_t prefix_len = strlen(g_cfg.subDeviceIdPrefix);

    if (prefix_len > 0 && strncmp(p, g_cfg.subDeviceIdPrefix, prefix_len) == 0) {
        p += prefix_len;
    }
    strncpy(out, p, outlen - 1);
    out[outlen - 1] = '\0';
}

/* ------------------------- 待回执命令表 ------------------------- */
static void pending_add(const char *requestId, const char *command_name,
                        const char *node_id)
{
    PendingCmd *p = (PendingCmd *)calloc(1, sizeof(*p));
    if (p == NULL) {
        return;
    }
    strncpy(p->requestId, requestId, sizeof(p->requestId) - 1);
    strncpy(p->command_name, command_name, sizeof(p->command_name) - 1);
    strncpy(p->node_id, node_id, sizeof(p->node_id) - 1);
    p->ts = time(NULL);
    pthread_mutex_lock(&g_pending_mutex);
    p->next = g_pending;
    g_pending = p;
    pthread_mutex_unlock(&g_pending_mutex);
}

/* 安全拷贝：总是以 '\0' 结尾，避免 strncpy 截断不终止的告警 */
static void safe_copy(char *dst, size_t dstsz, const char *src)
{
    if (dst == NULL || dstsz == 0) {
        return;
    }
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }
    strncpy(dst, src, dstsz - 1);
    dst[dstsz - 1] = '\0';
}

/* 按 request_id 取并移除 pending 命令（命令闭环匹配） */
static int pending_take(const char *requestId,
                        char *command_name, size_t command_sz,
                        char *node_id, size_t node_sz)
{
    int found = 0;
    pthread_mutex_lock(&g_pending_mutex);
    {
        PendingCmd **pp = &g_pending;
        while (*pp != NULL) {
            if (strcmp((*pp)->requestId, requestId) == 0) {
                PendingCmd *tmp = *pp;
                *pp = tmp->next;
                safe_copy(command_name, command_sz, tmp->command_name);
                safe_copy(node_id, node_sz, tmp->node_id);
                free(tmp);
                found = 1;
                break;
            }
            pp = &(*pp)->next;
        }
    }
    pthread_mutex_unlock(&g_pending_mutex);
    return found;
}

/* 清理超时未回执的命令：向云端回 result_code=1（避免云端永久挂起），并释放内存 */
static void pending_expire(void)
{
    time_t now = time(NULL);
    pthread_mutex_lock(&g_pending_mutex);
    {
        PendingCmd **pp = &g_pending;
        while (*pp != NULL) {
            PendingCmd *p = *pp;
            if (now - p->ts > PENDING_TIMEOUT_S) {
                *pp = p->next;
                printfLog(EN_LOG_LEVEL_WARNING,
                          "cloud_bridge: command %s to %s ack timeout, respond failed\n",
                          p->command_name, p->node_id);
                {
                    char resp[128];
                    snprintf(resp, sizeof(resp),
                             "{\"result\":\"failed\",\"detail\":\"ack timeout\"}");
                    IOTA_CommandResponse((HW_CHAR *)p->requestId, 1,
                                         (HW_CHAR *)p->command_name, resp);
                }
                free(p);
            } else {
                pp = &p->next;
            }
        }
    }
    pthread_mutex_unlock(&g_pending_mutex);
}

/* 把云端命令转发给设备；track=1 时登记待回执（等待设备 ack）。
 * 返回：0 成功；-1 节点不在线；-2 发送失败（由调用方决定响应通道） */
static int forward_command(const char *node_id, const char *command_name,
                           const char *paras_json, const char *requestId,
                           int track)
{
    /* set_alarm_threshold：乐观更新设备报警阈值 */
    if (strcmp(command_name, "set_alarm_threshold") == 0 && paras_json != NULL) {
        JSON *p = JSON_Parse(paras_json);
        if (p != NULL) {
            int v = JSON_GetIntFromObject(p, "value", g_cfg.defaultAlarmThreshold);
            device_table_set_alarm_threshold(node_id, v);
            JSON_Delete(p);
        }
    }

    if (device_table_get_fd(node_id) < 0) {
        /* 目标节点不在线：返回 -1，由调用方按通道回 result_code=1 */
        printfLog(EN_LOG_LEVEL_WARNING,
                  "cloud_bridge: command %s to %s failed: node offline\n",
                  command_name, node_id);
        return -1;
    }

    {
        char frame[2048];
        int n = protocol_build_cmd(requestId, command_name, paras_json,
                                   frame, sizeof(frame));
        if (n <= 0 || device_table_send(node_id, frame, (size_t)n) != 0) {
            printfLog(EN_LOG_LEVEL_WARNING,
                      "cloud_bridge: command %s to %s send failed\n",
                      command_name, node_id);
            return -2;
        }
        if (track) {
            pending_add(requestId, command_name, node_id);
        }
        printfLog(EN_LOG_LEVEL_INFO,
                  "cloud_bridge: command %s forwarded to %s (requestId=%s)\n",
                  command_name, node_id, requestId);
        return 0;
    }
}

/* ------------------------- 连接回调 ------------------------- */
static void handleAuthSuccess(void *context, int messageId, int code, char *message)
{
    (void)context; (void)messageId; (void)code; (void)message;
    g_cloud_connected = 1;
    printfLog(EN_LOG_LEVEL_INFO, "cloud_bridge: connect success\n");
}

/* 断线重连：重建 AgentLite 连接并重新登录/订阅（回调线程内调用） */
static void do_reconnect(const char *why)
{
    g_cloud_connected = 0;
    printfLog(EN_LOG_LEVEL_WARNING, "cloud_bridge: %s, reconnect in 2s...\n", why);
    sleep(2);
    if (IOTA_Connect() != 0) {
        printfLog(EN_LOG_LEVEL_ERROR, "cloud_bridge: reconnect failed\n");
    }
}

/* SDK 鉴权失败回调：打印错误（检查 deviceId/secret/serverUri） */
static void handleAuthFailure(void *context, int messageId, int code, char *message)
{
    (void)context; (void)messageId;
    printfLog(EN_LOG_LEVEL_ERROR, "cloud_bridge: auth failure code=%d msg=%s\n",
              code, message);
    do_reconnect("auth failure");
}

/* SDK 连接断开回调：置断线标志并触发重连 */
static void handleConnectionLost(void *context, int messageId, int code, char *message)
{
    (void)context; (void)messageId;
    printfLog(EN_LOG_LEVEL_WARNING, "cloud_bridge: connection lost code=%d msg=%s\n",
              code, message);
    do_reconnect("connection lost");
}

/* 主动断开成功回调（停止流程用） */
static void handleDisAuthSuccess(void *context, int messageId, int code, char *message)
{
    (void)context; (void)messageId; (void)code; (void)message;
    g_cloud_connected = 0;
    printfLog(EN_LOG_LEVEL_INFO, "cloud_bridge: disconnect success\n");
}

/* 主动断开失败回调 */
static void handleDisAuthFailure(void *context, int messageId, int code, char *message)
{
    (void)context; (void)messageId;
    printfLog(EN_LOG_LEVEL_WARNING, "cloud_bridge: disconnect failure code=%d msg=%s\n",
              code, message);
    do_reconnect("disconnect failure");
}

/* 主题订阅成功回调 */
static void handleSubscribesuccess(void *context, int messageId, int code, char *message)
{
    (void)context; (void)code; (void)message;
    printfLog(EN_LOG_LEVEL_INFO, "cloud_bridge: subscribe success mid=%d\n",
              messageId);
}

/* 主题订阅失败回调：打印失败主题 */
static void handleSubscribeFailure(void *context, int messageId, int code, char *message)
{
    (void)context;
    printfLog(EN_LOG_LEVEL_WARNING,
              "cloud_bridge: subscribe failure mid=%d code=%d msg=%s\n",
              messageId, code, message);
}

/* 消息发布成功回调 */
static void handlePublishSuccess(void *context, int messageId, int code, char *message)
{
    (void)context; (void)code; (void)message;
    printfLog(EN_LOG_LEVEL_INFO, "cloud_bridge: publish success mid=%d\n",
              messageId);
}

/* 消息发布失败回调：打印 topic/错误码 */
static void handlePublishFailure(void *context, int messageId, int code, char *message)
{
    (void)context;
    printfLog(EN_LOG_LEVEL_WARNING,
              "cloud_bridge: publish failure mid=%d code=%d msg=%s\n",
              messageId, code, message);
}

/* ------------------------- 下行消息/命令回调 ------------------------- */
static void handleMessageDown(void *context, int messageId, int code, char *message)
{
    (void)context; (void)code;
    JSON *root = JSON_Parse(message);
    const char *content = root != NULL ? JSON_GetStringFromObject(root, "content", "") : "";
    printfLog(EN_LOG_LEVEL_INFO, "cloud_bridge: message down mid=%d content=%s\n",
              messageId, content);
    if (root != NULL) {
        JSON_Delete(root);
    }
}

/* PFN_CALLBACK_HANDLER_WITH_TOPIC：带 requestId */
static void handleCommandRequest(void *context, int messageId, int code,
                                 char *message, char *requestId)
{
    (void)context; (void)code;
    JSON *root;
    const char *object_device_id;
    const char *command_name;
    JSON *paras;
    char node_id[64];
    char *paras_json = NULL;

    printfLog(EN_LOG_LEVEL_INFO,
              "cloud_bridge: command request mid=%d requestId=%s msg=%s\n",
              messageId, requestId, message);

    root = JSON_Parse(message);
    if (root == NULL) {
        printfLog(EN_LOG_LEVEL_WARNING, "cloud_bridge: command parse failed\n");
        return;
    }

    object_device_id = JSON_GetStringFromObject(root, "object_device_id", "");
    command_name = JSON_GetStringFromObject(root, "command_name", "");
    paras = JSON_GetObjectFromObject(root, "paras");

    strip_prefix(object_device_id, node_id, sizeof(node_id));
    if (paras != NULL) {
        paras_json = JSON_Print(paras);
    }

    {
        int rc = forward_command(node_id, command_name != NULL ? command_name : "",
                                 paras_json != NULL ? paras_json : "{}",
                                 requestId != NULL ? requestId : "", 1);
        if (rc != 0) {
            /* 节点离线或发送失败：按命令通道回 result_code=1 */
            char resp[256];
            snprintf(resp, sizeof(resp),
                     "{\"result\":\"failed\",\"detail\":\"%s\"}",
                     rc == -1 ? "node offline" : "send failed");
            IOTA_CommandResponse((HW_CHAR *)(requestId != NULL ? requestId : ""), 1,
                                 (HW_CHAR *)command_name, resp);
        }
    }

    if (paras_json != NULL) {
        free(paras_json);
    }
    JSON_Delete(root);
}

/* 云端属性设置下行：写阈值等可写属性 → 转发设备 */
static void handlePropertiesSet(void *context, int messageId, int code,
                                char *message, char *requestId)
{
    (void)context; (void)code;
    JSON *root;
    const char *object_device_id;
    JSON *services;
    int threshold = g_cfg.defaultAlarmThreshold;
    char node_id[64];
    char paras_json[64];

    printfLog(EN_LOG_LEVEL_INFO,
              "cloud_bridge: properties set mid=%d requestId=%s msg=%s\n",
              messageId, requestId, message);

    root = JSON_Parse(message);
    if (root == NULL) {
        printfLog(EN_LOG_LEVEL_WARNING, "cloud_bridge: propset parse failed\n");
        return;
    }

    object_device_id = JSON_GetStringFromObject(root, "object_device_id", "");
    services = JSON_GetObjectFromObject(root, "services");
    if (services != NULL && JSON_GetArraySize(services) > 0) {
        JSON *service = JSON_GetObjectFromArray(services, 0);
        JSON *properties = service != NULL
                         ? JSON_GetObjectFromObject(service, "properties") : NULL;
        if (properties != NULL) {
            threshold = JSON_GetIntFromObject(properties, "alarm_threshold",
                                              g_cfg.defaultAlarmThreshold);
        }
    }

    strip_prefix(object_device_id, node_id, sizeof(node_id));
    snprintf(paras_json, sizeof(paras_json), "{\"value\":%d}", threshold);

    /* 转发为 set_alarm_threshold 命令；属性设置走 IOTA_PropertiesSetResponse
       响应通道（不是 commands/response），故 track=0（不再回 IOTA_CommandResponse） */
    {
        int rc = forward_command(node_id, "set_alarm_threshold", paras_json,
                                 requestId != NULL ? requestId : "", 0);
        if (rc == 0) {
            IOTA_PropertiesSetResponse((HW_CHAR *)requestId, 0, "success");
        } else {
            /* 节点离线/发送失败：在属性设置响应通道回失败，不谎报 success */
            IOTA_PropertiesSetResponse((HW_CHAR *)requestId, 1,
                                       rc == -1 ? "node offline" : "send failed");
            printfLog(EN_LOG_LEVEL_WARNING,
                      "cloud_bridge: propset to %s failed rc=%d\n", node_id, rc);
        }
    }

    JSON_Delete(root);
}

/* 云端属性查询下行：按设备表快照回当前属性 */
static void handlePropertiesGet(void *context, int messageId, int code,
                                char *message, char *requestId)
{
    (void)context; (void)code;
    printfLog(EN_LOG_LEVEL_INFO,
              "cloud_bridge: properties get mid=%d requestId=%s msg=%s\n",
              messageId, requestId, message);
}

/* PFN_CALLBACK_HANDLER：不带 requestId（子设备事件/消息） */
static void handleEventsDown(void *context, int messageId, int code, char *message)
{
    (void)context; (void)code;
    printfLog(EN_LOG_LEVEL_INFO,
              "cloud_bridge: events down mid=%d msg=%s\n", messageId, message);
}

/* ------------------------- 属性批量上报 ------------------------- */
static void report_all(void)
{
    DeviceSnapshot snap[MAX_DEVICES];
    int n;
    int idx = 0;
    char event_time[64];
    const char *ets;

    if (!g_cloud_connected) {
        return;
    }

    n = device_table_snapshot(snap, MAX_DEVICES);
    if (n <= 0) {
        return;
    }

    ets = getEventTimeStamp();
    if (ets == NULL) {
        ets = "";
    }
    strncpy(event_time, ets, sizeof(event_time) - 1);
    event_time[sizeof(event_time) - 1] = '\0';

    while (idx < n) {
        ST_IOTA_DEVICE_DATA_INFO devices[REPORT_BATCH];
        int  serviceList[REPORT_BATCH];
        char devids[REPORT_BATCH][96];
        char props[REPORT_BATCH][256];
        int  cnt = 0;

        memset(devices, 0, sizeof(devices));

        for (; idx < n && cnt < REPORT_BATCH; idx++) {
            DeviceSnapshot *s = &snap[idx];
            if (!s->has_data) {
                continue;   /* 尚无数据的设备跳过 */
            }

            /* device_id = subDeviceIdPrefix + node_id */
            snprintf(devids[cnt], sizeof(devids[cnt]), "%s%s",
                     g_cfg.subDeviceIdPrefix, s->node_id);

            /* properties：temperature/humidity/led/beeper/alarm/alarm_threshold */
            snprintf(props[cnt], sizeof(props[cnt]),
                     "{\"temperature\":%g,\"humidity\":%g,"
                     "\"led\":%d,\"beeper\":%d,\"alarm\":%d,\"alarm_threshold\":%d}",
                     s->temperature, s->humidity,
                     s->led, s->beeper, s->alarm, s->alarm_threshold);

            devices[cnt].device_id = devids[cnt];
            devices[cnt].services[0].service_id = "coldstorage";
            devices[cnt].services[0].event_time = event_time;
            devices[cnt].services[0].properties = props[cnt];
            serviceList[cnt] = 1;
            cnt++;
        }

        if (cnt > 0) {
            int mid = IOTA_BatchPropertiesReport(devices, cnt, serviceList);
            if (mid != 0) {
                printfLog(EN_LOG_LEVEL_ERROR,
                          "cloud_bridge: batch report failed mid=%d\n", mid);
            } else {
                printfLog(EN_LOG_LEVEL_INFO,
                          "cloud_bridge: batch report %d devices\n", cnt);
            }
        }
    }
}

static void *report_thread_main(void *arg)
{
    (void)arg;
    while (!g_stop) {
        int removed = device_table_remove_offline(60);
        if (removed > 0) {
            printfLog(EN_LOG_LEVEL_INFO,
                      "cloud_bridge: removed %d offline devices\n", removed);
        }
        pending_expire();
        report_all();
        usleep((useconds_t)(g_cfg.reportIntervalMs > 0
                            ? g_cfg.reportIntervalMs : 2000) * 1000);
    }
    return NULL;
}

/* ------------------------- 回调注册 ------------------------- */
static void register_callbacks(void)
{
    IOTA_SetCallback(EN_IOTA_CALLBACK_CONNECT_SUCCESS, handleAuthSuccess);
    IOTA_SetCallback(EN_IOTA_CALLBACK_CONNECT_FAILURE, handleAuthFailure);
    IOTA_SetCallback(EN_IOTA_CALLBACK_CONNECTION_LOST, handleConnectionLost);
    IOTA_SetCallback(EN_IOTA_CALLBACK_DISCONNECT_SUCCESS, handleDisAuthSuccess);
    IOTA_SetCallback(EN_IOTA_CALLBACK_DISCONNECT_FAILURE, handleDisAuthFailure);
    IOTA_SetCallback(EN_IOTA_CALLBACK_SUBSCRIBE_SUCCESS, handleSubscribesuccess);
    IOTA_SetCallback(EN_IOTA_CALLBACK_SUBSCRIBE_FAILURE, handleSubscribeFailure);
    IOTA_SetCallback(EN_IOTA_CALLBACK_PUBLISH_SUCCESS, handlePublishSuccess);
    IOTA_SetCallback(EN_IOTA_CALLBACK_PUBLISH_FAILURE, handlePublishFailure);

    IOTA_SetCallback(EN_IOTA_CALLBACK_MESSAGE_DOWN, handleMessageDown);
    IOTA_SetCallbackWithTopic(EN_IOTA_CALLBACK_COMMAND_REQUEST, handleCommandRequest);
    IOTA_SetCallbackWithTopic(EN_IOTA_CALLBACK_PROPERTIES_SET, handlePropertiesSet);
    IOTA_SetCallbackWithTopic(EN_IOTA_CALLBACK_PROPERTIES_GET, handlePropertiesGet);
    IOTA_SetCallback(EN_IOTA_CALLBACK_SUB_DEVICE_MESSAGE_DOWN, handleEventsDown);
}

/* ------------------------- 公开接口 ------------------------- */
int cloud_bridge_init(const GatewayConfig *cfg, const char *workPath)
{
    char host[256];
    int  port = 8883;

    if (cfg == NULL) {
        return -1;
    }
    g_cfg = *cfg;

    parse_server_uri(g_cfg.serverUri, host, sizeof(host), &port);

    /* IOTA_Init 会读取 workPath/conf/rootcert.pem */
    if (IOTA_Init((HW_CHAR *)workPath) != 0) {
        printfLog(EN_LOG_LEVEL_ERROR,
                  "cloud_bridge: IOTA_Init failed, check workPath=%s\n",
                  workPath != NULL ? workPath : "");
        return -1;
    }

    IOTA_ConfigSetStr(EN_IOTA_CFG_MQTT_ADDR, host);
    IOTA_ConfigSetUint(EN_IOTA_CFG_MQTT_PORT, (HW_UINT)port);
    IOTA_ConfigSetStr(EN_IOTA_CFG_DEVICEID, g_cfg.deviceId);
    IOTA_ConfigSetStr(EN_IOTA_CFG_DEVICESECRET, g_cfg.secret);
    IOTA_ConfigSetUint(EN_IOTA_CFG_KEEP_ALIVE_TIME, 60);

    register_callbacks();

    printfLog(EN_LOG_LEVEL_INFO,
              "cloud_bridge: init ok, mqtt=%s:%d deviceId=%s\n",
              host, port, g_cfg.deviceId);
    return 0;
}

/* 建立云端连接：IOTA_Init + IOTA_Connect + 订阅主题，成功返回 0 */
int cloud_bridge_connect(void)
{
    int ret = IOTA_Connect();
    if (ret != 0) {
        printfLog(EN_LOG_LEVEL_ERROR, "cloud_bridge: IOTA_Connect failed ret=%d\n",
                  ret);
    }
    return ret;
}

/* 启动设备表轮询上报线程（每上报周期批量上报一次） */
void cloud_bridge_start_report_thread(void)
{
    pthread_t t;
    if (pthread_create(&t, NULL, report_thread_main, NULL) != 0) {
        printfLog(EN_LOG_LEVEL_ERROR, "cloud_bridge: create report thread failed\n");
        return;
    }
    pthread_detach(t);
}

/* 停止上报线程并断开云端连接 */
void cloud_bridge_stop(void)
{
    g_stop = 1;
}

/* 设备 ack 回执 → 回云端命令响应（result_code 0/1） */
void cloud_bridge_on_device_ack(const char *node_id, const char *cmd_id,
                                const char *result, const char *detail)
{
    char command_name[64] = {0};
    char node[64] = {0};
    int  result_code;
    char resp[512];

    (void)node_id;

    if (!pending_take(cmd_id, command_name, sizeof(command_name),
                      node, sizeof(node))) {
        printfLog(EN_LOG_LEVEL_INFO,
                  "cloud_bridge: ack for untracked cmd_id=%s ignored\n", cmd_id);
        return;
    }

    result_code = (result != NULL && strcmp(result, "success") == 0) ? 0 : 1;
    snprintf(resp, sizeof(resp), "{\"result\":\"%s\",\"detail\":\"%s\"}",
             result != NULL ? result : "",
             detail != NULL ? detail : "");

    if (IOTA_CommandResponse((HW_CHAR *)cmd_id, result_code, command_name, resp) != 0) {
        printfLog(EN_LOG_LEVEL_ERROR, "cloud_bridge: IOTA_CommandResponse failed\n");
    } else {
        printfLog(EN_LOG_LEVEL_INFO,
                  "cloud_bridge: command %s ack result=%s detail=%s\n",
                  command_name, result, detail);
    }
}
