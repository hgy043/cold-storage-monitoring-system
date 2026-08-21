/*
 * actuator.h - LED 与蜂鸣器
 *
 * 真实模式走 sysfs GPIO；模拟模式打印日志并维护内部状态。
 */
#ifndef ACTUATOR_H
#define ACTUATOR_H

/* 初始化：sim_mode 非 0 为模拟；真实模式指定 GPIO 编号（-1 表示不使用） */
int actuator_init(int sim_mode, int led_gpio, int beeper_gpio);

/* 设置 LED：on 0/1 */
int actuator_set_led(int on);

/* 设置蜂鸣器：on 0/1；on=1 时 duration_ms 为自动关闭时长（<=0 表示常开） */
int actuator_set_beeper(int on, int duration_ms);

/* 轮询（主循环周期调用），处理蜂鸣器定时关闭 */
void actuator_poll(void);

/* 读取当前 LED/蜂鸣器状态 */
void actuator_get_state(int *led, int *beeper);

/* 释放资源（真实模式释放 GPIO） */
void actuator_deinit(void);

#endif /* ACTUATOR_H */
