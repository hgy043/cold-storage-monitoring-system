/*
 * ui_main.c - gec6818 冷库终端（接收华为云虚拟数据 + LVGL 界面 + 声光报警）
 *
 * 角色（对应任务改动）：
 *   - gec6818 作终端与报警器：连 Ubuntu 虚拟数据转发程序（TCP 18830）
 *   - 接收 virt_data 帧（每 3s 虚拟数据 / 每 30s 报警数据）→ 更新 UI 显示
 *   - 报警判定（alarm=1 或任一温区 status=2）→ /dev/beep 蜂鸣 + LED 亮，吸引巡查
 *   - 巡查人员到场 → 触摸屏"确认报警"或云端 confirm_alarm 命令 → 停蜂鸣/灭 LED，发 confirm 帧
 *   - 云端 set_beeper/set_led 命令 → 执行 /dev/beep、/sys/class/leds/led1
 *
 * 运行：./coldstorage_ui <转发程序IP> [冷库名] [编号] [报警阈值]
 */
#include "lv_coldstorage_ui.h"

#include "lvgl/lvgl.h"
#include "lv_drivers/display/fbdev.h"
#include "lv_drivers/indev/evdev.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <math.h>
#include <pthread.h>
#include <time.h>
#include <fcntl.h>
#include <errno.h>
#include <stdarg.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define TCP_PORT 18830
#define LED_PATH "/sys/class/leds/led1/brightness"
#define BEEP_PATH "/dev/beep"

/* 终端日志：带 [HH:MM:SS] 时间戳，便于对照转发程序日志排查 */
static void tlog(const char *fmt, ...)
{
    time_t t = time(NULL);
    struct tm tmv;
    struct tm *tm = localtime_r(&t, &tmv);
    char tsb[32];
    snprintf(tsb, sizeof(tsb), "[%02d:%02d:%02d] ", tm->tm_hour, tm->tm_min, tm->tm_sec);
    fputs(tsb, stdout);
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    fflush(stdout);
}

/* ---------- 自动校时：解析转发程序帧里的服务器时间，偏差>10s 时校准 ----------
 * virt_data 帧 ts 为 "YYYY-MM-DD HH:MM:SS"（服务器本地时间，见 virtual_bridge.js iso()）。
 * 用 mktime 按板子本地时区解释后 settimeofday；后续顶栏 localtime 显示与服务器时间一致。 */
static void jstr(const char *json, const char *key, char *out, size_t outlen); /* 定义在下方，先声明 */
static void sync_clock_from_ts(const char *ts)
{
    if (!ts || !*ts)
        return;
    int y, mo, d, h, mi, s;
    if (sscanf(ts, "%d-%d-%d %d:%d:%d", &y, &mo, &d, &h, &mi, &s) != 6)
        return;
    struct tm tmv;
    memset(&tmv, 0, sizeof(tmv));
    tmv.tm_year = y - 1900;
    tmv.tm_mon = mo - 1;
    tmv.tm_mday = d;
    tmv.tm_hour = h;
    tmv.tm_min = mi;
    tmv.tm_sec = s;
    tmv.tm_isdst = -1;
    time_t t = mktime(&tmv);
    if (t == (time_t)-1)
        return;
    time_t now = time(NULL);
    if (labs((long)(now - t)) > 10)
    { /* 偏差超 10s 才校准，避免频繁 settimeofday */
        struct timeval tv = {.tv_sec = t, .tv_usec = 0};
        settimeofday(&tv, NULL);
        printf("[term] 自动校时: %s\n", ts);
    }
}

/* ---------- 简易 JSON 字段扫描（零依赖） ---------- */
static const char *find_val(const char *json, const char *key)
{
    char pat[48];
    const char *p;
    snprintf(pat, sizeof(pat), "\"%s\":", key);
    p = strstr(json, pat);
    return p ? p + strlen(pat) : NULL;
}
/* 提取 JSON 双精度字段（find_val 后 atof），缺省返回 def */
static double jd(const char *json, const char *key, double def)
{
    const char *v = find_val(json, key);
    return v ? atof(v) : def;
}
/* 提取 JSON 整数字段（find_val 后 atoi），缺省返回 def */
static int ji(const char *json, const char *key, int def)
{
    const char *v = find_val(json, key);
    return v ? atoi(v) : def;
}

