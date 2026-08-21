/*
 * lv_coldstorage_ui.c - 冷库监控终端（工业 SCADA/HMI · 800x480 · LVGL v9）
 *
 * 全局结构：
 *   ┌──────────────────────────────────────────────┐ 52px  顶栏（全局固定）
 *   │ 冷库名称/编号        时钟        室外/在线/报警 │
 *   ├──────────────────────────────────────────────┤
 *   │                                              │ 372px 内容区（5 页切换）
 *   │  主菜单 / 实时监测 / 温区总览 / 设备控制 / 能耗 │       左右滑动动画
 *   │                                              │
 *   ├──────────────────────────────────────────────┤
 *   │ 报警栏(滚动)                      [确认报警]  │ 56px  底部（全局固定）
 *   └──────────────────────────────────────────────┘
 *
 * 风格：深蓝渐变 #10264A→#050B18、卡片 #132544/#1B3358、强调科技青 #35D8FF、
 *       扁平化、无滚动、关键数值等宽粗体大字、触控按钮≥48px。
 */
#include "lv_coldstorage_ui.h"

#include "lvgl/lvgl.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <math.h>

/* 字体（均已在各自 .c 中改为 v9 非 const，便于运行时挂 fallback 链） */
extern lv_font_t lv_font_source_han_sans_bold_20;   /* 中文（仅汉字字形） */
extern lv_font_t lv_font_symbols_20;                /* ° · ℃ █ 符号 */
extern lv_font_t mono_bold;                         /* 等宽粗体数字（ASCII，32px） */
extern lv_font_t lv_font_montserrat_20;             /* 20px ASCII 数字（device/ui 本地字体，lv_conf 开关恒开） */

/* 可选 BMP 背景 */
#ifndef LV_UI_BG_BMP
#define LV_UI_BG_BMP 0
#endif
#ifndef LV_UI_BG_BMP_PATH
#define LV_UI_BG_BMP_PATH "S:/IOT/coldstorage/bg.bmp"
#endif

/* ---------- 配色 ---------- */
#define CLR_BG_TOP     0x10264a
#define CLR_BG_BOT     0x050b18
#define CLR_CARD       0x132544
#define CLR_CARD2      0x1b3358
#define CLR_LINE       0x2a4a7f
#define CLR_ACCENT     0x35d8ff
#define CLR_ACCENT_D   0x00adb5
#define CLR_TEXT       0xffffff
#define CLR_DIM        0x9fb3d1
#define CLR_GOOD       0x2ecc71
#define CLR_WARN       0xffb74d
#define CLR_BAD        0xff4d5e

/* ---------- 尺寸 ---------- */
#define SCREEN_W       800
#define SCREEN_H       480
#define TOP_H          52
#define BOT_H          56
#define CONTENT_H      (SCREEN_H - TOP_H - BOT_H)   /* 372 */
#define CHART_POINTS   60

/* 温区显示名 */
static const char *ZONE_NAMES[ZONE_COUNT] = { "冷冻区", "冷藏区", "预冷区", "穿堂" };

/* ---------- 内部状态 ---------- */
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static ui_state_t      s_state;
static ui_action_cb_t  s_action_cb = NULL;
static void           *s_action_user = NULL;
static char            s_cold_name[32] = "1号冷库";
static char            s_cold_no[16] = "NO.001";
static int             s_report_interval = 5;
static int             s_sel_zone = ZONE_FROZEN;   /* 图表聚焦温区 */

/* 温区历史（图表回填） */
#define HIST_LEN 60
static double s_hist[ZONE_COUNT][HIST_LEN];
static int    s_hist_count = 0;

/* 报警环形缓冲 */
typedef struct {
    int    level;          /* 0 一般 1 重要 2 紧急 */
    char   zone[20];
    char   desc[56];
    char   tstr[20];       /* HH:MM:SS */
    int    confirmed;
} alarm_rec_t;
#define ALARM_MAX 20
static alarm_rec_t s_alarms[ALARM_MAX];
static int         s_alarm_head = 0;    /* 写入游标 */
static int         s_alarm_count = 0;   /* 未确认总数 */
static int         s_alarm_blink = 0;

/* ---------- 全局层句柄 ---------- */
static lv_obj_t *s_lbl_time   = NULL;
static lv_obj_t *s_lbl_outdoor = NULL;
static lv_obj_t *s_lbl_online = NULL;
static lv_obj_t *s_lbl_online_dot = NULL;
static lv_obj_t *s_lbl_alarm_count = NULL;
static lv_obj_t *s_alarm_marquee = NULL;   /* 底部走马灯 */
static lv_obj_t *s_alarm_bar = NULL;       /* 底部报警栏容器 */
static lv_obj_t *s_overlay = NULL;         /* 报警详情浮层 */

/* ---------- 页面路由 ---------- */
static lv_obj_t *g_content = NULL;   /* 内容容器 800x372 */
static ui_page_t g_cur_page = UI_PAGE_HOME;
static int       s_busy = 0;
typedef enum { NAV_FWD = 0, NAV_BACK } nav_dir_t;

/* 监测页句柄 */
static lv_obj_t *s_chart = NULL;
static lv_chart_series_t *s_ser[ZONE_COUNT];
static lv_chart_series_t *s_line_alarm = NULL, *s_line_warn = NULL;
static lv_obj_t *s_zone_cards[ZONE_COUNT];   /* 下半区温区卡 */
static lv_obj_t *s_zone_tv[ZONE_COUNT];      /* 下半区温度 label */
static lv_obj_t *s_zone_dt[ZONE_COUNT];      /* 下半区湿度/设定 label */
static lv_obj_t *s_zone_sel_border[ZONE_COUNT];
/* 监测页概要区（可滑动扩展区） */
static lv_obj_t *s_mon_sum[ZONE_COUNT];       /* 概要区温度 label */
static lv_obj_t *s_mon_hum[ZONE_COUNT];       /* 概要区湿度 label */
/* 温区总览页 */
static lv_obj_t *s_zc[ZONE_COUNT];
static lv_obj_t *s_zc_border[ZONE_COUNT];
/* 控制页 */
static lv_obj_t *s_dev_dot[8];
static lv_obj_t *s_param[4];
static lv_obj_t *s_flow_dot = NULL;          /* 制冷剂流动圆点 */
/* 能耗页 */
static lv_obj_t *s_lbl_power, *s_lbl_kwh;
static lv_obj_t *s_lbl_defrost, *s_lbl_fan, *s_lbl_door;
static lv_obj_t *s_lbl_th, *s_lbl_iv;
static lv_obj_t *s_zc_status_dot[ZONE_COUNT];
static lv_obj_t *s_lbl_defrost_dot, *s_lbl_fan_dot, *s_lbl_door_dot, *s_lbl_led_dot;
static lv_obj_t *s_lbl_led;

static void refresh_timer_cb(lv_timer_t *timer);
/* 构建指定页面：重置所有句柄 → 新建可滚动内容容器 → 按页分派构建函数 */
static void build_page_content(ui_page_t page);
/* 页面切换：新页从侧滑入、旧页滑出后删除；s_busy 防止动画期间重复切页 */
static void switch_page(ui_page_t page, nav_dir_t dir);

/* ---------- 小工具 ---------- */
static void set_label_text(lv_obj_t *lbl, const char *fmt, ...)
{
    char buf[128];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (lbl) lv_label_set_text(lbl, buf);
}
/* 设置状态圆点颜色（背景+边框同色） */
static void set_dot_color(lv_obj_t *dot, uint32_t color)
{
    if (dot) {
        lv_obj_set_style_bg_color(dot, lv_color_hex(color), 0);
        lv_obj_set_style_border_color(dot, lv_color_hex(color), 0);
    }
}
/* 状态(0正常/1预警/2报警) → 颜色；数据源不可信时 clamp，防数组越界 */
static uint32_t status_color(int st)
{
    static const uint32_t c[3] = { CLR_GOOD, CLR_WARN, CLR_BAD };
    if (st < 0) st = 0;
    if (st > 2) st = 2;
    return c[st];
}
/* 温度格式化：离线/无数据时显示占位符 --.-，避免把 0.0 当真实温度 */
static void set_temp_text(lv_obj_t *lbl, int online, double t)
{
    if (lbl) {
        if (online) set_label_text(lbl, "%.1f", t);
        else        lv_label_set_text(lbl, "--.-");
    }
}
/* 卡片通用样式：深色渐变底 + 细边框 + 圆角，禁止滚动 */
static void panel_style(lv_obj_t *o, int radius)
{
    lv_obj_set_style_bg_color(o, lv_color_hex(CLR_CARD), 0);
    lv_obj_set_style_bg_grad_color(o, lv_color_hex(CLR_CARD2), 0);
    lv_obj_set_style_bg_grad_dir(o, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_border_width(o, 1, 0);
    lv_obj_set_style_border_color(o, lv_color_hex(CLR_LINE), 0);
    lv_obj_set_style_radius(o, radius, 0);
    lv_obj_set_style_shadow_width(o, 0, 0);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
}
static lv_obj_t *create_card(lv_obj_t *parent, int x, int y, int w, int h, int radius)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_size(card, w, h);
    lv_obj_align(card, LV_ALIGN_TOP_LEFT, x, y);
    panel_style(card, radius);
    return card;
}
static lv_obj_t *create_label(lv_obj_t *parent, const char *text,
                              const lv_font_t *font, uint32_t color)
{
    lv_obj_t *lbl = lv_label_create(parent);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, font, 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(color), 0);
    return lbl;
}
static lv_obj_t *create_dot(lv_obj_t *parent, int x, int y, int r, uint32_t color)
{
    lv_obj_t *dot = lv_obj_create(parent);
    lv_obj_set_size(dot, r * 2, r * 2);
    lv_obj_align(dot, LV_ALIGN_TOP_LEFT, x, y);
    lv_obj_set_style_bg_color(dot, lv_color_hex(color), 0);
    lv_obj_set_style_border_width(dot, 2, 0);
    lv_obj_set_style_border_color(dot, lv_color_hex(color), 0);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_clear_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
    return dot;
}
/* ---------- 按钮（动作参数池） ---------- */
typedef struct { const char *action; int value; } btn_arg_t;
#define BTN_ARG_POOL 64
static btn_arg_t s_btn_args[BTN_ARG_POOL];
static int       s_btn_arg_idx = 0;
/* 底栏常驻按钮（取消报警）用独立槽：页面按钮池 64 槽循环复用，约十几次切页后
 * 会回绕覆盖槽 0，导致常驻按钮被劫持执行别的动作（切页/调阈值）。 */
