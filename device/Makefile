# 冷库数据监控系统 - 设备端 Makefile
# 本机编译：make
# ARM Cortex-A53 Linux 板  交叉编译（32 位板子，用厂商工具链 arm-linux-gcc，soft float）：
#   make clean && make CC=arm-linux-gcc
# 注意：勿用 apt 的 arm-linux-gnueabihf-gcc（hard float），与板子系统 ABI 不匹配会运行崩溃。

CC      ?= gcc
CFLAGS  ?= -Wall -O2
STD      = -std=c99 -pthread -D_POSIX_C_SOURCE=200809L -D_GNU_SOURCE
LDLIBS   = -pthread

SRCS    = protocol.c sensor.c actuator.c tcp_client.c device.c
OBJS    = $(SRCS:.c=.o)
TARGET  = device

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) $(STD) -o $@ $(OBJS) $(LDLIBS)

%.o: %.c
	$(CC) $(CFLAGS) $(STD) -c -o $@ $<

clean:
	rm -f $(TARGET) $(OBJS)

.PHONY: all clean