/* ---------- 声光执行器 ---------- */
static void led_write(int on)
{
    int fd = open(LED_PATH, O_WRONLY);
    if (fd >= 0)
    {
        write(fd, on ? "1" : "0", 1);
        close(fd);
    }
    else
        printf("[act] LED open %s 失败: %s\n", LED_PATH, strerror(errno));
}
/* 蜂鸣器：/dev/beep 驱动 ioctl 接口（参考 buz_test.c）
 * ioctl(fd, 0, 1) = 响；ioctl(fd, 1, 1) = 停 */
static void beep_write(int on)
{
    int fd = open(BEEP_PATH, O_RDWR);
    if (fd >= 0)
    {
        ioctl(fd, on ? 0 : 1, 1);
        close(fd);
    }
    else
        printf("[act] beep open %s 失败: %s\n", BEEP_PATH, strerror(errno));
}

/* ---------- 报警/执行器状态（UI 回调与数据帧共享） ----------
 * 蜂鸣/LED 为状态驱动：报警响；否则由手动命令状态决定（默认不响）。
 * 手动命令状态（云端 set_beeper/set_led、界面按钮）不被每帧刷新覆盖：
 * 修 bug——旧实现 apply_alarm 无报警时每帧 beep_write(0)，3s 内吞掉云端命令触发的蜂鸣。 */
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_alarm = 0;
static int g_beep_manual = 0;   /* 云端/界面命令：蜂鸣器手动开关（默认不响） */
static int g_led_manual = 0;    /* 云端/界面命令：LED 手动开关 */
static long g_beep_until = 0;   /* set_beeper duration_ms 自动关时刻（0=常开） */
static long g_latch_until = 0;  /* 确认后压制窗口（秒）：吸收在途报警帧，防止回响 */

/* 汇总报警与命令状态，驱动蜂鸣/LED：报警自动响 OR 命令触发，确认后停止 */
static void apply_alarm(void)
{
    int alarm_on, beep_on, led_on;
    long now = time(NULL);
    pthread_mutex_lock(&g_lock);
    alarm_on = g_alarm && now >= g_latch_until;
    /* 蜂鸣 = 报警自动响 OR 命令触发（网页/华为云 set_beeper）；确认后报警停，命令状态保持 */
    beep_on = alarm_on || (g_beep_manual && (g_beep_until == 0 || now < g_beep_until));
    led_on  = alarm_on || g_led_manual;
    pthread_mutex_unlock(&g_lock);
    beep_write(beep_on);
    led_write(led_on);
}

/* ---------- TCP 终端（连 Ubuntu 转发程序） ---------- */
static char g_server_ip[64] = "127.0.0.1";
static int g_sock = -1;
static int g_stop = 0;
/* g_sock 被 UI 线程(tcp_send)与 TCP 线程(断开置 -1)共享，必须加锁：
 * 否则 UI 线程可能在 close 后写已关闭 fd，或向新连接写入旧帧 */
static pthread_mutex_t g_sock_lock = PTHREAD_MUTEX_INITIALIZER;

/* 向转发程序发送一帧 JSON：锁内 dup(fd) 防竞态，断线时静默丢弃 */
static void tcp_send(const char *line)
{
    /* TOCTOU 防护：锁内 dup(fd) 复制句柄，锁外写副本。
     * TCP 线程随后 close(fd)/置 -1 不影响副本，也不会错写进新连接。 */
    pthread_mutex_lock(&g_sock_lock);
    int fd = g_sock;
    int dupfd = (fd >= 0) ? dup(fd) : -1;
    pthread_mutex_unlock(&g_sock_lock);
    if (dupfd >= 0)
    {
        write(dupfd, line, strlen(line));
        write(dupfd, "\n", 1);
        close(dupfd);
    }
}

