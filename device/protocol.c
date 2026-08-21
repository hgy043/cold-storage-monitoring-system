/*
 * protocol.c - 设备端 TCP 帧协议封装
 *
 * 契约见 docs/02-设备-网关TCP协议.md：
 *   一帧一行 JSON，以单个 \n 结尾；单帧最大 4096 字节。
 *   本文件实现 register/data/ack 上行帧构造，以及 cmd/query 下行帧解析。
 *   为保持设备端零依赖，全部 JSON 均手工拼接/扫描，不引入 cJSON。
 */
#include "protocol.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 把已拼接好的 JSON 追加 \n 与 \0，返回整帧字节数（含 \n），失败返回 -1 */
static int finish(char *buf, size_t len, int n)
{
    /* snprintf 返回值 >= len 说明发生截断；还需预留 1 字节给 \n */
    if (n < 0 || (size_t)n + 1 >= len) {
        return -1;
    }
    buf[n] = '\n';
    buf[n + 1] = '\0';
    return n + 1;
}

/* 构造 register 注册帧（连接建立后首帧） */
int proto_build_register(char *buf, size_t len,
                         const char *node_id, const char *name,
                         int interval, const char *model)
{
    int n = snprintf(buf, len,
        "{\"type\":\"register\",\"node_id\":\"%s\",\"name\":\"%s\","
        "\"interval\":%d,\"model\":\"%s\"}",
        node_id ? node_id : "", name ? name : "", interval, model ? model : "");
    return finish(buf, len, n);
}

/* 构造 data 数据帧（温度/湿度/LED/蜂鸣/报警） */
int proto_build_data(char *buf, size_t len,
                     const char *node_id, const char *ts,
                     double temperature, double humidity,
                     int led, int beeper, int alarm)
{
    int n = snprintf(buf, len,
        "{\"type\":\"data\",\"node_id\":\"%s\",\"ts\":\"%s\","
        "\"temperature\":%.1f,\"humidity\":%.1f,"
        "\"led\":%d,\"beeper\":%d,\"alarm\":%d}",
        node_id ? node_id : "", ts ? ts : "",
        temperature, humidity, led, beeper, alarm);
    return finish(buf, len, n);
}

/* 构造 ack 命令回执帧（cmd_id 原样带回） */
int proto_build_ack(char *buf, size_t len,
                    const char *node_id, const char *cmd_id,
                    const char *result, const char *detail)
{
    int n = snprintf(buf, len,
        "{\"type\":\"ack\",\"node_id\":\"%s\",\"cmd_id\":\"%s\","
        "\"result\":\"%s\",\"detail\":\"%s\"}",
        node_id ? node_id : "", cmd_id ? cmd_id : "",
        result ? result : "", detail ? detail : "");
    return finish(buf, len, n);
}

/* ---- 手写 JSON 扫描辅助函数（零依赖） ---- */

/* 定位 "key" 之后的冒号，返回其后的值起始位置；找不到返回 NULL */
static const char *find_key(const char *json, const char *key)
{
    size_t klen = strlen(key);
    char pat[64];
    const char *p;

    if (klen + 2 >= sizeof(pat)) {
        return NULL;
    }
    snprintf(pat, sizeof(pat), "\"%s\"", key);

    /* 循环查找：跳过「值恰好等于 key 名」造成的误匹配，直到找到后跟冒号的键 */
    p = json;
    while ((p = strstr(p, pat)) != NULL) {
        const char *q = p + strlen(pat);
        while (*q == ' ' || *q == '\t') {
            q++;
        }
        if (*q == ':') {
            q++;
            while (*q == ' ' || *q == '\t') {
                q++;
            }
            return q;
        }
        p += strlen(pat);   /* 未匹配到冒号，继续向后找 */
    }
    return NULL;
}

/* 解析 "key":"字符串" 到 out（不做转义展开，字段值均为安全字符） */
static int get_string(const char *json, const char *key, char *out, size_t outlen)
{
    const char *p = find_key(json, key);
    size_t i = 0;
    if (!p || *p != '"') {
        return -1;
    }
    p++;
    while (*p && *p != '"' && i + 1 < outlen) {
        out[i++] = *p++;
    }
    out[i] = '\0';
    return 0;
}

/* 提取 JSON 整数字段，失败返回 0 */
static int get_int(const char *json, const char *key, int *out)
{
    const char *p = find_key(json, key);
    char *end;
    long v;
    if (!p) {
        return -1;
    }
    v = strtol(p, &end, 10);
    if (end == p) {
        return -1;
    }
    *out = (int)v;
    return 0;
}

/* 提取 JSON 浮点字段，失败返回 0 */
static int get_double(const char *json, const char *key, double *out)
{
    const char *p = find_key(json, key);
    char *end;
    double v;
    if (!p) {
        return -1;
    }
    v = strtod(p, &end);
    if (end == p) {
        return -1;
    }
    *out = v;
    return 0;
}

/* 解析一帧 JSON 文本为 proto_frame_t；非法 JSON / 缺 type 返回 0 */
int proto_parse_frame(const char *frame, size_t len, proto_frame_t *out)
{
    char type[24];

    if (!frame || len == 0 || !out) {
        return -1;
    }
    memset(out, 0, sizeof(*out));
    out->type = FRAME_OTHER;

    /* 缺 type 即视为非法帧 */
    if (get_string(frame, "type", type, sizeof(type)) != 0) {
        return -1;
    }
    snprintf(out->type_str, sizeof(out->type_str), "%s", type);

    if (strcmp(type, "cmd") == 0) {
        out->type = FRAME_CMD;
        get_string(frame, "cmd_id", out->cmd_id, sizeof(out->cmd_id));
        get_string(frame, "command", out->command, sizeof(out->command));
        if (get_int(frame, "on", &out->on) == 0) {
            out->has_on = 1;
        }
        if (get_int(frame, "duration_ms", &out->duration_ms) == 0) {
            out->has_duration_ms = 1;
        }
        if (get_int(frame, "seconds", &out->seconds) == 0) {
            out->has_seconds = 1;
        }
        if (get_double(frame, "value", &out->value) == 0) {
            out->has_value = 1;
        }
        return 0;
    }

    if (strcmp(type, "query") == 0) {
        out->type = FRAME_QUERY;
        get_string(frame, "cmd_id", out->cmd_id, sizeof(out->cmd_id));
        get_string(frame, "query", out->query, sizeof(out->query));
        return 0;
    }

    /* register_ack 等其他帧：识别为 OTHER，交由上层决定是否忽略 */
    return 0;
}
