/*
 * sensor.c - 温湿度采集
 *
 * 双模式：
 *   - 真实模式(sim_mode=0)：从 sysfs 文件读取温湿度（如 /sys/class/hwmon/...）。
 *   - 模拟模式(sim_mode=1)：围绕冷库基准 -18℃ 做随机游走，湿度 55~75，
 *     偶尔产生尖峰以模拟故障。
 * 若真实模式未配置或 sysfs 文件不存在，自动退化为模拟模式。
 */
#include "sensor.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>

/* 冷库基准参数 */
#define SIM_BASE_TEMP    (-18.0)
#define SIM_HUMI_BASE     65.0
#define SIM_HUMI_MIN      55.0
#define SIM_HUMI_MAX      75.0

static int g_sim_mode = 1;      /* 1=模拟，0=真实 */
static int g_real_ok = 0;       /* 真实传感器是否可用 */
static char g_temp_path[256];
static char g_humi_path[256];

static double g_temp = SIM_BASE_TEMP;
static double g_humi = SIM_HUMI_BASE;
static int g_seeded = 0;

/* 判断路径是否为普通文件（真实模式传感器文件存在性检查） */
static int file_is_regular(const char *path)
{
    struct stat st;
    if (!path || path[0] == '\0') {
        return 0;
    }
    return (stat(path, &st) == 0 && S_ISREG(st.st_mode));
}

/* 从 sysfs 读一个浮点数值 */
static double read_sysfs_double(const char *path)
{
    FILE *f = fopen(path, "r");
    double v = 0.0;
    if (!f) {
        return 0.0;
    }
    if (fscanf(f, "%lf", &v) != 1) {
        v = 0.0;
    }
    fclose(f);
    return v;
}

/* 均匀随机数 [0,1) */
static double frand(void)
{
    return (double)rand() / ((double)RAND_MAX + 1.0);
}

/* 初始化：sim_mode=1 走模拟随机游走；否则检查 sysfs 传感器文件 */
int sensor_init(int sim_mode, const char *temp_path, const char *humi_path)
{
    g_sim_mode = (sim_mode != 0);
    g_real_ok = 0;
    g_temp_path[0] = '\0';
    g_humi_path[0] = '\0';

    if (temp_path) {
        snprintf(g_temp_path, sizeof(g_temp_path), "%s", temp_path);
    }
    if (humi_path) {
        snprintf(g_humi_path, sizeof(g_humi_path), "%s", humi_path);
    }

    if (!g_sim_mode) {
        if (file_is_regular(g_temp_path) && file_is_regular(g_humi_path)) {
            g_real_ok = 1;
            printf("[sensor] 真实硬件模式: temp=%s humi=%s\n",
                   g_temp_path, g_humi_path);
        } else {
            printf("[sensor] 警告: 未找到温湿度 sysfs 文件，退化为模拟模式\n");
        }
    } else {
        printf("[sensor] 模拟模式: 围绕 %.1f℃ 随机游走, 湿度 %.0f~%.0f\n",
               SIM_BASE_TEMP, SIM_HUMI_MIN, SIM_HUMI_MAX);
    }

    g_temp = SIM_BASE_TEMP;
    g_humi = SIM_HUMI_BASE;
    return 0;
}

/* 反初始化（当前无资源需释放，保留接口） */
void sensor_deinit(void)
{
    /* 无资源需释放 */
}

/* 真实模式：读取 sysfs；常见 hwmon 以毫单位上报（18500 -> 18.5），按量级换算 */
static int read_real(double *temp, double *humi)
{
    double t_raw = read_sysfs_double(g_temp_path);
    double h_raw = read_sysfs_double(g_humi_path);
    double t, h;

    t = (t_raw > 1000.0 || t_raw < -1000.0) ? t_raw / 1000.0 : t_raw;
    h = (h_raw > 1000.0) ? h_raw / 1000.0 : h_raw;

    if (t < -60.0 || t > 80.0) {
        return -1;   /* 读数异常，视为失败 */
    }
    if (h < 0.0 || h > 100.0) {
        h = SIM_HUMI_BASE;  /* 湿度缺失时用基准值 */
    }
    *temp = t;
    *humi = h;
    return 0;
}

/* 模拟模式：随机游走 + 偶发尖峰 */
static int read_sim(double *temp, double *humi)
{
    if (!g_seeded) {
        /* 种子 = 时间 ^ pid ^ 栈地址：避免同一秒内启动的进程序列相同 */
    srand((unsigned int)(time(NULL) ^ (long)getpid() ^ (unsigned long)&g_seeded));
        g_seeded = 1;
    }

    /* 温度随机游走，并向基准缓慢回归 */
    g_temp += (frand() * 2.0 - 1.0) * 0.3;
    g_temp += (SIM_BASE_TEMP - g_temp) * 0.05;

    /* 2% 概率产生尖峰，模拟门未关严/压缩机故障导致的升温 */
    if (frand() < 0.02) {
        g_temp = SIM_BASE_TEMP + 5.0 + frand() * 10.0;  /* 约 -13 ~ -3℃ */
    }

    if (g_temp < SIM_BASE_TEMP - 5.0) {
        g_temp = SIM_BASE_TEMP - 5.0;
    }
    if (g_temp > 5.0) {
        g_temp = 5.0;
    }

    /* 湿度随机游走，限制在 55~75 */
    g_humi += (frand() * 2.0 - 1.0) * 1.0;
    g_humi += (SIM_HUMI_BASE - g_humi) * 0.05;
    if (g_humi < SIM_HUMI_MIN) {
        g_humi = SIM_HUMI_MIN;
    }
    if (g_humi > SIM_HUMI_MAX) {
        g_humi = SIM_HUMI_MAX;
    }

    *temp = g_temp;
    *humi = g_humi;
    return 0;
}

/* 读取温湿度：模拟返回内部游走值；真实读 sysfs，失败回退 0 */
int sensor_read(double *temp, double *humi)
{
    if (!temp || !humi) {
        return -1;
    }
    if (!g_sim_mode && g_real_ok) {
        return read_real(temp, humi);
    }
    return read_sim(temp, humi);
}