/* 处理 virt_data 帧 */
static void handle_virt_data(const char *json)
{
    char tsbuf[32];
    jstr(json, "ts", tsbuf, sizeof(tsbuf)); /* 服务器时间 → 自动校时 */
    if (tsbuf[0])
        sync_clock_from_ts(tsbuf);

    ui_state_t st;
    memset(&st, 0, sizeof(st));
    for (int z = 0; z < ZONE_COUNT; z++)
    {
        char k[8];
        snprintf(k, sizeof(k), "z%d_t", z + 1);
        st.zones[z].temperature = jd(json, k, 0);
        snprintf(k, sizeof(k), "z%d_h", z + 1);
        st.zones[z].humidity = jd(json, k, 0);
        snprintf(k, sizeof(k), "z%d_s", z + 1);
        st.zones[z].setpoint = jd(json, k, 0);
        snprintf(k, sizeof(k), "z%d_st", z + 1);
        st.zones[z].status = ji(json, k, 0);
    }
    st.system.outdoor_temp = jd(json, "outdoor", 0);
    st.system.system_online = 1;
    int alarm = ji(json, "alarm", 0);
    for (int z = 0; z < ZONE_COUNT; z++)
    {
        if (st.zones[z].status == 2)
            alarm = 1;
    }

    /* 能耗与设备状态（模拟真实值）：
     * 功率随温区温度波动，电量累加；除霜/风扇/门按状态位模拟 */
    static double sim_kwh = 0.0;
    static unsigned int sim_tick = 0;
    sim_tick++;
    double main_temp = st.zones[0].temperature;
    st.energy.power_kw = 3.2 + (main_temp < -22 ? 2.5 : 1.2) + 0.4 * sin(sim_tick / 8.0);
    st.energy.energy_kwh = sim_kwh;
    sim_kwh += st.energy.power_kw * 0.0014;
    st.energy.defrost = (sim_tick % 600) < 60;          /* 每 30 分钟化霜 3 分钟（600 帧=30min，60 帧=3min） */
    st.energy.condenser_fan = (sim_tick / 10) % 5 != 0; /* 周期性启停 */
    st.energy.door = (sim_tick % 240) < 6 ? 1 : 0;      /* 偶发开门 */
    pthread_mutex_lock(&g_lock);
    g_alarm = alarm;
    /* UI 显示实际执行器状态：报警 OR 手动命令 */
    st.led = g_led_manual || g_alarm;
    st.beeper = g_beep_manual || g_alarm;
    pthread_mutex_unlock(&g_lock);
    apply_alarm();
    ui_coldstorage_update(&st);

    /* 报警记录（去重：同温区同状态级别不重复追加）：
     * status=1 预警(重要), status=2 报警(紧急)；alarm=1 且无温区状态时也记一条 */
    static int last_st[ZONE_COUNT] = {-1, -1, -1, -1};
    static int last_alarm = -1;
    for (int z = 0; z < ZONE_COUNT; z++)
    {
        if (st.zones[z].status >= 1 && st.zones[z].status != last_st[z])
        {
            last_st[z] = st.zones[z].status;
            int lv = (st.zones[z].status >= 2) ? 2 : 1;
            const char *nm = (z == 0) ? "冷冻区" : (z == 1) ? "冷藏区"
                                               : (z == 2)   ? "预冷区"
                                                            : "穿堂";
            char desc[56];
            snprintf(desc, sizeof(desc), "温度 %.1f℃ 越限", st.zones[z].temperature);
            ui_coldstorage_alarm_add(lv, nm, desc);
            tlog("[term] 新增报警: %s %s\n", nm, desc);
        }
        else if (st.zones[z].status == 0 && last_st[z] != 0)
        {
            last_st[z] = 0;
        }
    }
    if (alarm && !last_alarm && st.zones[0].status < 1)
    {
        ui_coldstorage_alarm_add(2, "冷库", "综合报警");
        last_alarm = 1;
    }
    if (!alarm)
        last_alarm = 0;

    if (alarm && time(NULL) >= g_latch_until)
        tlog("[term] 报警! 蜂鸣器响, 等待确认\n");
}

