#ifndef GATEWAY_DEVICE_TABLE_H
#define GATEWAY_DEVICE_TABLE_H

#include <time.h>
#include "protocol.h"

/* 设备表快照（用于上报等只读场景，避免持锁期间调用 SDK） */
typedef struct DeviceSnapshot {
    char    node_id[64];
    char    name[128];
    char    model[64];
    int     interval;   /* 数据上报周期秒（register 帧），用于离线判定 */
    int     fd;
    double  temperature;
    double  humidity;
    int     led;
    int     beeper;
    int     alarm;
    int     alarm_threshold;
    char    last_ts[64];
    int     has_data;      /* 是否已收到过 data 帧 */
    int     online;        /* 是否在线 */
    time_t  lastSeen;
} DeviceSnapshot;

/* 初始化设备表；default_alarm_threshold 为新设备默认报警阈值 */
void device_table_init(int default_alarm_threshold);

/* 查在线节点 fd，不在线返回 -1 */
int  device_table_get_fd(const char *node_id);

/* 注册/更新设备（register 帧）：不存在则创建，存在则刷新 fd/名称/型号/上报周期 */
void device_table_register(const char *node_id, const char *name,
                           const char *model, int interval, int fd);

/* 向节点发送下行帧（持设备表锁发送，避免与断线关闭竞态导致 fd 复用错发）；
 * 返回 0 成功，-1 节点不在线/已关闭或发送失败 */
int  device_table_send(const char *node_id, const void *buf, size_t len);

/* 更新设备最新数据（data 帧），并刷新 lastSeen */
void device_table_update_data(const char *node_id, int fd, const Frame *frame);

/* 更新设备报警阈值（set_alarm_threshold 乐观更新） */
void device_table_set_alarm_threshold(const char *node_id, int value);

/* 按 fd 移除设备（连接断开时调用） */
void device_table_unregister_by_fd(int fd);

/* 移除超时（秒）未上报的设备，返回移除数量 */
int  device_table_remove_offline(int timeout_sec);

/* 拷贝设备表快照，返回实际拷贝数量 */
int  device_table_snapshot(DeviceSnapshot *out, int max);

/* 当前设备数量 */
int  device_table_count(void);

#endif /* GATEWAY_DEVICE_TABLE_H */
