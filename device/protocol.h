/*
 * protocol.h - 设备端 TCP 帧协议（契约见 docs/02-设备-网关TCP协议.md）
 *
 * 传输层约定：一帧一行 UTF-8 JSON，以单个 \n 结尾；单帧最大 4096 字节。
 */
#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stddef.h>

/* 单帧最大字节数（含末尾 \n），超长丢弃并告警 */
#define PROTO_FRAME_MAX 4096

/* 下行帧类型 */
typedef enum {
    FRAME_NONE = 0,
    FRAME_CMD,      /* 命令下发 */
    FRAME_QUERY,    /* 状态查询 */
    FRAME_OTHER     /* register_ack 等其他帧 */
} frame_type_t;

/* 解析后的下行帧 */
typedef struct {
    frame_type_t type;
    char type_str[24];
    char cmd_id[64];    /* 命令/查询 id（云端 request_id 最长 40 字符，见 docs/02 §4.1），回 ack 时原样带回 */
    char command[32];   /* cmd: set_led / set_beeper / set_interval / set_alarm_threshold */
    char query[16];     /* query: status */
    /* paras 解析结果 */
    int has_on;
    int on;
    int has_duration_ms;
    int duration_ms;
    int has_seconds;
    int seconds;
    int has_value;
    double value;
} proto_frame_t;

/* 上行帧构造：返回整帧字节数（含末尾 \n），失败返回 -1 */
int proto_build_register(char *buf, size_t len,
                         const char *node_id, const char *name,
                         int interval, const char *model);

int proto_build_data(char *buf, size_t len,
                     const char *node_id, const char *ts,
                     double temperature, double humidity,
                     int led, int beeper, int alarm);

int proto_build_ack(char *buf, size_t len,
                    const char *node_id, const char *cmd_id,
                    const char *result, const char *detail);

/* 解析一帧（不含末尾 \n）；缺 type 等非法帧返回 -1 */
int proto_parse_frame(const char *frame, size_t len, proto_frame_t *out);

#endif /* PROTOCOL_H */