/* 提取 JSON 字符串字段 */
static void jstr(const char *json, const char *key, char *out, size_t outlen)
{
    const char *v = find_val(json, key);
    if (v && *v == '"')
    {
        const char *e = strchr(v + 1, '"');
        size_t n = e ? (size_t)(e - v - 1) : strlen(v + 1);
        if (n >= outlen)
            n = outlen - 1;
        memcpy(out, v + 1, n);
        out[n] = 0;
    }
    else
    {
        out[0] = 0;
    }
}

/* 处理下行命令 */
static void handle_cmd(const char *json)
{
    char cmd[32] = "", cmd_id[64] = "";
    char line[200];
    jstr(json, "command", cmd, sizeof(cmd));
    jstr(json, "cmd_id", cmd_id, sizeof(cmd_id));
    printf("[term] 命令: %s (id=%s)\n", cmd, cmd_id);

    if (strcmp(cmd, "set_beeper") == 0)
    {
        int on = ji(json, "on", 0);
        int dur = ji(json, "duration_ms", 0);
        pthread_mutex_lock(&g_lock);
        g_beep_manual = on ? 1 : 0;
        g_beep_until = (on && dur > 0) ? time(NULL) + dur / 1000 : 0;
        pthread_mutex_unlock(&g_lock);
        apply_alarm();
        printf("[term] set_beeper on=%d dur=%d\n", on, dur);
    }
    else if (strcmp(cmd, "set_led") == 0)
    {
        int on = ji(json, "on", 0);
        pthread_mutex_lock(&g_lock);
        g_led_manual = on ? 1 : 0;
        pthread_mutex_unlock(&g_lock);
        apply_alarm();
        printf("[term] set_led on=%d\n", on);
    }
    else if (strcmp(cmd, "confirm_alarm") == 0)
    {
        pthread_mutex_lock(&g_lock);
        g_alarm = 0;
        g_latch_until = time(NULL) + 6;
        pthread_mutex_unlock(&g_lock);
        apply_alarm();
        printf("[term] 云端确认报警, 蜂鸣停止\n");
        char tsbuf[40];
        time_t nw = time(NULL);
        struct tm tmv2;
        struct tm *lt2 = localtime_r(&nw, &tmv2);
        strftime(tsbuf, sizeof(tsbuf), "%Y-%m-%dT%H:%M:%S%z", lt2);
        snprintf(line, sizeof(line),
                 "{\"type\":\"confirm\",\"node_id\":\"cold01\",\"ts\":\"%s\"}",
                 tsbuf);
        tcp_send(line);
    }
    /* 命令回执 */
    snprintf(line, sizeof(line),
             "{\"type\":\"ack\",\"node_id\":\"cold01\",\"cmd_id\":\"%s\","
             "\"command\":\"%s\",\"result\":\"success\",\"detail\":\"ok\"}",
             cmd_id, cmd);
    tcp_send(line);
}

/* ---------- 帧接收与分发 ---------- */
static char g_rx[4096];
static size_t g_rx_len = 0;
static int g_rx_overflow = 0; /* 超长帧标志：丢弃整帧直到换行 */

