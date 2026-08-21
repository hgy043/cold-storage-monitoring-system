/*
 * device.c - 冷库数据监控系统 设备端主程序
 *
 * 职责：
 *   读配置 -> 初始化传感器/执行器 -> 连网关 -> 发 register -> 按周期发 data；
 *   接收线程处理 cmd(set_led/set_beeper/set_interval/set_alarm_threshold) 与 query，
 *   执行动作后回 ack 并立即补发一帧 data；本地报警判定 temperature > alarm_threshold。
 *
 * 编译：纯 C99 + POSIX（socket/pthreads），零第三方依赖。
 */
#include "protocol.h"
#include "sensor.h"
#include "actuator.h"
#include "tcp_client.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>

#define MAX_LINE 512

/* ---- 配置 ---- */
typedef struct
{
    char gateway_ip[64];
    int gateway_port;
    char node_id[32];
    char name[64];
    int interval;           /* 上报周期（秒） */
    double alarm_threshold; /* 报警阈值（℃） */
    int sim_mode;           /* 1=模拟 */
    int led_gpio;
    int beeper_gpio;
    char sensor_temp_path[256];
    char sensor_humi_path[256];
} config_t;

static config_t g_cfg;
static pthread_mutex_t g_cfg_lock = PTHREAD_MUTEX_INITIALIZER;
static tcp_client_t *g_client = NULL;
static int g_registered = 0; /* 仅主线程访问 */
static volatile sig_atomic_t g_running = 1;

/* 信号处理：SIGINT/SIGTERM 置停止标志，主循环检测后优雅退出 */
static void on_signal(int sig)
{
    (void)sig;
    g_running = 0;
}

/* 载入默认配置（网关 127.0.0.1:18830、节点 cold01_front、阈值 -15℃ 等） */
static void config_default(config_t *c)
{
    memset(c, 0, sizeof(*c));
    snprintf(c->gateway_ip, sizeof(c->gateway_ip), "127.0.0.1");
    c->gateway_port = 18830;
    snprintf(c->node_id, sizeof(c->node_id), "cold01_front");
    snprintf(c->name, sizeof(c->name), "1号冷库-前门");
    c->interval = 5;
    c->alarm_threshold = -15.0;
    c->sim_mode = 1;
    c->led_gpio = -1;
    c->beeper_gpio = -1;
    c->sensor_temp_path[0] = '\0';
    c->sensor_humi_path[0] = '\0';
}

/* 去掉字符串首尾空白，返回去除后的字符串 */
static char *trim(char *s)
{
    char *end;
    while (*s == ' ' || *s == '\t')
    {
        s++;
    }
    end = s + strlen(s);
    while (end > s && (end[-1] == ' ' || end[-1] == '\t' ||
                       end[-1] == '\r' || end[-1] == '\n'))
    {
        end--;
    }
    *end = '\0';
    return s;
}

/* 解析一行 "key=value" 配置：按 key 分派写入对应字段，未知键忽略 */
static void config_apply_line(config_t *c, char *line)
{
    char *key, *val, *eq;

    key = trim(line);
    if (key[0] == '\0' || key[0] == '#')
    {
        return;
    }
    eq = strchr(key, '=');
    if (!eq)
    {
        return;
    }
    *eq = '\0';
    val = trim(eq + 1);

    if (strcmp(key, "gateway_ip") == 0)
    {
        snprintf(c->gateway_ip, sizeof(c->gateway_ip), "%s", val);
    }
    else if (strcmp(key, "gateway_port") == 0)
    {
        c->gateway_port = atoi(val);
    }
    else if (strcmp(key, "node_id") == 0)
    {
        snprintf(c->node_id, sizeof(c->node_id), "%s", val);
    }
    else if (strcmp(key, "name") == 0)
    {
        snprintf(c->name, sizeof(c->name), "%s", val);
    }
    else if (strcmp(key, "interval") == 0)
    {
        int v = atoi(val);
        if (v >= 1 && v <= 600)
        {
            c->interval = v;
        }
    }
    else if (strcmp(key, "alarm_threshold") == 0)
    {
        c->alarm_threshold = atof(val);
    }
    else if (strcmp(key, "sim_mode") == 0)
    {
        c->sim_mode = (atoi(val) != 0);
    }
    else if (strcmp(key, "led_gpio") == 0)
    {
        c->led_gpio = atoi(val);
    }
    else if (strcmp(key, "beeper_gpio") == 0)
    {
        c->beeper_gpio = atoi(val);
    }
    else if (strcmp(key, "sensor_temp_path") == 0)
    {
        snprintf(c->sensor_temp_path, sizeof(c->sensor_temp_path), "%s", val);
    }
    else if (strcmp(key, "sensor_humi_path") == 0)
    {
        snprintf(c->sensor_humi_path, sizeof(c->sensor_humi_path), "%s", val);
    }
}