static btn_arg_t s_ack_arg = { "confirm_alarm", 0 };

/* 按钮动作分发：page/back 切页；threshold/interval 更新本地状态并通知回调；其余动作回调 on_action */
static void btn_event_cb(lv_event_t *e)
{
    btn_arg_t *arg = (btn_arg_t *)lv_event_get_user_data(e);
    if (!arg || !arg->action) return;

    if (strcmp(arg->action, "page") == 0) { switch_page((ui_page_t)arg->value, NAV_FWD); return; }
    if (strcmp(arg->action, "back") == 0)  { switch_page(UI_PAGE_HOME, NAV_BACK); return; }

    int new_value = arg->value;
    if (strcmp(arg->action, "threshold") == 0) {
        pthread_mutex_lock(&s_lock);
        new_value = (int)s_state.zones[ZONE_FROZEN].setpoint + arg->value; /* 阈值=冷冻区设定联动 */
        pthread_mutex_unlock(&s_lock);
    }
    if (strcmp(arg->action, "interval") == 0) new_value = arg->value;

    /* threshold/interval：本地状态立即更新并刷新标签（UI 反馈），同时通知回调 */
    if (strcmp(arg->action, "threshold") == 0 || strcmp(arg->action, "interval") == 0) {
        pthread_mutex_lock(&s_lock);
        if (strcmp(arg->action, "threshold") == 0) s_state.zones[ZONE_FROZEN].setpoint = (double)new_value;
        else s_report_interval = arg->value;
        pthread_mutex_unlock(&s_lock);
        printf("[UI] 按钮: %s = %d\n", arg->action, new_value);
    }
    if (!s_action_cb) {
        pthread_mutex_lock(&s_lock);
        if (strcmp(arg->action, "led") == 0) s_state.led = arg->value;
        else if (strcmp(arg->action, "beeper") == 0) s_state.beeper = arg->value;
        else if (strcmp(arg->action, "threshold") == 0) { /* 已更新 */ }
        else if (strcmp(arg->action, "interval") == 0) { /* 已更新 */ }
        else { printf("[UI] 未注册回调, 忽略动作: %s = %d\n", arg->action, new_value); pthread_mutex_unlock(&s_lock); return; }
        pthread_mutex_unlock(&s_lock);
        return;
    }
    s_action_cb(s_action_user, arg->action, new_value);
}

