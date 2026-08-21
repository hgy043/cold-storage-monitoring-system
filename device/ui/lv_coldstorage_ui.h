/*
 * lv_coldstorage_ui.h - 冷库监控终端 LVGL 界面（工业 SCADA/HMI · 800x480）
 *
 * 结构：全局固定顶栏(52px) + 底部报警栏(56px) + 内容区(800x372) 5 页切换
 * 页面：主菜单 / 实时监测(多温区趋势) / 温区总览 / 设备控制(制冷流程图) / 能耗与设置
 *
 * 集成方式：
 *   1) 单独运行：ui_main.c 内置 4 温区 + 能耗模拟线程与报警事件；
 *   2) 对接 device.c：真实采集线程填充 ui_state_t 调 ui_coldstorage_update()，
 *      报警经 ui_coldstorage_alarm_add() 喂入底部报警栏；
 *      按钮动作经 ui_coldstorage_set_action_cb() 回调（led/beeper/threshold/interval）。
 */
#ifndef LV_COLDSTORAGE_UI_H
#define LV_COLDSTORAGE_UI_H

#ifdef __cplusplus
extern "C" {
#endif

/* 温区 */
typedef enum {
    ZONE_FROZEN = 0,   /* 冷冻区 -25℃ */
    ZONE_CHILLED,      /* 冷藏区 2℃ */
    ZONE_PRECOOL,      /* 预冷区 10℃ */
    ZONE_HALLWAY,      /* 穿堂 15℃ */
    ZONE_COUNT
} ui_zone_id_t;

typedef struct {
    double temperature;   /* 当前温度 ℃ */
    double humidity;      /* 相对湿度 % */
    double setpoint;      /* 设定值 ℃ */
    int    status;        /* 0=正常 1=预警 2=报警 */
} ui_zone_t;

typedef struct {
    double power_kw;      /* 实时功率 kW */
    double energy_kwh;    /* 今日耗电量 kWh */
    int    defrost;       /* 除霜状态 0/1 */
    int    condenser_fan; /* 冷凝器风扇 0/1 */
    int    door;          /* 冷库门 0=关 1=开 2=故障 */
} ui_energy_t;

typedef struct {
    double outdoor_temp;  /* 室外温度 ℃ */
    int    system_online; /* 系统在线 0/1 */
} ui_system_t;

/* 全量数据快照（一次喂全部，互斥锁保护） */
typedef struct {
    ui_zone_t   zones[ZONE_COUNT];
    ui_energy_t energy;
    ui_system_t system;
    int         led;      /* 执行器状态 */
    int         beeper;
} ui_state_t;

/* 报警等级 */
typedef enum { ALARM_MINOR = 0, ALARM_IMPORTANT, ALARM_CRITICAL } ui_alarm_level_t;

/* 页面枚举 */
typedef enum {
    UI_PAGE_HOME = 0,
    UI_PAGE_MONITOR,   /* 实时监测 */
    UI_PAGE_ZONES,     /* 温区总览 */
    UI_PAGE_CONTROL,   /* 设备控制 */
    UI_PAGE_ENERGY,    /* 能耗与设置 */
    UI_PAGE_COUNT
} ui_page_t;

/* 界面按钮动作回调：
 *   action: "led" | "beeper" | "threshold" | "interval" | "spike"
 *   value : led/beeper 0/1；threshold 新阈值℃；interval 周期秒；spike 尖峰温度 */
typedef void (*ui_action_cb_t)(void *user, const char *action, int value);

/* 创建界面（lv_init + 显示/输入驱动注册之后调用），进入主菜单页 */
int ui_coldstorage_init(const char *cold_name, const char *cold_no, double default_threshold);

/* 注册按钮动作回调 */
void ui_coldstorage_set_action_cb(ui_action_cb_t cb, void *user);

/* 线程安全更新全量数据（任意线程可调用） */
void ui_coldstorage_update(const ui_state_t *st);

/* 添加一条报警（底部报警栏滚动显示；level 0/1/2） */
void ui_coldstorage_alarm_add(int level, const char *zone, const char *desc);

/* 程序化切换页面（前进动画） */
void ui_coldstorage_show_page(ui_page_t page);

/* 报警自动跳监测页开关（默认关） */
void ui_coldstorage_set_auto_alarm_page(int enable);

#ifdef __cplusplus
}
#endif

#endif /* LV_COLDSTORAGE_UI_H */