/* 从文件加载配置：逐行解析，空行/# 注释忽略；文件缺失则用默认值 */
static int config_load(config_t *c, const char *path)
{
    FILE *f;
    char line[MAX_LINE];

    config_default(c);
    f = fopen(path, "r");
    if (!f)
    {
        printf("[config] 未找到 %s,使用默认配置\n", path);
        return 0;
    }
    while (fgets(line, sizeof(line), f))
    {
        config_apply_line(c, line);
    }
    fclose(f);
    return 0;
}

/* 生成 ISO8601 带时区时间戳，如 2025-01-01T12:00:00+08:00 */
static void make_iso8601(char *out, size_t len)
{
    time_t t = time(NULL);
    struct tm lt;
    char base[32];
    char tz[16];
    char off[16];

    localtime_r(&t, &lt);
    strftime(base, sizeof(base), "%Y-%m-%dT%H:%M:%S", &lt);
    strftime(tz, sizeof(tz), "%z", &lt); /* 形如 +0800 */

    /* 把 +0800 改写成 +08:00 */
    if (strlen(tz) == 5 && (tz[0] == '+' || tz[0] == '-'))
    {
        snprintf(off, sizeof(off), "%c%c%c:%c%c",
                 tz[0], tz[1], tz[2], tz[3], tz[4]);
    }
    else
    {
        snprintf(off, sizeof(off), "%s", tz);
    }
    snprintf(out, len, "%s%s", base, off);
}

/* 采集当前状态并发送一帧 data */
static void send_data_frame(void)
{
    char ts[48];
    char buf[PROTO_FRAME_MAX];
    double temp, humi, thr;
    int led, beeper, alarm, n;

    make_iso8601(ts, sizeof(ts));
    if (sensor_read(&temp, &humi) != 0)
    {
        printf("[device] 传感器读取失败，跳过本次上报\n");
        return;
    }
    actuator_get_state(&led, &beeper);

    pthread_mutex_lock(&g_cfg_lock);
    thr = g_cfg.alarm_threshold;
    pthread_mutex_unlock(&g_cfg_lock);

    alarm = (temp > thr) ? 1 : 0;

    n = proto_build_data(buf, sizeof(buf), g_cfg.node_id, ts,
                         temp, humi, led, beeper, alarm);
    if (n < 0)
    {
        printf("[device] 数据帧构造失败\n");
        return;
    }
    if (tcp_client_send(g_client, buf, (size_t)n) != 0)
    {
        printf("[device] 数据帧发送失败（未连接或已断开）\n");
    }
}

/* 发送注册帧 */
static void send_register(void)
{
    char buf[PROTO_FRAME_MAX];
    const char *model = g_cfg.sim_mode ? "gec6818-sim" : "gec6818";
    int n = proto_build_register(buf, sizeof(buf), g_cfg.node_id,
                                 g_cfg.name, g_cfg.interval, model);
    if (n < 0)
    {
        printf("[device] 注册帧构造失败\n");
        return;
    }
    if (tcp_client_send(g_client, buf, (size_t)n) == 0)
    {
        printf("[device] 已发送注册帧 node_id=%s model=%s\n",
               g_cfg.node_id, model);
    }
}

/* 回 ack 后立即补发一帧 data */
static void send_ack_then_data(const char *cmd_id,
                               const char *result, const char *detail)
{
    char buf[PROTO_FRAME_MAX];
    int n = proto_build_ack(buf, sizeof(buf), g_cfg.node_id,
                            cmd_id, result, detail);
    if (n > 0)
    {
        tcp_client_send(g_client, buf, (size_t)n);
    }
    send_data_frame();
}

