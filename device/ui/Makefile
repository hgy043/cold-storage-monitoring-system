# ============================================================
# 冷库监测终端 LVGL 界面 Makefile（gec6818 板端）
#
# 依赖：lvgl_linux_framebuffer-9 移植工程（本工作区根目录）
#
# 用法：
#   make                 # 交叉编译（编译器固定为 arm-linux-gcc），产物 coldstorage_ui
#   make clean
#
# 目标板：gec6818 32 位 Linux。交叉编译产物由工具链目标架构决定，与主机位数无关；
#         必须用板子系统配套的厂商工具链（arm-linux-gcc，soft float），
#         勿用 apt 的 arm-linux-gnueabihf-gcc（hard float），ABI 不匹配会崩溃。
# 验证产物：file coldstorage_ui 应显示 "ELF 32-bit LSB executable, ARM"；readelf -A 看 float ABI。
#
# 说明：
#   - LVGL 核心 + 驱动源码来自 LVGL_DIR（移植工程），本目录只放界面代码
#   - 中文字体直接引用移植工程 test/ 下的 lv_font_source_han_sans_bold_20.c
# ============================================================

# LVGL 移植工程路径（相对本 Makefile 所在目录，也可用 LVGL_DIR=... 覆盖）
LVGL_ROOT ?= ../../../lvgl_linux_framebuffer-9/lv_port_linux_frame_buffer-master
LVGL_DIR := $(abspath $(LVGL_ROOT))
LVGL_PATH := $(LVGL_DIR)/lvgl

# 交叉编译器（板端）：本工程只允许 arm-linux-gcc 一种工具链。
# 用 override 锁死而非 ?=：否则 make CC=gcc 或 shell 里 export CC=gcc 会悄悄换成
# 本机 gcc，编出 x86 产物传到板子直接 execve 失败（busybox 回退当脚本解析报语法错误）。
override CC := arm-linux-gcc

CFLAGS ?= -O2 -g -Wall -std=gnu99
CFLAGS += -I$(LVGL_DIR) -I$(LVGL_DIR)/lvgl -I$(LVGL_DIR)/lv_drivers
LDFLAGS ?= -lm -lpthread

BIN = coldstorage_ui

# 本工程源文件
CSRCS := lv_coldstorage_ui.c ui_main.c
# 中文字体（来自 LVGL 移植工程，仅含汉字字形）
CSRCS += lv_font_han_20.c   # 中文（复制自 LVGL 移植工程 test/，已改 v9 非 const）
CSRCS += lv_font_symbols_20.c
CSRCS += lv_font_mono_bold_32.c
CSRCS += lv_font_montserrat_20.c   # 20px ASCII 数字（本地拷贝，避免依赖 LVGL 树 lv_conf 开关）

# ---- make clean 不依赖 LVGL 路径（空格检测/引入只在构建时执行） ----
ifeq ($(MAKECMDGOALS),clean)
clean:
	rm -f *.o $(BIN)
.PHONY: clean
else
# ---- 空格路径检测（GNU make 的 include 按空格分词，路径含空格会失败） ----
space := $(subst x,, )
ifneq ($(findstring $(space),$(LVGL_DIR)),)
$(error 检测到 LVGL_DIR 路径含空格："$(LVGL_DIR)"，请 ln -s 到无空格路径后重试：make LVGL_ROOT=/home/gec/lvgl9)
endif

# ---- LVGL 路径存在性检测 ----
ifneq ($(wildcard $(LVGL_DIR)/lv_drivers/lv_drivers.mk),)
include $(LVGL_DIR)/lvgl/lvgl.mk
include $(LVGL_DIR)/lv_drivers/lv_drivers.mk
else
$(error LVGL_DIR 不是有效的 LVGL 移植工程（找不到 lv_drivers/lv_drivers.mk）："$(LVGL_DIR)"。请检查符号链接，或 make LVGL_ROOT=<正确路径>)
endif

OBJS = $(CSRCS:.c=.o)

all: $(BIN)

%.o: %.c
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(BIN): $(OBJS)
	$(CC) -o $(BIN) $(OBJS) $(LDFLAGS)
	@echo "build ok: $(BIN)"

# 头文件依赖（改 .h 自动触发重编，避免旧 .o 混入）
-include $(OBJS:.o=.d)

.PHONY: all
endif