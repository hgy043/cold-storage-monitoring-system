/*
 * device_table.c —— 在线设备表
 *
 * node_id -> {fd, name, lastSeen, lastData, ...}
 * 使用 pthread_mutex 保护链表，60s 无上行判定离线。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>

#include "device_table.h"
#include "tcp_server.h"
#include "LogUtil.h"

typedef struct DeviceNode {
    char    node_id[64];
    char    name[128];
    char    model[64];
    int     interval;   /* 数据上报周期秒（register 帧），离线判定用 */
    int     fd;
    time_t  lastSeen;
    double  temperature;
    double  humidity;
    int     led;
    int     beeper;
    int     alarm;
    int     alarm_threshold;
    char    last_ts[64];
    int     has_data;
    int     online;
    struct DeviceNode *next;
} DeviceNode;

static DeviceNode     *g_head = NULL;
static pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;
static int             g_default_alarm_threshold = -20;

/* 初始化设备表（设置默认报警阈值） */
void device_table_init(int default_alarm_threshold)
{
    g_default_alarm_threshold = default_alarm_threshold;
}

/* 必须在持锁状态下调用 */
static DeviceNode *find_locked(const char *node_id)
{
    DeviceNode *n;
    for (n = g_head; n != NULL; n = n->next) {
        if (strcmp(n->node_id, node_id) == 0) {
            return n;
        }
    }
    return NULL;
}

/* 按 node_id 查连接 fd（未注册/离线返回 -1） */
int device_table_get_fd(const char *node_id)
{
    int fd = -1;
    if (node_id == NULL) {
        return -1;
    }
    pthread_mutex_lock(&g_mutex);
    {
        DeviceNode *n = find_locked(node_id);
        if (n != NULL && n->online) {
            fd = n->fd;
        }
    }
    pthread_mutex_unlock(&g_mutex);
    return fd;
}

/* 注册节点（处理 register 帧）：不存在则新建，重复注册提示 */
void device_table_register(const char *node_id, const char *name,
                           const char *model, int interval, int fd)
{
    if (node_id == NULL) {
        return;
    }
    pthread_mutex_lock(&g_mutex);
    {
        DeviceNode *n = find_locked(node_id);
        if (n == NULL) {
            n = (DeviceNode *)calloc(1, sizeof(*n));
            if (n == NULL) {
                pthread_mutex_unlock(&g_mutex);
                return;
            }
            strncpy(n->node_id, node_id, sizeof(n->node_id) - 1);
            n->alarm_threshold = g_default_alarm_threshold;
            n->next = g_head;
            g_head = n;
        }
        if (name != NULL) {
            strncpy(n->name, name, sizeof(n->name) - 1);
        }
        if (model != NULL) {
            strncpy(n->model, model, sizeof(n->model) - 1);
        }
        if (interval >= 1 && interval <= 600) {
            n->interval = interval;
        }
        n->fd = fd;
        n->online = 1;
        n->lastSeen = time(NULL);
    }
    pthread_mutex_unlock(&g_mutex);
}

/* 持设备表锁发送：断线移除（unregister）与 fd 复用注册都需等待发送完成，
 * 从根本上避免"取到 fd 后连接关闭、fd 被新连接复用导致命令错发"。 */
int device_table_send(const char *node_id, const void *buf, size_t len)
{
    int ret = -1;
    if (node_id == NULL || buf == NULL) {
        return -1;
    }
    pthread_mutex_lock(&g_mutex);
    {
        DeviceNode *n = find_locked(node_id);
        if (n != NULL && n->online) {
            ret = tcp_server_send(n->fd, (const char *)buf, len);
        }
    }
    pthread_mutex_unlock(&g_mutex);
    return ret;
}

