/*
 * tcp_client.h - TCP 客户端
 *
 * 连接网关、断线自动重连（退避 1s/2s/4s…上限 30s）、发送整帧、独立接收线程。
 */
#ifndef TCP_CLIENT_H
#define TCP_CLIENT_H

#include <stddef.h>

/* 收到一帧（不含 \n）时回调；运行在接收线程中 */
typedef void (*tcp_frame_cb_t)(const char *frame, size_t len, void *user);

typedef struct tcp_client tcp_client_t;

/* 创建客户端（不连接） */
tcp_client_t *tcp_client_create(const char *ip, int port);

/* 设置帧回调 */
void tcp_client_set_frame_cb(tcp_client_t *c, tcp_frame_cb_t cb, void *user);

/* 启动网络线程（连接 + 接收），失败返回 -1 */
int tcp_client_start(tcp_client_t *c);

/* 已连接则发送整帧（buf 应含末尾 \n），失败返回 -1 */
int tcp_client_send(tcp_client_t *c, const char *buf, size_t len);

/* 是否已连接 */
int tcp_client_is_connected(tcp_client_t *c);

/* 停止网络线程（幂等） */
void tcp_client_stop(tcp_client_t *c);

/* 停止并释放 */
void tcp_client_destroy(tcp_client_t *c);

#endif /* TCP_CLIENT_H */
