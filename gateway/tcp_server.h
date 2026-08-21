#ifndef GATEWAY_TCP_SERVER_H
#define GATEWAY_TCP_SERVER_H

#include <stddef.h>
#include "protocol.h"

/* 收到完整帧后的回调；fd 为来源连接 */
typedef void (*TcpFrameHandler)(int fd, const Frame *frame);

/* 连接断开（recv 返回 0 或出错）后的回调 */
typedef void (*TcpDisconnectHandler)(int fd);

/* 启动 TCP 服务端（监听端口 port），内部起 accept 线程，每连接一个读线程 */
int  tcp_server_start(int port, TcpFrameHandler onFrame,
                      TcpDisconnectHandler onDisconnect);

/* 向指定连接发送完整数据（线程安全，循环写直到写完） */
int  tcp_server_send(int fd, const char *buf, size_t len);

/* 停止服务端 */
void tcp_server_stop(void);

#endif /* GATEWAY_TCP_SERVER_H */