/* 更新节点最新数据（温度/湿度/报警位），同时维护在线时间 */
void device_table_update_data(const char *node_id, int fd, const Frame *frame)
{
    if (node_id == NULL || frame == NULL) {
        return;
    }
    pthread_mutex_lock(&g_mutex);
    {
        DeviceNode *n = find_locked(node_id);
        if (n == NULL) {
            n = (DeviceNode *)calloc(1, sizeof(*n));
            if (n == NULL) {
                pthread_mutex_unlock(&g_mutex);
                return;
            }
            strncpy(n->node_id, node_id, sizeof(n->node_id) - 1);
            n->alarm_threshold = g_default_alarm_threshold;
            n->next = g_head;
            g_head = n;
        }
        n->fd = fd;
        n->online = 1;
        n->lastSeen = time(NULL);
        n->temperature = frame->temperature;
        n->humidity = frame->humidity;
        n->led = frame->led;
        n->beeper = frame->beeper;
        n->alarm = frame->alarm;
        strncpy(n->last_ts, frame->ts, sizeof(n->last_ts) - 1);
        n->has_data = 1;
    }
    pthread_mutex_unlock(&g_mutex);
}

/* 设置节点报警阈值（云端属性设置时调用） */
void device_table_set_alarm_threshold(const char *node_id, int value)
{
    if (node_id == NULL) {
        return;
    }
    pthread_mutex_lock(&g_mutex);
    {
        DeviceNode *n = find_locked(node_id);
        if (n != NULL) {
            n->alarm_threshold = value;
        }
    }
    pthread_mutex_unlock(&g_mutex);
}

/* 按 fd 注销节点（断线时先注销后 close，防 fd 复用错发） */
void device_table_unregister_by_fd(int fd)
{
    pthread_mutex_lock(&g_mutex);
    {
        DeviceNode **pp = &g_head;
        while (*pp != NULL) {
            if ((*pp)->fd == fd) {
                DeviceNode *tmp = *pp;
                *pp = tmp->next;
                free(tmp);
            } else {
                pp = &(*pp)->next;
            }
        }
    }
    pthread_mutex_unlock(&g_mutex);
}

/* 清理超时离线节点（按注册周期计算超时），返回移除数 */
int device_table_remove_offline(int timeout_sec)
{
    int removed = 0;
    time_t now = time(NULL);
    pthread_mutex_lock(&g_mutex);
    {
        DeviceNode **pp = &g_head;
        while (*pp != NULL) {
            DeviceNode *n = *pp;
            /* 按设备上报周期计算超时：max(全局下限, 周期×3 + 30s)，
             * 兼容协议允许的最大上报周期 600s（不能固定 60s 判离线） */
            long per_dev = (long)timeout_sec;
            if (n->interval > 0) {
                long want = (long)n->interval * 3 + 30;
                if (want > per_dev) {
                    per_dev = want;
                }
            }
            if (now - n->lastSeen > per_dev) {
                *pp = n->next;
                printfLog(EN_LOG_LEVEL_INFO,
                          "device_table: device %s offline (timeout %lds)\n",
                          n->node_id, per_dev);
                free(n);
                removed++;
            } else {
                pp = &n->next;
            }
        }
    }
    pthread_mutex_unlock(&g_mutex);
    return removed;
}

/* 导出全表快照（供上报线程批量上报） */
int device_table_snapshot(DeviceSnapshot *out, int max)
{
    int count = 0;
    if (out == NULL || max <= 0) {
        return 0;
    }
    pthread_mutex_lock(&g_mutex);
    {
        DeviceNode *n;
        for (n = g_head; n != NULL && count < max; n = n->next) {
            strncpy(out[count].node_id, n->node_id, sizeof(out[count].node_id) - 1);
            strncpy(out[count].name, n->name, sizeof(out[count].name) - 1);
            strncpy(out[count].model, n->model, sizeof(out[count].model) - 1);
            out[count].interval = n->interval;
            out[count].fd = n->fd;
            out[count].temperature = n->temperature;
            out[count].humidity = n->humidity;
            out[count].led = n->led;
            out[count].beeper = n->beeper;
            out[count].alarm = n->alarm;
            out[count].alarm_threshold = n->alarm_threshold;
            strncpy(out[count].last_ts, n->last_ts, sizeof(out[count].last_ts) - 1);
            out[count].has_data = n->has_data;
            out[count].online = n->online;
            out[count].lastSeen = n->lastSeen;
            count++;
        }
    }
    pthread_mutex_unlock(&g_mutex);
    return count;
}

/* 返回已注册节点数 */
int device_table_count(void)
{
    int count = 0;
    pthread_mutex_lock(&g_mutex);
    {
        DeviceNode *n;
        for (n = g_head; n != NULL; n = n->next) {
            count++;
        }
    }
    pthread_mutex_unlock(&g_mutex);
    return count;
}
