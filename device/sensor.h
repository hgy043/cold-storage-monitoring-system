/*
 * sensor.h - 温湿度采集
 *
 * 真实硬件优先走 sysfs；否则按模拟模式生成围绕 -18℃ 的随机游走数据。
 */
#ifndef SENSOR_H
#define SENSOR_H

/* 初始化：sim_mode 非 0 为模拟；真实模式使用指定 sysfs 文件（可为 NULL） */
int sensor_init(int sim_mode, const char *temp_path, const char *humi_path);

/* 读取一次温湿度：temp 单位 ℃，humi 单位 %RH；失败返回 -1 */
int sensor_read(double *temp, double *humi);

/* 释放资源 */
void sensor_deinit(void);

#endif /* SENSOR_H */
