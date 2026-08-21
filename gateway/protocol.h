#ifndef GATEWAY_PROTOCOL_H
#define GATEWAY_PROTOCOL_H

#include <stddef.h>

/* 单帧最大长度（字节），超长丢弃并告警（见 docs/02-设备-网关TCP协议.md） */
#define PROTO_MAX_FRAME_LEN 4096

/*
 * 上行帧解析结果结构体。
 * 设备上行帧类型：register / data / ack。
 * 字段按需填充，未出现的字段保持默认值。
 */
typedef struct Frame {
    char    type[16];        /* 帧类型：register / data / ack */
    char    node_id[64];     /* 节点标识，如 cold01_front */
    char    name[128];       /* 显示名称（register） */
    int     interval;        /* 数据上报周期秒（register） */
    char    model[64];       /* 设备型号（register） */

    /* data 帧字段 */
    char    ts[64];          /* 采集时间戳 */
    double  temperature;     /* 温度（℃） */
    double  humidity;        /* 相对湿度（%） */
    int     led;             /* LED 状态 0/1 */
    int     beeper;          /* 蜂鸣器状态 0/1 */
    int     alarm;           /* 本机超阈值告警 0/1 */

    /* ack 帧字段 */
    char    cmd_id[64];      /* 回执对应的命令 id（=云端 requestId） */
    char    result[16];      /* success / failed */
    char    detail[256];     /* 人类可读说明 */
} Frame;

/* 解析设备上行帧（JSON），成功返回 0，失败返回 -1 */
int protocol_parse_frame(const char *buf, Frame *out);

/* 构造 register_ack 下行帧，返回写入字节数（含结尾 '\n'），失败返回 -1 */
int protocol_build_register_ack(const char *node_id, char *out, size_t outlen);

/* 构造 cmd 下行帧：{"type":"cmd","cmd_id":..,"command":..,"paras":..}\n */
int protocol_build_cmd(const char *cmd_id, const char *command,
                       const char *paras_json, char *out, size_t outlen);

/* 构造 query 下行帧：{"type":"query","cmd_id":..,"query":..}\n */
int protocol_build_query(const char *cmd_id, const char *query,
                         char *out, size_t outlen);

#endif /* GATEWAY_PROTOCOL_H */
