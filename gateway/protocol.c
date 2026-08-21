/*
 * protocol.c —— 设备<->网关 TCP 帧协议实现（对应 docs/02-设备-网关TCP协议.md）
 *
 * 使用华为 SDK 自带的 JSON_* 工具解析/构造 JSON，
 * 不直接引入 cJSON 头文件，避免类型冲突。
 */
#include <stdio.h>
#include <string.h>

#include "protocol.h"
#include "JsonUtil.h"
#include "StringUtil.h"

/* 解析设备上行帧：register/data/ack，非法帧返回 0 */
int protocol_parse_frame(const char *buf, Frame *out)
{
    JSON *root;
    const char *type;
    const char *node_id;

    if (buf == NULL || out == NULL) {
        return -1;
    }
    memset(out, 0, sizeof(*out));

    root = JSON_Parse(buf);
    if (root == NULL) {
        return -1;
    }

    type = JSON_GetStringFromObject(root, "type", NULL);
    node_id = JSON_GetStringFromObject(root, "node_id", NULL);

    /* 缺 type / 缺 node_id 视为非法帧（见 docs/02 异常约定） */
    if (type == NULL || type[0] == '\0' ||
        node_id == NULL || node_id[0] == '\0') {
        JSON_Delete(root);
        return -1;
    }
    strncpy(out->type, type, sizeof(out->type) - 1);
    strncpy(out->node_id, node_id, sizeof(out->node_id) - 1);

    if (strcmp(out->type, "register") == 0) {
        const char *name = JSON_GetStringFromObject(root, "name", "");
        const char *model = JSON_GetStringFromObject(root, "model", "");
        if (name != NULL) {
            strncpy(out->name, name, sizeof(out->name) - 1);
        }
        if (model != NULL) {
            strncpy(out->model, model, sizeof(out->model) - 1);
        }
        out->interval = JSON_GetIntFromObject(root, "interval", 5);
    } else if (strcmp(out->type, "data") == 0) {
        const char *ts = JSON_GetStringFromObject(root, "ts", "");
        if (ts != NULL) {
            strncpy(out->ts, ts, sizeof(out->ts) - 1);
        }
        out->temperature = JSON_GetDoubleFromObject(root, "temperature", 0.0);
        out->humidity = JSON_GetDoubleFromObject(root, "humidity", 0.0);
        out->led = JSON_GetIntFromObject(root, "led", 0);
        out->beeper = JSON_GetIntFromObject(root, "beeper", 0);
        out->alarm = JSON_GetIntFromObject(root, "alarm", 0);
    } else if (strcmp(out->type, "ack") == 0) {
        const char *cmd_id = JSON_GetStringFromObject(root, "cmd_id", "");
        const char *result = JSON_GetStringFromObject(root, "result", "");
        const char *detail = JSON_GetStringFromObject(root, "detail", "");
        if (cmd_id != NULL) {
            strncpy(out->cmd_id, cmd_id, sizeof(out->cmd_id) - 1);
        }
        if (result != NULL) {
            strncpy(out->result, result, sizeof(out->result) - 1);
        }
        if (detail != NULL) {
            strncpy(out->detail, detail, sizeof(out->detail) - 1);
        }
    }

    JSON_Delete(root);
    return 0;
}

/* 构造 register_ack 响应帧（含服务器时间） */
int protocol_build_register_ack(const char *node_id, char *out, size_t outlen)
{
    const char *ts = getClientTimeStamp();
    int n = snprintf(out, outlen,
                     "{\"type\":\"register_ack\",\"node_id\":\"%s\","
                     "\"result\":\"success\",\"server_time\":\"%s\"}\n",
                     node_id != NULL ? node_id : "",
                     ts != NULL ? ts : "");
    if (n < 0 || (size_t)n >= outlen) {
        return -1;
    }
    return n;
}

/* 构造 cmd 下行帧（命令下发：set_led/set_beeper 等） */
int protocol_build_cmd(const char *cmd_id, const char *command,
                       const char *paras_json, char *out, size_t outlen)
{
    /* paras_json 已是合法 JSON 对象字符串（来自 JSON_Print），直接内嵌 */
    int n = snprintf(out, outlen,
                     "{\"type\":\"cmd\",\"cmd_id\":\"%s\","
                     "\"command\":\"%s\",\"paras\":%s}\n",
                     cmd_id != NULL ? cmd_id : "",
                     command != NULL ? command : "",
                     paras_json != NULL ? paras_json : "{}");
    if (n < 0 || (size_t)n >= outlen) {
        return -1;
    }
    return n;
}

/* 构造 query 下行帧（状态查询） */
int protocol_build_query(const char *cmd_id, const char *query,
                         char *out, size_t outlen)
{
    int n = snprintf(out, outlen,
                     "{\"type\":\"query\",\"cmd_id\":\"%s\",\"query\":\"%s\"}\n",
                     cmd_id != NULL ? cmd_id : "",
                     query != NULL ? query : "");
    if (n < 0 || (size_t)n >= outlen) {
        return -1;
    }
    return n;
}
