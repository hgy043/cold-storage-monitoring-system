/*
 * gateway.c —— 冷库监控系统边缘网关主程序
 *
 * 流程：读 ClientConf.json -> IOTA_Init -> 配置 -> 注册回调 -> IOTA_Connect
 *       -> 启动 TCP server -> 主循环 sleep；云连接断开由回调自动重连。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <stdarg.h>

#include "hw_type.h"
#include "iota_init.h"
#include "LogUtil.h"

#include "cloud_bridge.h"
#include "device_table.h"
#include "tcp_server.h"
#include "protocol.h"

/* SDK 根目录（相对 gateway/ 运行目录），IOTA_Init 需要 conf/rootcert.pem */
#define SDK_WORK_PATH "../../huaweicloud-iot-device-quickstart"

/* 日志回调：打印到控制台（AgentLite 约定） */
static void myPrintLog(int level, char *format, va_list args)
{
    (void)level;
    vprintf(format, args);
    fflush(stdout);
}

/* TCP 收到完整帧 -> 分发 */
static void gateway_on_frame(int fd, const Frame *frame)
{
    if (strcmp(frame->type, "register") == 0) {
        device_table_register(frame->node_id, frame->name, frame->model, frame->interval, fd);
        {
            char ack[512];
            int n = protocol_build_register_ack(frame->node_id, ack, sizeof(ack));
            if (n > 0) {
                tcp_server_send(fd, ack, (size_t)n);
            }
        }
        printfLog(EN_LOG_LEVEL_INFO,
                  "gateway: device registered node_id=%s fd=%d name=%s model=%s\n",
                  frame->node_id, fd, frame->name, frame->model);
    } else if (strcmp(frame->type, "data") == 0) {
        device_table_update_data(frame->node_id, fd, frame);
    } else if (strcmp(frame->type, "ack") == 0) {
        cloud_bridge_on_device_ack(frame->node_id, frame->cmd_id,
                                   frame->result, frame->detail);
    } else {
        printfLog(EN_LOG_LEVEL_WARNING,
                  "gateway: unknown frame type=%s from node_id=%s\n",
                  frame->type, frame->node_id);
    }
}

/* TCP 连接断开 -> 从设备表移除 */
static void gateway_on_disconnect(int fd)
{
    device_table_unregister_by_fd(fd);
}

/* 主流程：加载配置 → 初始化设备表/协议回调 → 启动 TCP 服务 → 连接华为云 → 主循环睡眠 */
int main(int argc, char **argv)
{
    const char *conf = (argc > 1) ? argv[1] : "./ClientConf.json";
    GatewayConfig cfg;

    /* 忽略 SIGPIPE，避免向已断开连接写时进程退出（Windows/MinGW 无 SIGPIPE） */
#ifdef SIGPIPE
    signal(SIGPIPE, SIG_IGN);
#endif

    IOTA_SetPrintLogCallback(myPrintLog);

    if (cloud_bridge_load_config(&cfg, conf) != 0) {
        return 1;
    }

    device_table_init(cfg.defaultAlarmThreshold);

    if (cloud_bridge_init(&cfg, SDK_WORK_PATH) != 0) {
        return 1;
    }

    if (cloud_bridge_connect() != 0) {
        printfLog(EN_LOG_LEVEL_WARNING,
                  "gateway: initial connect failed, will retry via callback\n");
    }

    cloud_bridge_start_report_thread();

    if (tcp_server_start(cfg.tcpPort, gateway_on_frame, gateway_on_disconnect) != 0) {
        return 1;
    }

    printfLog(EN_LOG_LEVEL_INFO, "gateway: running...\n");

    /* 主循环：仅睡眠；云重连由回调完成，上报/离线清理由报告线程完成 */
    while (1) {
        sleep(10);
    }

    return 0;
}
