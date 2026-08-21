/*
 * tcp_server.c —— TCP 服务端线程
 *
 * accept 设备连接 -> 每连接一个读线程 -> 按 '\n' 分行解析帧 -> 回调。
 * 单帧最大 PROTO_MAX_FRAME_LEN 字节，超长丢弃并告警。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "tcp_server.h"
#include "LogUtil.h"

static TcpFrameHandler      g_frame_handler = NULL;
static TcpDisconnectHandler g_disconnect_handler = NULL;
static volatile int         g_running = 0;
static int                  g_listen_fd = -1;
static pthread_mutex_t      g_send_mutex = PTHREAD_MUTEX_INITIALIZER;

/* 处理一行完整帧 */
static void handle_line(int fd, const char *line)
{
    Frame frame;

    if (protocol_parse_frame(line, &frame) != 0) {
        printfLog(EN_LOG_LEVEL_WARNING,
                  "tcp_server: invalid frame dropped: %s\n", line);
        return;
    }
    if (g_frame_handler != NULL) {
        g_frame_handler(fd, &frame);
    }
}

/* 每个连接的读线程 */
static void *conn_thread(void *arg)
{
    int fd = (int)(long)arg;
    char line[PROTO_MAX_FRAME_LEN + 1];
    size_t len = 0;
    int overflow = 0;
    char tmp[1024];
    ssize_t n;
    ssize_t i;

    while ((n = recv(fd, tmp, sizeof(tmp), 0)) > 0) {
        for (i = 0; i < n; i++) {
            char c = tmp[i];
            if (c == '\n') {
                if (!overflow && len > 0) {
                    line[len] = '\0';
                    handle_line(fd, line);
                }
                len = 0;
                overflow = 0;
            } else {
                if (len < PROTO_MAX_FRAME_LEN) {
                    line[len++] = c;
                } else if (!overflow) {
                    overflow = 1;
                    printfLog(EN_LOG_LEVEL_WARNING,
                              "tcp_server: frame exceeds %d bytes, dropped\n",
                              PROTO_MAX_FRAME_LEN);
                }
            }
        }
    }

    printfLog(EN_LOG_LEVEL_INFO, "tcp_server: connection fd=%d closed\n", fd);
    /* 关闭顺序必须“先通知、后 close(fd)”：
     * disconnect 回调里的 device_table_unregister 与 device_table_send 共用设备表锁，
     * 先 unregister 可保证：任何在途发送先完成、节点先删除，之后才 close(fd)。
     * 否则 close(fd) 后 fd 号可能被新连接复用，命令会错发到新连接。 */
    if (g_disconnect_handler != NULL) {
        g_disconnect_handler(fd);
    }
    close(fd);
    return NULL;
}

/* accept 线程 */
static void *accept_thread(void *arg)
{
    (void)arg;
    while (g_running) {
        struct sockaddr_in addr;
        socklen_t alen = sizeof(addr);
        int fd = accept(g_listen_fd, (struct sockaddr *)&addr, &alen);
        if (fd < 0) {
            if (g_running) {
                printfLog(EN_LOG_LEVEL_WARNING,
                          "tcp_server: accept failed: %s\n", strerror(errno));
            }
            continue;
        }
        /* inet_ntoa 返回静态缓冲且非线程安全，改用 inet_ntop（线程安全） */
        char ipbuf[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &addr.sin_addr, ipbuf, sizeof(ipbuf));
        printfLog(EN_LOG_LEVEL_INFO,
                  "tcp_server: device connected fd=%d from %s:%d\n",
                  fd, ipbuf, ntohs(addr.sin_port));

        {
            pthread_t t;
            if (pthread_create(&t, NULL, conn_thread, (void *)(long)fd) != 0) {
                close(fd);
                continue;
            }
            pthread_detach(t);
        }
    }
    return NULL;
}

/* 启动 TCP 服务：监听端口，每连接独立线程处理 */
int tcp_server_start(int port, TcpFrameHandler onFrame,
                     TcpDisconnectHandler onDisconnect)
{
    int opt = 1;
    struct sockaddr_in addr;

    g_frame_handler = onFrame;
    g_disconnect_handler = onDisconnect;

    g_listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (g_listen_fd < 0) {
        printfLog(EN_LOG_LEVEL_ERROR, "tcp_server: socket failed: %s\n",
                  strerror(errno));
        return -1;
    }

    setsockopt(g_listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons((unsigned short)port);

    if (bind(g_listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        printfLog(EN_LOG_LEVEL_ERROR, "tcp_server: bind port %d failed: %s\n",
                  port, strerror(errno));
        close(g_listen_fd);
        return -1;
    }

    if (listen(g_listen_fd, 32) < 0) {
        printfLog(EN_LOG_LEVEL_ERROR, "tcp_server: listen failed: %s\n",
                  strerror(errno));
        close(g_listen_fd);
        return -1;
    }

    g_running = 1;
    {
        pthread_t t;
        pthread_create(&t, NULL, accept_thread, NULL);
        pthread_detach(t);
    }

    printfLog(EN_LOG_LEVEL_INFO, "tcp_server: listening on 0.0.0.0:%d\n", port);
    return 0;
}

/* 向指定 fd 发送一帧（阻塞写，调用方需先确认节点在线） */
int tcp_server_send(int fd, const char *buf, size_t len)
{
    size_t off = 0;

    if (buf == NULL) {
        return -1;
    }

    pthread_mutex_lock(&g_send_mutex);
    while (off < len) {
        ssize_t w = send(fd, buf + off, len - off, MSG_NOSIGNAL);
        if (w <= 0) {
            pthread_mutex_unlock(&g_send_mutex);
            return -1;
        }
        off += (size_t)w;
    }
    pthread_mutex_unlock(&g_send_mutex);
    return 0;
}

/* 停止 TCP 服务：关闭监听 socket */
void tcp_server_stop(void)
{
    g_running = 0;
    if (g_listen_fd >= 0) {
        shutdown(g_listen_fd, SHUT_RDWR);
        close(g_listen_fd);
        g_listen_fd = -1;
    }
}
