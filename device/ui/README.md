# device/ui —— gec6818 板端冷库监控终端（工业 SCADA/HMI · 800×480）

基于工作区根目录的 LVGL Linux Framebuffer 移植工程（`lvgl_linux_framebuffer-9/lv_port_linux_frame_buffer-master`，LVGL v9）。

**界面结构（全局固定，不可滚动）**：
- 顶栏 52px：冷库名称/编号 + 时钟 + 室外温度/系统在线/报警总数
- 底部报警栏 56px：最新报警走马灯（紧急红色闪烁）+ 报警详情浮层（逐条确认）
- 内容区 800×372，5 页左右滑动切换

**配色**：背景深蓝渐变 #10264A→#050B18、卡片 #132544/#1B3358、强调科技青 #35D8FF、状态绿/黄/红。

## 页面

| 页面 | 内容 |
|---|---|
| 主菜单 | 冷库平面图（4 温区温度/状态）+ 2×2 入口大卡片 |
| 实时监测 | 24h 多温区趋势折线图（红/黄阈值线、图例筛选）+ 4 温区当前值卡（点击选中联动） |
| 温区总览 | 4 温区卡 2×2：温度大字/湿度/设定值/状态灯，异常红闪 |
| 设备控制 | 制冷流程图（压缩机/冷凝器/膨胀阀/蒸发器/除霜/风机 + 制冷剂流动动画）+ 参数条（频率/电流/排气/吸气） |
| 能耗与设置 | 实时功率/今日 kWh、除霜/风扇/门/LED 状态、报警阈值与上报周期设置按钮 |

## 数据与接口

- **数据模型 `ui_state_t`**：4 温区（冷冻/冷藏/预冷/穿堂：温度/湿度/设定值/状态 0正常 1预警 2报警）+ 能耗（功率/kWh/除霜/风扇/门）+ 系统（室外温度/在线）+ 执行器（LED/蜂鸣器）。
- **更新**：`ui_coldstorage_update(&st)`（任意线程，互斥锁保护）
- **报警**：`ui_coldstorage_alarm_add(level, zone, desc)` 喂入底部报警栏（0 一般/1 重要/2 紧急）
- **动作回调**：`ui_coldstorage_set_action_cb(cb, user)`，action = led/beeper/threshold/interval/spike
- **字体**：思源黑体粗 20（中文）+ `lv_font_symbols_20`（°·℃█）+ `mono_bold`（Consolas 等宽粗体 32px 数字，本目录 lv_font_mono_bold_32.c）
- **触控**：主按钮≥48px，辅助按钮≥30px，支持戴手套

## 编译与运行

```bash
# 空格路径（如 "deepseek harness"）会让 make 的 include 分词失败，先建无空格符号链接：
ln -s "/mnt/.../deepseek harness/lvgl_linux_framebuffer-9/lv_port_linux_frame_buffer-master" /home/gec/lvgl9b
cd device/ui
make LVGL_ROOT=/home/gec/lvgl9b CC=arm-linux-gcc   # 厂商 32 位工具链（soft float）
# 板端运行：./coldstorage_ui [冷库名] [编号] [报警阈值]
./coldstorage_ui 1号冷库 NO.001 -20
```

- 显示：自动读取 framebuffer 分辨率（/dev/fb0），回退 800×480
- 触摸：evdev（/dev/input/event0，evtest 确认设备号，改 lv_drv_conf.h 的 EVDEV_NAME）
- **字体 fallback**：han/mono/symbols 三个字体在 v9 均为非 const（lv_font_conv 模板 v8+ 默认 const，会进 .rodata 导致写 .fallback 段错误；已手工改守卫，重新生成字体后需再次改回）

## 集成 device.c（真实数据）

1. 初始化 LVGL（lv_init + fbdev + evdev + 深色主题，见 ui_main.c）；
2. 真实采集线程填充 4 温区/能耗/系统到 `ui_state_t`，调 `ui_coldstorage_update()`；
3. 报警经 `ui_coldstorage_alarm_add()` 喂入；
4. 按钮回调映射：led/beeper → `actuator_*()`，threshold/interval → 本地配置 + TCP 上报（语义见 docs/02）。

## 背景图（可选）

`assets/bg.bmp` 为 1024×600 生成（`node assets/gen_bg.js` 可重新生成，800×480 屏需先改 W/H）。
启用：拷到板子（如 S:/IOT/coldstorage/bg.bmp，盘符见 lv_conf.h）后编译加 `-DLV_UI_BG_BMP=1`；默认 LVGL 渐变背景无需图片。

## 文件

| 文件 | 职责 |
|---|---|
| lv_coldstorage_ui.h | 数据模型（多温区/能耗/报警）与对外 API |
| lv_coldstorage_ui.c | 全局顶栏/报警栏 + 5 页 + 滑动动画 + 刷新 |
| lv_font_symbols_20.c | °·℃█ 符号字体（Dengb.ttf 生成） |
| lv_font_mono_bold_32.c | 等宽粗体数字（Consolas Bold 生成，变量名 mono_bold） |
| ui_main.c | 入口：驱动初始化 + 4 温区/能耗模拟 + 报警事件 |
| assets/ | 背景图生成脚本与 BMP |