/* 按行切帧（处理粘包/半包）并分发：virt_data 数据帧 / cmd 命令帧 / register_ack */
static void feed_rx(const char *data, size_t n)
{
    for (size_t i = 0; i < n; i++)
    {
        char c = data[i];
        if (c == '\n')
        {
            if (g_rx_overflow)
            {
                g_rx_overflow = 0; /* 超长帧整行丢弃（协议约定：超长丢弃并告警） */
                g_rx_len = 0;
                printf("[term] 丢弃超长帧\n");
                continue;
            }
            if (g_rx_len > 0)
            {
                g_rx[g_rx_len] = 0;
                const char *type = strstr(g_rx, "\"type\":\"");
                if (type)
                {
                    const char *t = type + 8;
                    if (strncmp(t, "virt_data", 9) == 0)
                    {
                        handle_virt_data(g_rx);
                    }
                    else if (strncmp(t, "cmd", 3) == 0)
                    {
                        handle_cmd(g_rx);
                    }
                    else if (strncmp(t, "register_ack", 12) == 0)
                    {
                        printf("[term] 注册成功\n");
                    }
                }
                g_rx_len = 0;
            }
        }
        else if (g_rx_len < sizeof(g_rx) - 1)
        {
            g_rx[g_rx_len++] = c;
        }
        else
        {
            g_rx_overflow = 1; /* 超长：后续字节丢弃，直到换行复位 */
        }
    }
}

/* TCP 连接线程：重连退避 + 注册 + 接收 */
static void *tcp_thread(void *arg)
{
    (void)arg;
    int attempt = 0;
    while (!g_stop)
    {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd >= 0)
        {
            struct sockaddr_in addr;
            memset(&addr, 0, sizeof(addr));
            addr.sin_family = AF_INET;
            addr.sin_port = htons(TCP_PORT);
            inet_pton(AF_INET, g_server_ip, &addr.sin_addr);
            if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0)
            {
                pthread_mutex_lock(&g_sock_lock);
                g_sock = fd;
                pthread_mutex_unlock(&g_sock_lock);
                attempt = 0;
                g_rx_len = 0;
                printf("[term] 已连接转发程序 %s:%d\n", g_server_ip, TCP_PORT);
                tcp_send("{\"type\":\"register\",\"node_id\":\"cold01\",\"name\":\"1号冷库终端\",\"interval\":3,\"model\":\"gec6818\"}");
                char buf[1024];
                ssize_t n;
                while (!g_stop && (n = read(fd, buf, sizeof(buf))) > 0)
                {
                    feed_rx(buf, (size_t)n);
                }
                pthread_mutex_lock(&g_sock_lock);
                g_sock = -1;
                pthread_mutex_unlock(&g_sock_lock);
                close(fd);
                printf("[term] 连接断开, 重连中\n");
            }
            else
            {
                close(fd);
            }
        }
        /* 退避 1,2,4,8,16 → 封顶 30s（docs/02 §1：上限 30s） */
        int delay = (attempt < 5) ? (1 << attempt) : 30;
        sleep(delay);
        if (attempt < 8)
            attempt++;
    }
    return NULL;
}

/* ---------- UI 动作回调：确认报警 / 执行器 ---------- */
static void on_action(void *user, const char *action, int value)
{
    (void)user;
    tlog("[UI] 屏幕按钮: %s = %d\n", action, value);
    if (strcmp(action, "confirm_alarm") == 0)
    {
        pthread_mutex_lock(&g_lock);
        g_alarm = 0;
        g_latch_until = time(NULL) + 6; /* 6 秒窗口：吸收在途报警帧，防止回响 */
        pthread_mutex_unlock(&g_lock);
        apply_alarm();
        char tsbuf[40];
        time_t nw = time(NULL);
        struct tm tmv3;
        struct tm *lt3 = localtime_r(&nw, &tmv3);
        strftime(tsbuf, sizeof(tsbuf), "%Y-%m-%dT%H:%M:%S%z", lt3);
        char line[200];
        snprintf(line, sizeof(line),
                 "{\"type\":\"confirm\",\"node_id\":\"cold01\",\"ts\":\"%s\"}",
                 tsbuf);
        tcp_send(line);
        tlog("[term] 已确认报警, 蜂鸣停止, 上报确认\n");
    }
    else if (strcmp(action, "led") == 0)
    {
        pthread_mutex_lock(&g_lock);
        g_led_manual = value ? 1 : 0;
        pthread_mutex_unlock(&g_lock);
        apply_alarm();
    }
    else if (strcmp(action, "beeper") == 0)
    {
        pthread_mutex_lock(&g_lock);
        g_beep_manual = value ? 1 : 0;
        g_beep_until = 0;
        pthread_mutex_unlock(&g_lock);
        apply_alarm();
    }
    /* threshold/interval/spike 在虚拟数据场景由转发程序管理，忽略 */
}

