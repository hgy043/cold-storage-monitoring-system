# =====================================================================
#  冷库监控系统 - Ubuntu 边缘网关 Makefile
#  依赖：../../huaweicloud-iot-device-quickstart（华为 IoT AgentLite SDK）
#  目标平台：Ubuntu x86-64
# =====================================================================

CC      = gcc
SDK     = ../../huaweicloud-iot-device-quickstart

# 头文件路径：agentlite / service / util / third_party/cjson
INC  = -I$(SDK)/include/agentlite
INC += -I$(SDK)/include/service
INC += -I$(SDK)/include/util
INC += -I$(SDK)/include/third_party/cjson

# 库路径
LIB  = -L$(SDK)/lib

# 编译选项：-Wall 打开警告（不用 -w 屏蔽）；SDK 头文件若有非致命警告可忽略
CFLAGS = -g -Wall -O2 -m64 -pthread $(INC)

# 链接：HWMQTT(AgentLite) + paho-mqtt3as + pthread + rt + m
LIBS = $(LIB) -lHWMQTT -lpaho-mqtt3as -lpthread -lrt -lm

TARGET = gateway
OBJS   = gateway.o tcp_server.o protocol.o device_table.o cloud_bridge.o

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $(TARGET) $(OBJS) $(LIBS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET) *.d

.PHONY: all clean