/* 下行帧回调（运行于接收线程） */
static void on_frame(const char *frame, size_t len, void *user)
{
    proto_frame_t f;
    (void)user;

    if (proto_parse_frame(frame, len, &f) != 0)
    {
        printf("[device] 非法帧丢弃: %.*s\n", (int)len, frame);
        return;
    }

    if (f.type == FRAME_CMD)
    {
        const char *result = "success";
        char detail[64] = "ok";
        int failed = 0;

        if (strcmp(f.command, "set_led") == 0)
        {
            if (f.has_on)
            {
                actuator_set_led(f.on);
                snprintf(detail, sizeof(detail), "LED %s", f.on ? "on" : "off");
            }
            else
            {
                failed = 1;
                snprintf(detail, sizeof(detail), "missing paras.on");
            }
        }
        else if (strcmp(f.command, "set_beeper") == 0)
        {
            if (f.has_on)
            {
                int dur = f.has_duration_ms ? f.duration_ms : 3000;
                actuator_set_beeper(f.on, f.on ? dur : 0);
                snprintf(detail, sizeof(detail), "beeper %s", f.on ? "on" : "off");
            }
            else
            {
                failed = 1;
                snprintf(detail, sizeof(detail), "missing paras.on");
            }
        }
        else if (strcmp(f.command, "set_interval") == 0)
        {
            if (f.has_seconds && f.seconds >= 1 && f.seconds <= 600)
            {
                pthread_mutex_lock(&g_cfg_lock);
                g_cfg.interval = f.seconds;
                pthread_mutex_unlock(&g_cfg_lock);
                snprintf(detail, sizeof(detail), "interval set to %ds", f.seconds);
            }
            else
            {
                failed = 1;
                snprintf(detail, sizeof(detail), "invalid seconds (1~600)");
            }
        }
        else if (strcmp(f.command, "set_alarm_threshold") == 0)
        {
            if (f.has_value)
            {
                pthread_mutex_lock(&g_cfg_lock);
                g_cfg.alarm_threshold = f.value;
                pthread_mutex_unlock(&g_cfg_lock);
                snprintf(detail, sizeof(detail), "alarm_threshold set to %.1f", f.value);
            }
            else
            {
                failed = 1;
                snprintf(detail, sizeof(detail), "missing paras.value");
            }
        }
        else
        {
            failed = 1;
            snprintf(detail, sizeof(detail), "unsupported command %s", f.command);
        }

        if (failed)
        {
            result = "failed";
        }
        send_ack_then_data(f.cmd_id, result, detail);
    }
    else if (f.type == FRAME_QUERY)
    {
        char buf[PROTO_FRAME_MAX];
        int n;
        /* 查询：先回最新 data，再回 ack */
        send_data_frame();
        n = proto_build_ack(buf, sizeof(buf), g_cfg.node_id,
                            f.cmd_id, "success", "status queried");
        if (n > 0)
        {
            tcp_client_send(g_client, buf, (size_t)n);
        }
    }
    /* FRAME_OTHER（如 register_ack）忽略 */
}

/* 主流程：加载配置 → 初始化传感器/执行器 → 启动 TCP 客户端 → 周期上报 → 处理下行命令/信号 */
int main(int argc, char **argv)
{
    const char *conf = "device.conf";
    int prev_connected = 0;
    int loop_ticks = 0;
    int interval;

    if (argc > 1)
    {
        conf = argv[1];
    }

#ifdef SIGPIPE
    signal(SIGPIPE, SIG_IGN); /* 避免对端断开触发 SIGPIPE 杀死进程 */
#endif
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    config_load(&g_cfg, conf);

    printf("===== 冷库数据监控系统 设备端 =====\n");
    printf("[config] gateway=%s:%d node_id=%s interval=%ds "
           "alarm_threshold=%.1f℃ sim_mode=%d\n",
           g_cfg.gateway_ip, g_cfg.gateway_port, g_cfg.node_id,
           g_cfg.interval, g_cfg.alarm_threshold, g_cfg.sim_mode);

    //传感器
    if (sensor_init(g_cfg.sim_mode,
                    g_cfg.sensor_temp_path[0] ? g_cfg.sensor_temp_path : NULL,
                    g_cfg.sensor_humi_path[0] ? g_cfg.sensor_humi_path : NULL) != 0)
    {
        fprintf(stderr, "传感器初始化失败\n");
        return 1;
    }
    if (actuator_init(g_cfg.sim_mode, g_cfg.led_gpio, g_cfg.beeper_gpio) != 0)
    {
        fprintf(stderr, "执行器初始化失败\n");
        return 1;
    }

    //TCP
    g_client = tcp_client_create(g_cfg.gateway_ip, g_cfg.gateway_port);
    if (!g_client)
    {
        fprintf(stderr, "TCP 客户端创建失败\n");
        return 1;
    }
    tcp_client_set_frame_cb(g_client, on_frame, NULL);
    if (tcp_client_start(g_client) != 0)
    {
        fprintf(stderr, "TCP 客户端启动失败\n");
        return 1;
    }

    printf("[device] 运行中…(Ctrl+C 退出）\n");

    while (g_running)
    {
        int connected;

        sleep(1);
        actuator_poll(); /* 蜂鸣器定时关闭 */

        connected = tcp_client_is_connected(g_client);
        if (connected && !prev_connected)
        {
            g_registered = 0; /* 新连接需要重新注册 */
        }
        if (connected && !g_registered)
        {
            send_register();
            g_registered = 1;
        }
        prev_connected = connected;

        loop_ticks++;
        pthread_mutex_lock(&g_cfg_lock);
        interval = g_cfg.interval;
        pthread_mutex_unlock(&g_cfg_lock);
        if (interval < 1)
        {
            interval = 1;
        }

        if (loop_ticks >= interval)
        {
            loop_ticks = 0;
            if (connected && g_registered)
            {
                send_data_frame();
            }
        }
    }

    printf("[device] 正在退出…\n");
    tcp_client_stop(g_client);
    tcp_client_destroy(g_client);
    actuator_deinit();
    sensor_deinit();
    return 0;
}
