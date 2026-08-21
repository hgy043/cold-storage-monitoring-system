/*
 * actuator.c - LED 与蜂鸣器控制
 *
 * 真实模式：sysfs GPIO（/sys/class/gpio/export + direction + value）。
 * 模拟模式：仅打印日志并维护内部状态。
 */
#include "actuator.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/time.h>

static int g_sim_mode = 1;
static int g_led_gpio = -1;
static int g_beeper_gpio = -1;

static int g_led = 0;
static int g_beeper = 0;
static long long g_beeper_off_at_ms = 0;  /* 蜂鸣器自动关闭时刻（0 表示不自动关） */

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

/* 当前毫秒时间戳（CLOCK_MONOTONIC，供蜂鸣器定时关使用） */
static long long now_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (long long)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

/* ---- sysfs GPIO 基础操作 ---- */

static int gpio_path_exists(int n)
{
    char path[64];
    snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/value", n);
    return access(path, F_OK) == 0 ? 1 : 0;
}

/* 导出 GPIO（写 /sys/class/gpio/export） */
static int gpio_export(int n)
{
    FILE *f;
    if (gpio_path_exists(n)) {
        return 0;   /* 已导出 */
    }
    f = fopen("/sys/class/gpio/export", "w");
    if (!f) {
        return -1;
    }
    fprintf(f, "%d", n);
    fclose(f);
    usleep(100000);   /* 等待内核创建 gpioN 目录 */
    return gpio_path_exists(n) ? 0 : -1;
}

/* 设置 GPIO 为输出方向 */
static int gpio_direction_out(int n)
{
    char path[64];
    FILE *f;
    snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/direction", n);
    f = fopen(path, "w");
    if (!f) {
        return -1;
    }
    fprintf(f, "out");
    fclose(f);
    return 0;
}

/* 写 GPIO 电平 0/1 */
static int gpio_write(int n, int value)
{
    char path[64];
    FILE *f;
    snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/value", n);
    f = fopen(path, "w");
    if (!f) {
        return -1;
    }
    fprintf(f, "%d", value ? 1 : 0);
    fclose(f);
    return 0;
}

/* 取消 GPIO 导出 */
static void gpio_unexport(int n)
{
    FILE *f = fopen("/sys/class/gpio/unexport", "w");
    if (!f) {
        return;
    }
    fprintf(f, "%d", n);
    fclose(f);
}

/* 初始化：模拟模式仅维护内部状态；真实模式导出并配置 LED/蜂鸣 GPIO */
int actuator_init(int sim_mode, int led_gpio, int beeper_gpio)
{
    g_sim_mode = (sim_mode != 0);
    g_led_gpio = led_gpio;
    g_beeper_gpio = beeper_gpio;

    if (!g_sim_mode) {
        if (g_led_gpio >= 0 &&
            gpio_export(g_led_gpio) == 0 &&
            gpio_direction_out(g_led_gpio) == 0) {
            gpio_write(g_led_gpio, 0);
            printf("[actuator] LED 使用 GPIO%d\n", g_led_gpio);
        } else {
            printf("[actuator] 警告: LED GPIO%d 初始化失败\n", g_led_gpio);
            g_led_gpio = -1;
        }
        if (g_beeper_gpio >= 0 &&
            gpio_export(g_beeper_gpio) == 0 &&
            gpio_direction_out(g_beeper_gpio) == 0) {
            gpio_write(g_beeper_gpio, 0);
            printf("[actuator] 蜂鸣器使用 GPIO%d\n", g_beeper_gpio);
        } else {
            printf("[actuator] 警告: 蜂鸣器 GPIO%d 初始化失败\n", g_beeper_gpio);
            g_beeper_gpio = -1;
        }
    } else {
        printf("[actuator] 模拟模式: LED/蜂鸣器仅打印日志并维护内部状态\n");
    }
    return 0;
}

/* 设置 LED 开关（真实写 GPIO，模拟记录状态） */
int actuator_set_led(int on)
{
    pthread_mutex_lock(&g_lock);
    g_led = on ? 1 : 0;
    if (!g_sim_mode && g_led_gpio >= 0) {
        gpio_write(g_led_gpio, g_led);
    }
    printf("[actuator] LED -> %s\n", g_led ? "on" : "off");
    pthread_mutex_unlock(&g_lock);
    return 0;
}

/* 设置蜂鸣器：on=1 可选 duration_ms 定时自动关（真实模式） */
int actuator_set_beeper(int on, int duration_ms)
{
    pthread_mutex_lock(&g_lock);
    if (on) {
        g_beeper = 1;
        g_beeper_off_at_ms = (duration_ms > 0) ? now_ms() + duration_ms : 0;
    } else {
        g_beeper = 0;
        g_beeper_off_at_ms = 0;
    }
    if (!g_sim_mode && g_beeper_gpio >= 0) {
        gpio_write(g_beeper_gpio, g_beeper);
    }
    printf("[actuator] beeper -> %s%s\n", g_beeper ? "on" : "off",
           (on && duration_ms > 0) ? " (定时关闭)" : "");
    pthread_mutex_unlock(&g_lock);
    return 0;
}

/* 周期轮询：检查蜂鸣器定时关是否到期并执行 */
void actuator_poll(void)
{
    pthread_mutex_lock(&g_lock);
    if (g_beeper && g_beeper_off_at_ms > 0 && now_ms() >= g_beeper_off_at_ms) {
        g_beeper = 0;
        g_beeper_off_at_ms = 0;
        if (!g_sim_mode && g_beeper_gpio >= 0) {
            gpio_write(g_beeper_gpio, 0);
        }
        printf("[actuator] beeper 定时关闭\n");
    }
    pthread_mutex_unlock(&g_lock);
}

/* 查询 LED/蜂鸣当前状态 */
void actuator_get_state(int *led, int *beeper)
{
    pthread_mutex_lock(&g_lock);
    if (led) {
        *led = g_led;
    }
    if (beeper) {
        *beeper = g_beeper;
    }
    pthread_mutex_unlock(&g_lock);
}

/* 反初始化：关闭输出并取消 GPIO 导出 */
void actuator_deinit(void)
{
    if (!g_sim_mode) {
        if (g_led_gpio >= 0) {
            gpio_write(g_led_gpio, 0);
            gpio_unexport(g_led_gpio);
        }
        if (g_beeper_gpio >= 0) {
            gpio_write(g_beeper_gpio, 0);
            gpio_unexport(g_beeper_gpio);
        }
    }
}