/* ---------- 主函数 ----------
启动时：解析命令行参数 → 打印配置 → 初始化图形栈。
准备显示和输入：初始化 framebuffer、注册显示驱动、设置主题、注册触摸输入。
创建业务界面：构建冷库监控 UI，设置交互回调。
后台数据通信：启动 TCP 线程，与服务器/转发程序通信，获取实时数据。
主循环：持续处理 GUI 事件，更新界面，直到程序退出。

*/
int main(int argc, char **argv)
{
    const char *server_ip = (argc > 1) ? argv[1] : "127.0.0.1";
    const char *cold_name = (argc > 2) ? argv[2] : "1号冷库";
    const char *cold_no = (argc > 3) ? argv[3] : "NO.001";
    double threshold = (argc > 4) ? atof(argv[4]) : -20.0;
    uint32_t hor = 800, ver = 480;

    snprintf(g_server_ip, sizeof(g_server_ip), "%s", server_ip);
    printf("===== 冷库终端 (LVGL SCADA · 800x480) =====\n");
    printf("[配置] 转发程序=%s  %s %s 报警阈值=%.0f℃\n", g_server_ip, cold_name, cold_no, threshold);

    lv_init();

    fbdev_init();
    fbdev_get_sizes(&hor, &ver, NULL);
    printf("[显示] framebuffer %ux%u\n", (unsigned)hor, (unsigned)ver);
    if (hor == 0 || ver == 0)
    {
        hor = 800;
        ver = 480;
    }

    static lv_color_t buf[1024 * 64]; // 显示缓冲区
    static lv_disp_draw_buf_t disp_buf;
    lv_disp_draw_buf_init(&disp_buf, buf, NULL, 1024 * 64); // 单缓冲，大小 64K 像素
    static lv_disp_drv_t disp_drv;                          // 显示驱动结构体
    lv_disp_drv_init(&disp_drv);
    disp_drv.draw_buf = &disp_buf;
    disp_drv.flush_cb = fbdev_flush; // 刷屏回调函数（由 fbdev 实现）
    disp_drv.hor_res = hor;
    disp_drv.ver_res = ver;
    lv_disp_drv_register(&disp_drv); // 注册显示设备

    /* 深色主题（显示注册之后） */
    lv_theme_t *ui_theme = lv_theme_default_init(
        lv_disp_get_default(),
        lv_palette_main(LV_PALETTE_CYAN),
        lv_palette_main(LV_PALETTE_BLUE),
        true, &lv_font_montserrat_14);
    if (ui_theme)
        lv_disp_set_theme(lv_disp_get_default(), ui_theme);

    evdev_init();
    static lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);
    indev_drv.type = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = evdev_read;
    lv_indev_drv_register(&indev_drv);

    if (ui_coldstorage_init(cold_name, cold_no, threshold) != 0)
    {
        fprintf(stderr, "界面初始化失败\n");
        return 1;
    }
    ui_coldstorage_set_action_cb(on_action, NULL);

    /* 启动 TCP 终端线程（连接转发程序、收虚拟数据） */
    pthread_t tid;
    if (pthread_create(&tid, NULL, tcp_thread, NULL) != 0)
    {
        fprintf(stderr, "TCP 线程创建失败\n");
        return 1;
    }

    printf("[term] 运行中... 触摸操作; 报警时蜂鸣器响, 点取消报警按钮停止\n");
    while (1)
    {
        lv_timer_handler();
        lv_tick_inc(5);
        usleep(5000);
    }
    return 0;
}