static lv_obj_t *create_btn(lv_obj_t *parent, const char *text,
                            int x, int y, int w, int h,
                            uint32_t bg, uint32_t bg2,
                            const char *action, int value)
{
    btn_arg_t *arg = &s_btn_args[s_btn_arg_idx % BTN_ARG_POOL];
    s_btn_arg_idx++;
    arg->action = action;
    arg->value  = value;

    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_size(btn, w, h);
    lv_obj_align(btn, LV_ALIGN_TOP_LEFT, x, y);
    lv_obj_set_style_bg_color(btn, lv_color_hex(bg), 0);
    lv_obj_set_style_bg_grad_color(btn, lv_color_hex(bg2), 0);
    lv_obj_set_style_bg_grad_dir(btn, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_radius(btn, 10, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_add_event_cb(btn, btn_event_cb, LV_EVENT_CLICKED, arg);
    lv_obj_t *lbl = create_label(btn, text, &lv_font_source_han_sans_bold_20, CLR_TEXT);
    lv_obj_center(lbl);
    return btn;
}

/* 主菜单大卡片（图标符号 + 标题 + 副标题） */
static lv_obj_t *create_menu_card(lv_obj_t *parent, const char *title, const char *sub,
                                  int x, int y, int w, int h,
                                  uint32_t c1, uint32_t c2,
                                  const char *action, int value)
{
    btn_arg_t *arg = &s_btn_args[s_btn_arg_idx % BTN_ARG_POOL];
    s_btn_arg_idx++;
    arg->action = action;
    arg->value  = value;

    lv_obj_t *card = lv_btn_create(parent);
    lv_obj_set_size(card, w, h);
    lv_obj_align(card, LV_ALIGN_TOP_LEFT, x, y);
    lv_obj_set_style_bg_color(card, lv_color_hex(c1), 0);
    lv_obj_set_style_bg_grad_color(card, lv_color_hex(c2), 0);
    lv_obj_set_style_bg_grad_dir(card, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_radius(card, 14, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(CLR_LINE), 0);
    lv_obj_set_style_shadow_width(card, 0, 0);
    lv_obj_add_event_cb(card, btn_event_cb, LV_EVENT_CLICKED, arg);

    /* 标题 + 副标题（无装饰条，消除文字上方的横杠） */
    lv_obj_t *t = create_label(card, title, &lv_font_source_han_sans_bold_20, CLR_TEXT);
    lv_obj_align(t, LV_ALIGN_TOP_LEFT, 20, 40);
    lv_obj_t *s = create_label(card, sub, &lv_font_source_han_sans_bold_20, CLR_DIM);
    lv_obj_align(s, LV_ALIGN_TOP_LEFT, 20, 76);
    return card;
}

/* ---------- 全局层：顶栏 ---------- */
static void build_topbar(void)
{
    lv_obj_t *bar = lv_obj_create(lv_scr_act());
    lv_obj_set_size(bar, SCREEN_W, TOP_H);
    lv_obj_align(bar, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x0a1830), 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    /* 底部青色细线 */
    lv_obj_t *line = lv_obj_create(bar);
    lv_obj_set_size(line, SCREEN_W, 2);
    lv_obj_align(line, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_set_style_bg_color(line, lv_color_hex(CLR_ACCENT_D), 0);
    lv_obj_clear_flag(line, LV_OBJ_FLAG_SCROLLABLE);

    /* 左：冷库名称 + 编号 */
    lv_obj_t *nm = create_label(bar, s_cold_name, &lv_font_source_han_sans_bold_20, CLR_TEXT);
    lv_obj_align(nm, LV_ALIGN_TOP_LEFT, 8, 12);
    lv_obj_t *no = create_label(bar, s_cold_no, &lv_font_source_han_sans_bold_20, CLR_DIM);
    lv_obj_align(no, LV_ALIGN_TOP_LEFT, 96, 14);

    /* 中：时钟 */
    s_lbl_time = create_label(bar, "--:--:--", &mono_bold, CLR_TEXT);
    lv_obj_align(s_lbl_time, LV_ALIGN_TOP_MID, -160, 8);

    /* 右：室外温度 / 在线 / 报警总数 */
    s_lbl_outdoor = create_label(bar, "室外 --.-", &lv_font_source_han_sans_bold_20, CLR_TEXT);
    lv_obj_align(s_lbl_outdoor, LV_ALIGN_TOP_RIGHT, -300, 14);
    s_lbl_online_dot = create_dot(bar, 0, 0, 6, CLR_DIM);
    lv_obj_align(s_lbl_online_dot, LV_ALIGN_TOP_RIGHT, -200, 21);
    s_lbl_online = create_label(bar, "在线", &lv_font_source_han_sans_bold_20, CLR_TEXT);
    lv_obj_align_to(s_lbl_online, s_lbl_online_dot, LV_ALIGN_OUT_RIGHT_MID, 8, 0);
    s_lbl_alarm_count = create_label(bar, "报警 0", &lv_font_source_han_sans_bold_20, CLR_TEXT);
    lv_obj_align(s_lbl_alarm_count, LV_ALIGN_TOP_RIGHT, -8, 14);
}

/* ---------- 全局层：底部报警栏与报警浮层 ---------- */
static void alarm_overlay_build(void);

/* 关闭报警详情浮层（删除对象并置空句柄） */
static void alarm_overlay_close(void)
{
    if (s_overlay) {
        lv_obj_del(s_overlay);
        s_overlay = NULL;
    }
}

/* 报警浮层：点击条目"确认" */
static void alarm_row_confirm_cb(lv_event_t *e)
{
    int idx = (int)(long)lv_event_get_user_data(e);
    if (idx < 0 || idx >= ALARM_MAX) return;
    pthread_mutex_lock(&s_lock);
    s_alarms[idx].confirmed = 1;
    int un = 0;
    for (int i = 0; i < ALARM_MAX; i++) {
        if (!s_alarms[i].confirmed) un++;
    }
    s_alarm_count = un;
    pthread_mutex_unlock(&s_lock);
    alarm_overlay_build();
}

static void alarm_overlay_build(void)
{
    alarm_overlay_close();
    s_overlay = lv_obj_create(lv_scr_act());
    lv_obj_set_size(s_overlay, SCREEN_W, CONTENT_H);
    lv_obj_align(s_overlay, LV_ALIGN_TOP_LEFT, 0, TOP_H);
    lv_obj_set_style_bg_color(s_overlay, lv_color_hex(0x0a1830), 0);
    lv_obj_set_style_border_width(s_overlay, 0, 0);
    lv_obj_set_style_radius(s_overlay, 0, 0);
    lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *t = create_label(s_overlay, "报警详情 (点击 确认 逐条处理)", &lv_font_source_han_sans_bold_20, CLR_ACCENT);
    lv_obj_align(t, LV_ALIGN_TOP_LEFT, 8, 10);

    /* 关闭按钮 */
    lv_obj_t *close_btn = lv_btn_create(s_overlay);
    lv_obj_set_size(close_btn, 96, 36);
    lv_obj_align(close_btn, LV_ALIGN_TOP_RIGHT, -8, 8);
    lv_obj_set_style_bg_color(close_btn, lv_color_hex(0x2a3a66), 0);
    lv_obj_set_style_radius(close_btn, 8, 0);
    lv_obj_set_style_border_width(close_btn, 0, 0);
    lv_obj_add_event_cb(close_btn, (lv_event_cb_t)alarm_overlay_close, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cl = create_label(close_btn, "关闭", &lv_font_source_han_sans_bold_20, CLR_TEXT);
    lv_obj_center(cl);

    /* 报警列表：可上下滚动的列表容器（未确认优先，最多显示全部 ALARM_MAX 条） */
    lv_obj_t *list = lv_obj_create(s_overlay);
    lv_obj_set_size(list, SCREEN_W - 16, CONTENT_H - 58);
    lv_obj_align(list, LV_ALIGN_TOP_LEFT, 8, 52);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_radius(list, 0, 0);
    lv_obj_set_style_pad_all(list, 0, 0);
    lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_AUTO);
    char line[160];
    int shown = 0;
    for (int pass = 0; pass < 2 && shown < ALARM_MAX; pass++) {
        pthread_mutex_lock(&s_lock);
        for (int k = 0; k < ALARM_MAX && shown < ALARM_MAX; k++) {
            int idx = s_alarm_head - 1 - k;
            while (idx < 0) idx += ALARM_MAX;
            alarm_rec_t *rec = &s_alarms[idx];
            if (rec->level == 0 && rec->zone[0] == '\0') continue;      /* 空槽 */
            int want_un = (pass == 0);
            if ((rec->confirmed ? 0 : 1) != want_un) continue;
            shown++;
            const char *lv_name = rec->level >= 2 ? "紧急" : (rec->level == 1 ? "重要" : "一般");
            snprintf(line, sizeof(line), "%s [%s] %s  %s",
                     rec->tstr, lv_name, rec->zone, rec->desc);
            pthread_mutex_unlock(&s_lock);

            int y = (shown - 1) * 38;
            lv_obj_t *row = lv_obj_create(list);
            lv_obj_set_size(row, SCREEN_W - 16, 30);
            lv_obj_align(row, LV_ALIGN_TOP_LEFT, 0, y);
            lv_obj_set_style_pad_all(row, 0, 0);
            lv_obj_set_style_bg_color(row, lv_color_hex(0x0f1e38), 0);
            lv_obj_set_style_border_width(row, 0, 0);
            lv_obj_set_style_radius(row, 6, 0);
            lv_obj_set_style_clip_corner(row, true, 0);   /* 裁剪超长文本，避免与下行重叠 */
            lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

            lv_obj_t *lt = create_label(row, line, &lv_font_source_han_sans_bold_20,
                                        rec->level >= 2 ? CLR_BAD : (rec->level == 1 ? CLR_WARN : CLR_TEXT));
            lv_obj_set_width(lt, SCREEN_W - 16 - 80);      /* 限宽，留出右侧确认按钮 */
            lv_obj_align(lt, LV_ALIGN_TOP_LEFT, 10, 4);

            lv_obj_t *ok = lv_btn_create(row);
            lv_obj_set_size(ok, 56, 24);
            lv_obj_align(ok, LV_ALIGN_TOP_RIGHT, -8, 3);
            lv_obj_set_style_bg_color(ok, lv_color_hex(0x1e7f4f), 0);
            lv_obj_set_style_radius(ok, 6, 0);
            lv_obj_set_style_border_width(ok, 0, 0);
            lv_obj_add_event_cb(ok, alarm_row_confirm_cb, LV_EVENT_CLICKED, (void *)(long)idx);
            lv_obj_t *ol = create_label(ok, "确认", &lv_font_source_han_sans_bold_20, CLR_TEXT);
            lv_obj_center(ol);
            pthread_mutex_lock(&s_lock);
        }
        pthread_mutex_unlock(&s_lock);
    }
    if (shown == 0) {
        lv_obj_t *empty = create_label(list, "当前无未处理报警", &lv_font_source_han_sans_bold_20, CLR_DIM);
        lv_obj_align(empty, LV_ALIGN_TOP_LEFT, 8, 10);
    }
}

/* 打开报警详情浮层（每次重建列表） */
static void alarm_overlay_open(lv_event_t *e)
{
    (void)e;
    alarm_overlay_build();
}

/* 底部报警栏 */
static void build_bottombar(void)
{
    s_alarm_bar = lv_obj_create(lv_scr_act());
    lv_obj_set_size(s_alarm_bar, SCREEN_W, BOT_H);
    lv_obj_align(s_alarm_bar, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_set_style_bg_color(s_alarm_bar, lv_color_hex(0x0a1830), 0);
    lv_obj_set_style_border_width(s_alarm_bar, 0, 0);
    lv_obj_set_style_radius(s_alarm_bar, 0, 0);
    lv_obj_set_style_pad_all(s_alarm_bar, 0, 0);
    lv_obj_clear_flag(s_alarm_bar, LV_OBJ_FLAG_SCROLLABLE);
    /* 顶部青色细线 */
    lv_obj_t *line = lv_obj_create(s_alarm_bar);
    lv_obj_set_size(line, SCREEN_W, 2);
    lv_obj_align(line, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(line, lv_color_hex(CLR_ACCENT_D), 0);
    lv_obj_clear_flag(line, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *tag = create_label(s_alarm_bar, "报警", &lv_font_source_han_sans_bold_20, CLR_TEXT);
    lv_obj_set_style_bg_color(tag, lv_color_hex(CLR_BAD), 0);
    lv_obj_set_style_radius(tag, 6, 0);
    lv_obj_set_style_pad_all(tag, 6, 0);
    lv_obj_align(tag, LV_ALIGN_TOP_LEFT, 8, 12);

    /* 走马灯（最新报警）：裁剪容器 + 自动宽度 label，超宽时循环滚动 */
    lv_obj_t *mclip = lv_obj_create(s_alarm_bar);
    lv_obj_set_size(mclip, 540, 32);
    lv_obj_align(mclip, LV_ALIGN_TOP_LEFT, 80, 12);
    lv_obj_set_style_bg_opa(mclip, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(mclip, 0, 0);
    lv_obj_set_style_radius(mclip, 0, 0);
    lv_obj_set_style_clip_corner(mclip, true, 0);
    lv_obj_clear_flag(mclip, LV_OBJ_FLAG_SCROLLABLE);
    s_alarm_marquee = create_label(mclip, "无报警", &lv_font_source_han_sans_bold_20, CLR_DIM);
    /* 垂直居中：与右侧"确认报警"按钮文字（center）同高，避免滚动行视觉偏下 */
    lv_obj_align(s_alarm_marquee, LV_ALIGN_LEFT_MID, 0, 0);

    /* 确认报警 → 巡查人员到场后停止蜂鸣（回调到终端执行 /dev/beep 关闭 + 上报 confirm） */
    lv_obj_t *ackbtn = lv_btn_create(s_alarm_bar);
    lv_obj_set_size(ackbtn, 110, 36);
    lv_obj_align(ackbtn, LV_ALIGN_TOP_RIGHT, -134, 10);
    lv_obj_set_style_bg_color(ackbtn, lv_color_hex(0x1e7f4f), 0);
    lv_obj_set_style_radius(ackbtn, 8, 0);
    lv_obj_set_style_border_width(ackbtn, 0, 0);
    lv_obj_add_event_cb(ackbtn, btn_event_cb, LV_EVENT_CLICKED, &s_ack_arg);
    lv_obj_t *abl = create_label(ackbtn, "取消报警", &lv_font_source_han_sans_bold_20, CLR_TEXT);
    lv_obj_center(abl);

    /* 报警详情 → 报警浮层 */
    lv_obj_t *btn = lv_btn_create(s_alarm_bar);
    lv_obj_set_size(btn, 110, 36);
    lv_obj_align(btn, LV_ALIGN_TOP_RIGHT, -8, 10);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x2a3a66), 0);
    lv_obj_set_style_radius(btn, 8, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_add_event_cb(btn, alarm_overlay_open, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bl = create_label(btn, "报警详情", &lv_font_source_han_sans_bold_20, CLR_TEXT);
    lv_obj_center(bl);
}

/* ---------- 顶栏 ---------- */
static void create_topbar(const char *title, int with_back)
{
    /* 内容页顶栏：页标题 + 返回（位于内容容器内） */
    lv_obj_t *bar = lv_obj_create(g_content);
    lv_obj_set_size(bar, SCREEN_W, 40);
    lv_obj_align(bar, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x0d1c36), 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *t = create_label(bar, title, &lv_font_source_han_sans_bold_20, CLR_ACCENT);
    lv_obj_align(t, LV_ALIGN_TOP_LEFT, 8, 9);

    if (with_back) {
        create_btn(bar, "返回", 700, 4, 92, 32, 0x1b3358, 0x2a4a7f, "back", 0);
    }
}
/* ---------- 主菜单页 ---------- */
static void build_home(void)
{
    /* 主菜单：2x2 功能入口卡片铺满内容区（已去除区划图） */
    create_menu_card(g_content, "实时监测", "趋势图/当前值", 0, 0, 396, 182,
                     0x154a66, 0x1b3358, "page", UI_PAGE_MONITOR);
    create_menu_card(g_content, "温区总览", "4 温区状态", 404, 0, 396, 182,
                     0x154a66, 0x1b3358, "page", UI_PAGE_ZONES);
    create_menu_card(g_content, "设备控制", "制冷流程图", 0, 190, 396, 182,
                     0x2a3a66, 0x1b3358, "page", UI_PAGE_CONTROL);
    create_menu_card(g_content, "能耗与设置", "功率/参数", 404, 190, 396, 182,
                     0x2a3a66, 0x1b3358, "page", UI_PAGE_ENERGY);
}

/* ---------- 监测页：图例筛选（点击切换该温区曲线显隐） ---------- */
static void legend_cb(lv_event_t *e)
{
    int z = (int)(long)lv_event_get_user_data(e);
    if (z < 0 || z >= ZONE_COUNT) return;
    s_sel_zone = z;
    if (s_ser[z]) {
        s_ser[z]->hidden = !s_ser[z]->hidden;
        lv_chart_refresh(s_chart);
    }
}
/* 温区卡点击：只切换选中高亮，不改变曲线显隐（避免曲线"意外消失"） */
static void zone_select_cb(lv_event_t *e)
{
    int z = (int)(long)lv_event_get_user_data(e);
    if (z < 0 || z >= ZONE_COUNT) return;
    s_sel_zone = z;
}

/* ---------- 实时监测页 ---------- */
static void build_monitor(void)
{
    create_topbar("实时监测", 1);

    /* 上半：24h 多温区趋势 */
    lv_obj_t *cc = create_card(g_content, 0, 40, 800, 357, 14);
    lv_obj_t *ct = create_label(cc, "温区温度趋势 (滚动)", &lv_font_source_han_sans_bold_20, CLR_DIM);
    lv_obj_align(ct, LV_ALIGN_TOP_LEFT, 12, 8);

    s_chart = lv_chart_create(cc);
    lv_obj_set_size(s_chart, 760, 237);
    lv_obj_align(s_chart, LV_ALIGN_TOP_LEFT, 20, 60);
    lv_chart_set_type(s_chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(s_chart, CHART_POINTS);
    lv_chart_set_range(s_chart, LV_CHART_AXIS_PRIMARY_Y, -30, 25);
    lv_chart_set_div_line_count(s_chart, 5, 8);
    /* 坐标轴刻度标签：Y 轴温度(℃)，X 轴时间 */
    lv_chart_set_axis_tick(s_chart, LV_CHART_AXIS_PRIMARY_Y, 10, 5, 6, 6, true, 30);
    lv_chart_set_axis_tick(s_chart, LV_CHART_AXIS_PRIMARY_X, 10, 5, 6, 6, true, 40);
    lv_obj_set_style_bg_color(s_chart, lv_color_hex(0x0a1830), 0);
    lv_obj_set_style_border_width(s_chart, 0, 0);
    lv_obj_set_style_radius(s_chart, 8, 0);
    lv_obj_clear_flag(s_chart, LV_OBJ_FLAG_SCROLLABLE);

    uint32_t line_c[ZONE_COUNT] = { 0x35d8ff, 0x2ecc71, 0xffb74d, 0xff9f43 };
    for (int z = 0; z < ZONE_COUNT; z++) {
        s_ser[z] = lv_chart_add_series(s_chart, lv_color_hex(line_c[z]), LV_CHART_AXIS_PRIMARY_Y);
    }
    /* 阈值参考线（v9 series 无 per-series 线宽/虚线，用实线颜色区分）：
     * 红 = 报警触发线(设定+4℃)，黄 = 预警触发线(设定+2℃)，与 zoneStatus 判定一致；
     * 阈值随云端 z1_s 动态变化，刷新时持续推点保持恒线 */
    pthread_mutex_lock(&s_lock);
    double sp0 = s_state.zones[ZONE_FROZEN].setpoint;
    pthread_mutex_unlock(&s_lock);
    s_line_alarm = lv_chart_add_series(s_chart, lv_color_hex(CLR_BAD), LV_CHART_AXIS_PRIMARY_Y);
    s_line_warn  = lv_chart_add_series(s_chart, lv_color_hex(CLR_WARN), LV_CHART_AXIS_PRIMARY_Y);
    for (int i = 0; i < CHART_POINTS; i++) {
        lv_chart_set_next_value(s_chart, s_line_alarm, (lv_coord_t)lround(sp0 + 4));
        lv_chart_set_next_value(s_chart, s_line_warn, (lv_coord_t)lround(sp0 + 2));
    }

    /* 图例（可筛选：点击聚焦该温区） */
    const char *legend_names[ZONE_COUNT] = { "冷冻", "冷藏", "预冷", "穿堂" };
    for (int z = 0; z < ZONE_COUNT; z++) {
        lv_obj_t *lb = lv_btn_create(cc);
        lv_obj_set_size(lb, 64, 28);
        lv_obj_align(lb, LV_ALIGN_TOP_RIGHT, -(12 + z * 70), 4);
        lv_obj_set_style_bg_color(lb, lv_color_hex(0x1b3358), 0);
        lv_obj_set_style_radius(lb, 6, 0);
        lv_obj_set_style_border_width(lb, 0, 0);
        lv_obj_add_event_cb(lb, legend_cb, LV_EVENT_CLICKED, (void *)(long)z);
        lv_obj_t *lt = create_label(lb, legend_names[z], &lv_font_source_han_sans_bold_20, CLR_TEXT);
        lv_obj_center(lt);
    }

    /* 回填历史（环形缓冲：从写游标倒推最近 n 点，保证时间顺序） */
    pthread_mutex_lock(&s_lock);
    for (int z = 0; z < ZONE_COUNT; z++) {
        int n = (s_hist_count < HIST_LEN) ? s_hist_count : HIST_LEN;
        for (int i = 0; i < n; i++) {
            int idx = (s_hist_count - n + i) % HIST_LEN;
            lv_chart_set_next_value(s_chart, s_ser[z], (lv_coord_t)s_hist[z][idx]);
        }
    }
    pthread_mutex_unlock(&s_lock);

    /* 下半：4 温区当前值卡（点击选中，温度/湿度句柄保存供刷新） */
    for (int z = 0; z < ZONE_COUNT; z++) {
        lv_obj_t *zc = create_card(g_content, 6 + z * 197, 403, 190, 189, 10);
        s_zone_cards[z] = zc;
        s_zone_sel_border[z] = zc;
        lv_obj_t *nm = create_label(zc, ZONE_NAMES[z], &lv_font_source_han_sans_bold_20, CLR_TEXT);
        lv_obj_align(nm, LV_ALIGN_TOP_LEFT, 10, 8);
        lv_obj_t *tv = create_label(zc, "--.-", &mono_bold, CLR_TEXT);
        lv_obj_align(tv, LV_ALIGN_TOP_LEFT, 10, 60);
        s_zone_tv[z] = tv;
        lv_obj_t *dt = create_label(zc, "湿度 -- 设定 --", &lv_font_source_han_sans_bold_20, CLR_DIM);
        lv_obj_align(dt, LV_ALIGN_TOP_LEFT, 10, 140);
        s_zone_dt[z] = dt;
        /* 选中卡可点击（只选中，不切换曲线显隐） */
        lv_obj_add_flag(zc, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(zc, zone_select_cb, LV_EVENT_CLICKED, (void *)(long)z);
    }
    s_sel_zone = ZONE_FROZEN;

    /* 可滑动扩展区：24h 温区概要（与上方温区卡对齐，高度充足） */
    lv_obj_t *sum = create_card(g_content, 0, 598, 800, 300, 14);
    lv_obj_t *st_ = create_label(sum, "24h 温区概要", &lv_font_source_han_sans_bold_20, CLR_DIM);
    lv_obj_align(st_, LV_ALIGN_TOP_LEFT, 12, 8);
    for (int z = 0; z < ZONE_COUNT; z++) {
        /* 概要内卡：3 行完整数据（名称/温度/湿度/设定），模块做大可滚动 */
        lv_obj_t *rc = create_card(sum, 6 + z * 197, 44, 190, 210, 8);
        lv_obj_t *rn = create_label(rc, ZONE_NAMES[z], &lv_font_source_han_sans_bold_20, CLR_TEXT);
        lv_obj_align(rn, LV_ALIGN_TOP_LEFT, 12, 8);
        lv_obj_t *rv = create_label(rc, "--.-°C", &lv_font_source_han_sans_bold_20, CLR_ACCENT);
        lv_obj_align(rv, LV_ALIGN_TOP_LEFT, 12, 60);
        lv_obj_t *rh = create_label(rc, "湿度 --%", &lv_font_source_han_sans_bold_20, CLR_DIM);
        lv_obj_align(rh, LV_ALIGN_TOP_LEFT, 12, 110);
        lv_obj_t *rs = create_label(rc, "设定 --°C", &lv_font_source_han_sans_bold_20, CLR_DIM);
        lv_obj_align(rs, LV_ALIGN_TOP_LEFT, 12, 160);
        s_mon_sum[z] = rv;
        s_mon_hum[z] = rh;
    }
}
/* ---------- 温区总览页 ---------- */
static void build_zones(void)
{
    create_topbar("温区总览", 1);
    for (int z = 0; z < ZONE_COUNT; z++) {
        int zx = 8 + (z % 2) * 392;
        int zy = 48 + (z / 2) * 243;
        lv_obj_t *zc = create_card(g_content, zx, zy, 384, 231, 14);
        s_zc[z] = zc;
        s_zc_border[z] = zc;
        lv_obj_t *nm = create_label(zc, ZONE_NAMES[z], &lv_font_source_han_sans_bold_20, CLR_ACCENT);
        lv_obj_align(nm, LV_ALIGN_TOP_LEFT, 16, 12);
        lv_obj_t *tv = create_label(zc, "--.-", &mono_bold, CLR_TEXT);
        lv_obj_align(tv, LV_ALIGN_TOP_LEFT, 16, 80);
        lv_obj_set_width(tv, 120);   /* 固定宽度，负数值不挤压右侧 */
        lv_obj_t *un = create_label(zc, "°C", &lv_font_montserrat_20, CLR_DIM);
        lv_obj_align(un, LV_ALIGN_TOP_LEFT, 140, 82);   /* 固定位置，不与负数重叠 */
        lv_obj_t *dt = create_label(zc, "湿度 --.-%   设定 --.-℃", &lv_font_source_han_sans_bold_20, CLR_DIM);
        lv_obj_align(dt, LV_ALIGN_TOP_LEFT, 16, 170);
        /* 状态灯（右上） */
        lv_obj_t *dot = create_dot(zc, 0, 0, 8, CLR_DIM);
        lv_obj_align(dot, LV_ALIGN_TOP_RIGHT, -20, 12);
        s_zc_status_dot[z] = dot;
    }

    /* 可滑动扩展区：状态说明（内容超出容器高度可上下滑动） */
    lv_obj_t *note = create_card(g_content, 0, 534, 800, 215, 14);
    lv_obj_t *nt = create_label(note, "状态说明(滑动查看)", &lv_font_source_han_sans_bold_20, CLR_DIM);
    lv_obj_align(nt, LV_ALIGN_TOP_LEFT, 12, 8);
    const char *notes[] = {
        "· 绿色: 温度正常(偏差 <2℃)",
        "· 黄色: 温度预警(偏差 2-4℃)",
        "· 红色: 温度报警(偏差 >4℃), 蜂鸣器响",
        "· 设定值: 各温区目标温度, 可在线调整",
    };
    for (int i = 0; i < 4; i++) {
        lv_obj_t *nlab = create_label(note, notes[i], &lv_font_source_han_sans_bold_20, CLR_TEXT);
        lv_obj_align(nlab, LV_ALIGN_TOP_LEFT, 16, 44 + i * 34);
    }
}

/* ---------- 设备控制页：制冷流程图 ---------- */
static int s_flow_seg = 0;
/* 制冷剂循环环线（压缩机→冷凝器→膨胀阀→蒸发器→压缩机） */
static const int FLOW_PATH[4][4] = {
    { 130, 215, 400, 95 },
    { 400, 95, 670, 215 },
    { 670, 215, 400, 263 },
    { 400, 263, 130, 215 },
};
static void flow_exec_x(void *var, int32_t v) { lv_obj_set_x((lv_obj_t *)var, v); }
static void flow_exec_y(void *var, int32_t v) { lv_obj_set_y((lv_obj_t *)var, v); }
/* 制冷剂流动圆点：沿管线 FLOW_PATH 逐段做线性动画，段间 ready 回调链衔接 */
static void flow_next(lv_anim_t *a)
{
    (void)a;
    if (!s_flow_dot) return;
    int seg = s_flow_seg % 4;
    int x0 = FLOW_PATH[seg][0], y0 = FLOW_PATH[seg][1];
    int x1 = FLOW_PATH[seg][2], y1 = FLOW_PATH[seg][3];
    lv_anim_t ax, ay;
    lv_anim_init(&ax);
    lv_anim_set_var(&ax, s_flow_dot);
    lv_anim_set_exec_cb(&ax, flow_exec_x);
    lv_anim_set_values(&ax, x0, x1);
    lv_anim_set_time(&ax, 850);
    lv_anim_set_path_cb(&ax, lv_anim_path_linear);
    lv_anim_set_ready_cb(&ax, flow_next);
    lv_anim_start(&ax);
    lv_anim_init(&ay);
    lv_anim_set_var(&ay, s_flow_dot);
    lv_anim_set_exec_cb(&ay, flow_exec_y);
    lv_anim_set_values(&ay, y0, y1);
    lv_anim_set_time(&ay, 850);
    lv_anim_set_path_cb(&ay, lv_anim_path_linear);
    lv_anim_start(&ay);
    s_flow_seg++;
}

/* 设备控制页：制冷流程动画图 + 6 个设备状态点 + 频率/电流/排气/吸气参数条 + 流程说明 */
static void build_control(void)
{
    create_topbar("设备控制", 1);

    /* 流程图卡 */
    lv_obj_t *fc = create_card(g_content, 0, 40, 800, 332, 14);
    lv_obj_t *ft = create_label(fc, "制冷系统流程", &lv_font_source_han_sans_bold_20, CLR_DIM);
    lv_obj_align(ft, LV_ALIGN_TOP_LEFT, 14, 8);

    /* 管线（青色细线环） */
    static lv_point_t pipe_pts[] = { {130,215},{400,95},{670,215},{400,263},{130,215} };
    lv_obj_t *pipe = lv_line_create(fc);
    lv_line_set_points(pipe, pipe_pts, 5);
    lv_obj_set_style_line_width(pipe, 2, 0);
    lv_obj_set_style_line_color(pipe, lv_color_hex(0x2a5a8f), 0);
    lv_obj_set_style_line_rounded(pipe, 0, 0);

    /* 设备方块 + 状态点 */
    const char *dev_names[6] = { "压缩机", "冷凝器", "膨胀阀", "蒸发器", "除霜加热", "冷风机" };
    const int dev_xy[6][4] = {
        /* 主循环：压缩机(左下) 冷凝器(顶中) 膨胀阀(右下) 蒸发器(底中) */
        { 40, 180, 180, 70 }, { 310, 60, 180, 70 }, { 580, 180, 180, 70 },
        { 310, 238, 180, 50 },
        /* 辅助：冷风机(左下旁) 除霜加热(右下旁) */
        { 40, 242, 180, 46 }, { 580, 242, 180, 46 },
    };
    for (int i = 0; i < 6; i++) {
        lv_obj_t *d = create_card(fc, dev_xy[i][0], dev_xy[i][1], dev_xy[i][2], dev_xy[i][3], 8);
        lv_obj_set_style_bg_color(d, lv_color_hex(0x1b3358), 0);
        lv_obj_set_style_bg_grad_color(d, lv_color_hex(0x24508a), 0);
        lv_obj_set_style_border_color(d, lv_color_hex(CLR_ACCENT), 0);
        lv_obj_set_style_border_width(d, 2, 0);
        lv_obj_t *nm = create_label(d, dev_names[i], &lv_font_source_han_sans_bold_20, CLR_TEXT);
        lv_obj_center(nm);
        lv_obj_t *dot = create_dot(fc, dev_xy[i][0] + dev_xy[i][2] - 18, dev_xy[i][1] + 8, 6, CLR_GOOD);
        s_dev_dot[i] = dot;
    }

    /* 制冷剂流动圆点 */
    s_flow_dot = lv_obj_create(fc);
    lv_obj_set_size(s_flow_dot, 10, 10);
    lv_obj_set_style_bg_color(s_flow_dot, lv_color_hex(CLR_ACCENT), 0);
    lv_obj_set_style_radius(s_flow_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(s_flow_dot, 0, 0);
    lv_obj_clear_flag(s_flow_dot, LV_OBJ_FLAG_SCROLLABLE);
    s_flow_seg = 0;
    lv_obj_set_pos(s_flow_dot, FLOW_PATH[0][0], FLOW_PATH[0][1]);
    flow_next(NULL);

    /* 参数条 */
    lv_obj_t *pc = create_card(fc, 16, 296, 768, 30, 8);  /* 参数条（fc 卡内底部） */
    const char *pnames[4] = { "频率 Hz", "电流 A", "排气 bar", "吸气 bar" };
    for (int i = 0; i < 4; i++) {
        lv_obj_t *pl = create_label(pc, pnames[i], &lv_font_source_han_sans_bold_20, CLR_DIM);
        lv_obj_align(pl, LV_ALIGN_TOP_LEFT, 20 + i * 190, 5);
        lv_obj_t *pv = create_label(pc, "--", &mono_bold, CLR_ACCENT);
        lv_obj_align(pv, LV_ALIGN_TOP_LEFT, 110 + i * 190, 5);
        s_param[i] = pv;
    }

    /* 可滑动扩展区：流程说明（内容超出容器高度可上下滑动） */
    lv_obj_t *note = create_card(g_content, 0, 380, 800, 215, 14);
    lv_obj_t *nt = create_label(note, "流程说明(滑动查看)", &lv_font_source_han_sans_bold_20, CLR_DIM);
    lv_obj_align(nt, LV_ALIGN_TOP_LEFT, 12, 8);
    const char *notes[] = {
        "· 制冷循环: 压缩机-冷凝器-膨胀阀-蒸发器-压缩机",
        "· 除霜加热: 化霜期间点亮, 冷凝器风扇停转",
        "· 绿点: 设备运行正常; 黄点: 除霜/风机状态异常",
        "· 参数条: 频率/电流/排气/吸气 实时波动",
    };
    for (int i = 0; i < 4; i++) {
        lv_obj_t *nlab = create_label(note, notes[i], &lv_font_source_han_sans_bold_20, CLR_TEXT);
        lv_obj_align(nlab, LV_ALIGN_TOP_LEFT, 16, 44 + i * 28);
    }
}

/* ---------- 能耗与设置页 ---------- */
static void energy_status_row(lv_obj_t *parent, int y, const char *name,
                              lv_obj_t **dot_out, lv_obj_t **lbl_out)
{
    lv_obj_t *dot = create_dot(parent, 16, y, 7, CLR_DIM);
    lv_obj_t *nm = create_label(parent, name, &lv_font_source_han_sans_bold_20, CLR_TEXT);
    lv_obj_align_to(nm, dot, LV_ALIGN_OUT_RIGHT_MID, 12, 0);
    lv_obj_t *st = create_label(parent, "--", &lv_font_source_han_sans_bold_20, CLR_DIM);
    lv_obj_align(st, LV_ALIGN_TOP_RIGHT, -14, y - 6);
    *dot_out = dot;
    *lbl_out = st;
}

/* 能耗与设置页：实时功率/耗电量、设备状态（除霜/风扇/门/LED）、阈值与上报周期按钮 + 说明 */
static void build_energy(void)
{
    create_topbar("能耗与设置", 1);

    /* 卡1：能耗 */
    lv_obj_t *c1 = create_card(g_content, 8, 48, 384, 258, 14);
    lv_obj_t *t1 = create_label(c1, "实时功率", &lv_font_source_han_sans_bold_20, CLR_DIM);
    lv_obj_align(t1, LV_ALIGN_TOP_LEFT, 16, 10);
    s_lbl_power = create_label(c1, "--.-", &mono_bold, CLR_ACCENT);
    lv_obj_align(s_lbl_power, LV_ALIGN_TOP_LEFT, 16, 90);
    lv_obj_t *u1 = create_label(c1, "kW", &lv_font_source_han_sans_bold_20, CLR_DIM);
    lv_obj_align_to(u1, s_lbl_power, LV_ALIGN_OUT_RIGHT_TOP, 6, 14);
    lv_obj_t *t2 = create_label(c1, "今日耗电量", &lv_font_source_han_sans_bold_20, CLR_DIM);
    lv_obj_align(t2, LV_ALIGN_TOP_LEFT, 200, 10);
    s_lbl_kwh = create_label(c1, "--.-", &mono_bold, CLR_TEXT);
    lv_obj_align(s_lbl_kwh, LV_ALIGN_TOP_LEFT, 200, 90);
    lv_obj_t *u2 = create_label(c1, "kWh", &lv_font_source_han_sans_bold_20, CLR_DIM);
    lv_obj_align_to(u2, s_lbl_kwh, LV_ALIGN_OUT_RIGHT_TOP, 6, 14);

    /* 卡2：设备状态 */
    lv_obj_t *c2 = create_card(g_content, 408, 48, 384, 258, 14);
    lv_obj_t *t3 = create_label(c2, "设备状态", &lv_font_source_han_sans_bold_20, CLR_DIM);
    lv_obj_align(t3, LV_ALIGN_TOP_LEFT, 16, 8);
    energy_status_row(c2, 60, "除霜", &s_lbl_defrost_dot, &s_lbl_defrost);
    energy_status_row(c2, 100, "冷凝器风扇", &s_lbl_fan_dot, &s_lbl_fan);
    energy_status_row(c2, 140, "冷库门", &s_lbl_door_dot, &s_lbl_door);
    energy_status_row(c2, 180, "LED / 蜂鸣", &s_lbl_led_dot, &s_lbl_led);

    /* 卡3：报警阈值 */
    lv_obj_t *c3 = create_card(g_content, 8, 312, 384, 216, 14);
    lv_obj_t *t4 = create_label(c3, "报警阈值(℃)", &lv_font_source_han_sans_bold_20, CLR_DIM);
    lv_obj_align(t4, LV_ALIGN_TOP_LEFT, 16, 10);
    s_lbl_th = create_label(c3, "-20", &mono_bold, CLR_WARN);
    lv_obj_align(s_lbl_th, LV_ALIGN_TOP_LEFT, 16, 90);
    create_btn(c3, "-1", 200, 84, 66, 60, 0x2a3a66, 0x1b3358, "threshold", -1);
    create_btn(c3, "+1", 276, 84, 66, 60, 0x2a3a66, 0x1b3358, "threshold", 1);
    create_btn(c3, "-5", 200, 156, 66, 46, 0x6e3a4a, 0x9a4d5e, "threshold", -5);
    create_btn(c3, "+5", 276, 156, 66, 46, 0x1e7f4f, 0x2ecc71, "threshold", 5);

    /* 卡4：上报周期 */
    lv_obj_t *c4 = create_card(g_content, 408, 312, 384, 216, 14);
    lv_obj_t *t5 = create_label(c4, "上报周期(秒)", &lv_font_source_han_sans_bold_20, CLR_DIM);
    lv_obj_align(t5, LV_ALIGN_TOP_LEFT, 16, 10);
    s_lbl_iv = create_label(c4, "5", &mono_bold, CLR_ACCENT);
    lv_obj_align(s_lbl_iv, LV_ALIGN_TOP_LEFT, 16, 90);
    create_btn(c4, "1", 200, 84, 66, 46, 0x2a3a66, 0x1b3358, "interval", 1);
    create_btn(c4, "5", 276, 84, 66, 46, 0x2a3a66, 0x1b3358, "interval", 5);
    create_btn(c4, "10", 200, 140, 66, 46, 0x2a3a66, 0x1b3358, "interval", 10);
    create_btn(c4, "30", 276, 140, 66, 46, 0x2a3a66, 0x1b3358, "interval", 30);

    /* 可滑动扩展区：能耗说明（内容超出容器高度可上下滑动） */
    lv_obj_t *note = create_card(g_content, 0, 540, 800, 215, 14);
    lv_obj_t *nt = create_label(note, "能耗说明(滑动查看)", &lv_font_source_han_sans_bold_20, CLR_DIM);
    lv_obj_align(nt, LV_ALIGN_TOP_LEFT, 12, 8);
    const char *notes[] = {
        "· 实时功率: 当前压缩机组总功耗 (kW)",
        "· 今日耗电量: 自 00:00 累计用电量 (kWh)",
        "· 报警阈值: 温度越限告警门限, 可 +1/-1/+5/-5 调整",
        "· 上报周期: 数据上云间隔, 可选 1/5/10/30 秒",
    };
    for (int i = 0; i < 4; i++) {
        lv_obj_t *nlab = create_label(note, notes[i], &lv_font_source_han_sans_bold_20, CLR_TEXT);
        lv_obj_align(nlab, LV_ALIGN_TOP_LEFT, 16, 44 + i * 28);
    }
}
/* ---------- 页面路由 ---------- */
static void reset_handles(void)
{
    s_chart = NULL;
    for (int i = 0; i < ZONE_COUNT; i++) {
        s_ser[i] = NULL; s_zone_cards[i] = NULL; s_zone_sel_border[i] = NULL;
        s_zc[i] = NULL; s_zc_border[i] = NULL; s_zc_status_dot[i] = NULL;
        s_zone_tv[i] = NULL; s_zone_dt[i] = NULL; s_mon_sum[i] = NULL; s_mon_hum[i] = NULL;
    }
    s_line_alarm = s_line_warn = NULL;
    s_lbl_power = s_lbl_kwh = s_lbl_th = s_lbl_iv = NULL;
    s_lbl_defrost = s_lbl_fan = s_lbl_door = s_lbl_led = NULL;
    s_lbl_defrost_dot = s_lbl_fan_dot = s_lbl_door_dot = s_lbl_led_dot = NULL;
    for (int i = 0; i < 8; i++) { s_dev_dot[i] = NULL; }
    for (int i = 0; i < 4; i++) { s_param[i] = NULL; }
    s_flow_dot = NULL;
}

/* 构建指定页面：重置所有句柄 → 新建可滚动内容容器 → 按页分派构建函数 */
static void build_page_content(ui_page_t page)
{
    reset_handles();
    g_cur_page = page;
    g_content = lv_obj_create(lv_scr_act());
    /* 内容区铺满全屏：800x372 贴边（顶栏 52、底栏 56） */
    lv_obj_set_size(g_content, SCREEN_W, CONTENT_H);
    lv_obj_align(g_content, LV_ALIGN_TOP_LEFT, 0, TOP_H);
    lv_obj_set_style_bg_opa(g_content, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(g_content, 0, 0);
    /* 子界面内容可上下滑动（触摸/滚轮），超出容器高度时可滚动查看 */
    lv_obj_add_flag(g_content, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(g_content, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(g_content, LV_SCROLLBAR_MODE_AUTO);

    switch (page) {
        case UI_PAGE_HOME:    build_home();    break;
        case UI_PAGE_MONITOR: build_monitor(); break;
        case UI_PAGE_ZONES:   build_zones();   break;
        case UI_PAGE_CONTROL: build_control(); break;
        case UI_PAGE_ENERGY:  build_energy();  break;
        default: break;
    }
}

/* 页面滑动动画结束：删除旧页面对象 */
static void page_slide_ready_del(lv_anim_t *a) { lv_obj_del((lv_obj_t *)a->var); }
/* 页面滑动动画结束：恢复内容区滚动、解除切页锁 s_busy */
static void page_slide_clear_busy(lv_anim_t *a)
{
    (void)a;
    s_busy = 0;
    /* 动画结束恢复内容区滚动 */
    if (g_content) lv_obj_add_flag(g_content, LV_OBJ_FLAG_SCROLLABLE);
}

/* 页面切换：新页从侧滑入、旧页滑出后删除；s_busy 防止动画期间重复切页 */
static void switch_page(ui_page_t page, nav_dir_t dir)
{
    if (s_busy) return;
    if (page == g_cur_page && g_content != NULL) return;
    if (s_overlay) alarm_overlay_close();

    lv_obj_t *old = g_content;
    build_page_content(page);
    if (old == NULL) return;

    s_busy = 1;
    lv_obj_clear_flag(g_content, LV_OBJ_FLAG_SCROLLABLE);   /* 动画期间禁滚动 */
    int from = (dir == NAV_FWD) ? SCREEN_W : -SCREEN_W;
    lv_obj_set_x(g_content, from);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, g_content);
    lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)lv_obj_set_x);
    lv_anim_set_values(&a, from, 0);
    lv_anim_set_time(&a, 260);
    lv_anim_set_delay(&a, 20);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_ready_cb(&a, page_slide_clear_busy);
    lv_anim_start(&a);

    lv_anim_t b;
    lv_anim_init(&b);
    lv_anim_set_var(&b, old);
    lv_anim_set_exec_cb(&b, (lv_anim_exec_xcb_t)lv_obj_set_x);
    lv_anim_set_values(&b, 0, (dir == NAV_FWD) ? -SCREEN_W : SCREEN_W);
    lv_anim_set_time(&b, 260);
    lv_anim_set_path_cb(&b, lv_anim_path_ease_in);
    lv_anim_set_ready_cb(&b, page_slide_ready_del);
    lv_anim_start(&b);
}

/* ---------- 初始化 ---------- */
int ui_coldstorage_init(const char *cold_name, const char *cold_no, double default_threshold)
{
    if (cold_name) snprintf(s_cold_name, sizeof(s_cold_name), "%s", cold_name);
    if (cold_no)   snprintf(s_cold_no, sizeof(s_cold_no), "%s", cold_no);
    memset(&s_state, 0, sizeof(s_state));
    for (int z = 0; z < ZONE_COUNT; z++) {
        s_state.zones[z].setpoint = (z == ZONE_FROZEN) ? default_threshold : 0;
    }

    /* 字体 fallback 链 */
    lv_font_source_han_sans_bold_20.fallback = &lv_font_symbols_20;
    lv_font_symbols_20.fallback = &lv_font_montserrat_20;   /* 20px ASCII 与汉字同字号，数字不再显小/截断 */
    mono_bold.fallback = &lv_font_symbols_20;

    /* 屏幕渐变背景 */
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_hex(CLR_BG_TOP), 0);
    lv_obj_set_style_bg_grad_color(scr, lv_color_hex(CLR_BG_BOT), 0);
    lv_obj_set_style_bg_grad_dir(scr, LV_GRAD_DIR_VER, 0);
    /* 固定界面：关闭屏幕滚动 */
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

#if LV_UI_BG_BMP
    lv_obj_t *bg = lv_img_create(scr);
    lv_img_set_src(bg, LV_UI_BG_BMP_PATH);
    lv_obj_align(bg, LV_ALIGN_TOP_LEFT, 0, 0);
#endif

    build_topbar();
    build_bottombar();
    build_page_content(UI_PAGE_HOME);
    lv_timer_create(refresh_timer_cb, 500, NULL);
    return 0;
}

void ui_coldstorage_show_page(ui_page_t page)
{
    switch_page(page, NAV_FWD);
}

void ui_coldstorage_set_auto_alarm_page(int enable)
{
    (void)enable;   /* 保留接口：本版 UI 固定报警栏，不自动跳页 */
}

void ui_coldstorage_set_action_cb(ui_action_cb_t cb, void *user)
{
    s_action_cb   = cb;
    s_action_user = user;
}

void ui_coldstorage_update(const ui_state_t *st)
{
    if (!st) return;
    pthread_mutex_lock(&s_lock);
    s_state = *st;
    for (int z = 0; z < ZONE_COUNT; z++) {
        s_hist[z][s_hist_count % HIST_LEN] = st->zones[z].temperature;
    }
    s_hist_count++;
    pthread_mutex_unlock(&s_lock);
}

void ui_coldstorage_alarm_add(int level, const char *zone, const char *desc)
{
    pthread_mutex_lock(&s_lock);
    alarm_rec_t *r = &s_alarms[s_alarm_head % ALARM_MAX];
    s_alarm_head++;
    r->level = level;
    snprintf(r->zone, sizeof(r->zone), "%s", zone ? zone : "");
    snprintf(r->desc, sizeof(r->desc), "%s", desc ? desc : "");
    r->confirmed = 0;
    time_t now = time(NULL);
    struct tm tmv;
    struct tm *lt = localtime_r(&now, &tmv);
    if (lt) snprintf(r->tstr, sizeof(r->tstr), "%02d:%02d:%02d", lt->tm_hour, lt->tm_min, lt->tm_sec);
    else    snprintf(r->tstr, sizeof(r->tstr), "--:--:--");
    int un = 0;
    for (int i = 0; i < ALARM_MAX; i++) if (!s_alarms[i].confirmed) un++;
    s_alarm_count = un;
    pthread_mutex_unlock(&s_lock);
}

/* ---------- 定时刷新 ---------- */
static void update_marquee(void)
{
    /* 取最新一条未确认报警 */
    char text[160] = "无报警";
    int  level = 0;
    pthread_mutex_lock(&s_lock);
    for (int k = 0; k < ALARM_MAX; k++) {
        int idx = s_alarm_head - 1 - k;
        while (idx < 0) idx += ALARM_MAX;
        alarm_rec_t *rec = &s_alarms[idx];
        if (rec->zone[0] == '\0') continue;
        if (rec->confirmed) continue;
        const char *lv_name = rec->level >= 2 ? "紧急" : (rec->level == 1 ? "重要" : "一般");
        snprintf(text, sizeof(text), "%s  [%s]  %s  %s", rec->tstr, lv_name, rec->zone, rec->desc);
        level = rec->level;
        break;
    }
    pthread_mutex_unlock(&s_lock);

    if (s_alarm_marquee) {
        lv_label_set_text(s_alarm_marquee, text);
        lv_obj_set_style_text_color(s_alarm_marquee,
            lv_color_hex(level >= 2 ? CLR_BAD : (level == 1 ? CLR_WARN : CLR_DIM)), 0);
        /* 走马灯：内容超过 560px 窗口时循环滚动（滚动->停顿->滚回） */
        lv_anim_del(s_alarm_marquee, (lv_anim_exec_xcb_t)lv_obj_set_x);
        int tw = lv_obj_get_width(s_alarm_marquee);
        int cw = 560;
        lv_obj_set_x(s_alarm_marquee, 0);
        if (tw > cw) {
            uint32_t dur = (uint32_t)((tw - cw) / 10) * 300 + 500;
            lv_anim_t a;
            lv_anim_init(&a);
            lv_anim_set_var(&a, s_alarm_marquee);
            lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)lv_obj_set_x);
            lv_anim_set_values(&a, 0, cw - tw);
            lv_anim_set_time(&a, dur);
            lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
            lv_anim_set_playback_time(&a, dur);
            lv_anim_set_playback_delay(&a, 900);
            lv_anim_set_path_cb(&a, lv_anim_path_linear);
            lv_anim_start(&a);
        }
        /* 紧急红闪 */
        if (level >= 2 && s_alarm_bar) {
            s_alarm_blink = !s_alarm_blink;
            lv_obj_set_style_border_color(s_alarm_bar, lv_color_hex(s_alarm_blink ? CLR_BAD : CLR_ACCENT_D), 0);
            lv_obj_set_style_border_width(s_alarm_bar, 2, 0);
        } else if (s_alarm_bar) {
            lv_obj_set_style_border_width(s_alarm_bar, 0, 0);
        }
    }
}

/* 定时刷新（500ms）：读共享快照 → 更新顶栏时钟/在线/报警数 → 按当前页刷新图表、温区卡、设备状态、能耗 */
static void refresh_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    ui_state_t snap;
    char buf[64];
    time_t now = time(NULL);
    struct tm tmv;
    struct tm *lt = localtime_r(&now, &tmv);

    pthread_mutex_lock(&s_lock);
    snap = s_state;
    int alarm_un = s_alarm_count;
    pthread_mutex_unlock(&s_lock);

    /* 顶栏 */
    if (lt && s_lbl_time) {
        snprintf(buf, sizeof(buf), "%02d:%02d:%02d", lt->tm_hour, lt->tm_min, lt->tm_sec);
        lv_label_set_text(s_lbl_time, buf);
    }
    if (s_lbl_outdoor) {
        if (snap.system.system_online) set_label_text(s_lbl_outdoor, "室外 %.1f℃", snap.system.outdoor_temp);
        else                           lv_label_set_text(s_lbl_outdoor, "室外 --.-℃");
    }
    if (s_lbl_online_dot) set_dot_color(s_lbl_online_dot, snap.system.system_online ? CLR_GOOD : CLR_BAD);
    if (s_lbl_online) {
        set_label_text(s_lbl_online, "%s", snap.system.system_online ? "在线" : "离线");
        lv_obj_set_style_text_color(s_lbl_online, lv_color_hex(snap.system.system_online ? CLR_GOOD : CLR_BAD), 0);
    }
    if (s_lbl_alarm_count) {
        set_label_text(s_lbl_alarm_count, "报警 %d", alarm_un);
        lv_obj_set_style_text_color(s_lbl_alarm_count, lv_color_hex(alarm_un > 0 ? CLR_BAD : CLR_DIM), 0);
    }
    update_marquee();

    /* 状态颜色 */
    uint32_t st_color[3] = { CLR_GOOD, CLR_WARN, CLR_BAD };

    switch (g_cur_page) {
    case UI_PAGE_HOME:
        /* 主菜单为静态 2x2 入口卡，无需刷新 */
        break;

    case UI_PAGE_MONITOR:
        /* 图表推点 */
        if (s_chart) {
            for (int z = 0; z < ZONE_COUNT; z++) {
                if (s_ser[z]) lv_chart_set_next_value(s_chart, s_ser[z], (lv_coord_t)snap.zones[z].temperature);
            }
            if (s_line_alarm && s_line_warn) {
                lv_chart_set_next_value(s_chart, s_line_alarm, (lv_coord_t)lround(snap.zones[ZONE_FROZEN].setpoint + 4));
                lv_chart_set_next_value(s_chart, s_line_warn, (lv_coord_t)lround(snap.zones[ZONE_FROZEN].setpoint + 2));
            }
        }
        /* 下半温区卡 */
        for (int z = 0; z < ZONE_COUNT; z++) {
            if (s_zone_tv[z]) set_temp_text(s_zone_tv[z], snap.system.system_online, snap.zones[z].temperature);
            if (s_zone_dt[z]) set_label_text(s_zone_dt[z], "湿度 %.0f%% 设定 %.0f", snap.zones[z].humidity, snap.zones[z].setpoint);
            if (s_mon_sum[z]) {
                if (snap.system.system_online)
                    set_label_text(s_mon_sum[z], "%.1f°C", snap.zones[z].temperature);
                else
                    lv_label_set_text(s_mon_sum[z], "--.-°C");
            }
            if (s_mon_hum[z]) {
                if (snap.system.system_online)
                    set_label_text(s_mon_hum[z], "湿度 %.0f%%", snap.zones[z].humidity);
                else
                    lv_label_set_text(s_mon_hum[z], "湿度 --%");
            }
            if (s_zone_cards[z]) {
                int sel = (z == s_sel_zone);
                lv_obj_set_style_border_color(s_zone_cards[z],
                    lv_color_hex(sel ? CLR_ACCENT : status_color(snap.zones[z].status)), 0);
                lv_obj_set_style_border_width(s_zone_cards[z], sel ? 3 : 1, 0);
            }
        }
        break;

    case UI_PAGE_ZONES:
        for (int z = 0; z < ZONE_COUNT; z++) {
            if (!s_zc[z]) continue;
            /* 温度大字（child 0 是标题，child 1 是温度） */
            lv_obj_t *child = lv_obj_get_child(s_zc[z], 1);
            if (child && lv_obj_check_type(child, &lv_label_class)) {
                if (snap.system.system_online) lv_label_set_text_fmt(child, "%.1f", snap.zones[z].temperature);
                else                           lv_label_set_text(child, "--.-");
            }
            lv_obj_t *dt = lv_obj_get_child(s_zc[z], 3);
            if (dt && lv_obj_check_type(dt, &lv_label_class)) {
                lv_label_set_text_fmt(dt, "湿度 %.1f%%   设定 %.1f℃",
                                      snap.zones[z].humidity, snap.zones[z].setpoint);
            }
            if (s_zc_status_dot[z]) set_dot_color(s_zc_status_dot[z], status_color(snap.zones[z].status));
            /* 异常红闪边框 */
            if (s_zc_border[z]) {
                if (snap.zones[z].status == 2) {
                    s_alarm_blink = !s_alarm_blink;
                    lv_obj_set_style_border_color(s_zc_border[z], lv_color_hex(s_alarm_blink ? CLR_BAD : CLR_WARN), 0);
                    lv_obj_set_style_border_width(s_zc_border[z], 3, 0);
                } else {
                    lv_obj_set_style_border_color(s_zc_border[z], lv_color_hex(status_color(snap.zones[z].status)), 0);
                    lv_obj_set_style_border_width(s_zc_border[z], 1, 0);
                }
            }
        }
        break;

    case UI_PAGE_CONTROL:
        /* 设备状态：除霜中→除霜加热黄；风扇关→冷风机黄；其余绿 */
        for (int i = 0; i < 6; i++) {
            if (!s_dev_dot[i]) continue;
            uint32_t c = CLR_GOOD;
            if (i == 4 && snap.energy.defrost) c = CLR_WARN;
            if (i == 5 && !snap.energy.condenser_fan) c = CLR_WARN;
            set_dot_color(s_dev_dot[i], c);
        }
        /* 参数条（模拟值 + 小幅波动） */
        if (s_param[0]) {
            static unsigned int tick = 0;
            tick++;
            double freq = 50.0 + 3.0 * sin((double)(tick % 360) / 57.3);
            double cur  = 11.0 + 2.0 * sin((double)((tick + 90) % 360) / 57.3);
            double dis  = 12.5 + 1.5 * sin((double)((tick + 180) % 360) / 57.3);
            double suc  = 3.2 + 0.6 * sin((double)((tick + 270) % 360) / 57.3);
            set_label_text(s_param[0], "%.0f", freq);
            set_label_text(s_param[1], "%.1f", cur);
            set_label_text(s_param[2], "%.1f", dis);
            set_label_text(s_param[3], "%.1f", suc);
        }
        break;

    case UI_PAGE_ENERGY:
        if (s_lbl_power) set_label_text(s_lbl_power, "%.1f", snap.energy.power_kw);
        if (s_lbl_kwh)   set_label_text(s_lbl_kwh, "%.1f", snap.energy.energy_kwh);
        if (s_lbl_th)    set_label_text(s_lbl_th, "%.0f", snap.zones[ZONE_FROZEN].setpoint);
        if (s_lbl_iv)    set_label_text(s_lbl_iv, "%d", s_report_interval);
        if (s_lbl_defrost) {
            set_label_text(s_lbl_defrost, "%s", snap.energy.defrost ? "运行中" : "停止");
            if (s_lbl_defrost_dot) set_dot_color(s_lbl_defrost_dot, snap.energy.defrost ? CLR_WARN : CLR_GOOD);
        }
        if (s_lbl_fan) {
            set_label_text(s_lbl_fan, "%s", snap.energy.condenser_fan ? "运行" : "停止");
            if (s_lbl_fan_dot) set_dot_color(s_lbl_fan_dot, snap.energy.condenser_fan ? CLR_GOOD : CLR_WARN);
        }
        if (s_lbl_door) {
            const char *door_text[3] = { "关闭", "开启", "故障" };
            set_label_text(s_lbl_door, "%s", door_text[snap.energy.door > 2 ? 2 : snap.energy.door]);
            if (s_lbl_door_dot) set_dot_color(s_lbl_door_dot, st_color[snap.energy.door == 2 ? 2 : (snap.energy.door == 1 ? 1 : 0)]);
        }
        if (s_lbl_led) {
            set_label_text(s_lbl_led, "LED %s / 蜂鸣 %s", snap.led ? "开" : "关", snap.beeper ? "开" : "关");
            if (s_lbl_led_dot) set_dot_color(s_lbl_led_dot, (snap.led || snap.beeper) ? CLR_GOOD : CLR_DIM);
        }
        break;
    default:
        break;
    }
}
