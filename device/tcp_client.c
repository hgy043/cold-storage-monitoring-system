/*
 * tcp_client.c - TCP 客户端
 *
 * 职责：
 *   连接网关、断线自动重连（退避 1s/2s/4s…上限 30s）、发送整帧、
 *   独立接收线程按 \n 切帧并容忍半包/粘包/超长帧。
 */
#include "tcp_client.h"
#include "protocol.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define RECV_CHUNK        1024
#define CONNECT_TIMEOUT_S 5

struct tcp_client {
    char ip[64];
    int port;
    int sockfd;             /* 当前连接的套接字，-1 表示未连接 */
    int running;            /* 网络线程运行标志,置 0 后线程退出 */
    int connected;          /* 是否已连接 */
    int started;            /* 网络线程是否已创建,用于停止时判断是否需要 join */
    pthread_mutex_t lock;       /* 保护连接状态 */
    pthread_mutex_t send_lock;  /* 串行化发送，避免两线程向同一个 fd 写入*/
    pthread_t thread;
    tcp_frame_cb_t on_frame;
    void *user;
};

/* 非阻塞 connect + select 实现带超时的连接 */
static int connect_timeout(tcp_client_t *c, int timeout_s)
{
    int fd, flags, rc;
    struct sockaddr_in addr;
    socklen_t slen;
    int soerr = 0;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)c->port);
    if (inet_pton(AF_INET, c->ip, &addr.sin_addr) != 1) {
        close(fd);
        return -1;
    }

    rc = connect(fd, (struct sockaddr *)&addr, sizeof(addr));
    if (rc != 0 && errno != EINPROGRESS) {
        close(fd);
        return -1;
    }

    {
        fd_set wfds;
        struct timeval tv;
        FD_ZERO(&wfds);
        FD_SET(fd, &wfds);
        tv.tv_sec = timeout_s;
        tv.tv_usec = 0;
        rc = select(fd + 1, NULL, &wfds, NULL, &tv);
    }
    if (rc <= 0) {
        close(fd);
        return -1;
    }
    slen = sizeof(soerr);
    getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &slen);
    if (soerr != 0) {
        close(fd);
        return -1;
    }
    fcntl(fd, F_SETFL, flags);   /* 恢复阻塞模式 */
    return fd;
}

/* 可中断的退避睡眠：每秒检查一次 running */
static void backoff_sleep(tcp_client_t *c, int seconds)
{
    int i;
    for (i = 0; i < seconds; i++) {
        pthread_mutex_lock(&c->lock);
        int running = c->running;
        pthread_mutex_unlock(&c->lock);
        if (!running) {
            return;
        }
        sleep(1);
    }
}

/* 按 \n 切帧处理粘包/半包，完整帧回调 frame_cb */
static void process_frame(tcp_client_t *c, char *line, size_t len)
{
    if (c->on_frame) {
        c->on_frame(line, len, c->user);
    }
}

/* 网络线程：连接 + 接收 */
static void *network_thread(void *arg)
{
    tcp_client_t *c = (tcp_client_t *)arg;
    int backoff = 1;

    while (1) {
        pthread_mutex_lock(&c->lock);
        int running = c->running;
        pthread_mutex_unlock(&c->lock);
        if (!running) {
            break;
        }

        int fd = connect_timeout(c, CONNECT_TIMEOUT_S);
        if (fd < 0) {
            backoff_sleep(c, backoff);
            if (backoff < 30) {
                backoff *= 2;
            }
            continue;
        }

        /* 连接成功 */
        pthread_mutex_lock(&c->lock);
        if (!c->running) {
            pthread_mutex_unlock(&c->lock);
            close(fd);
            break;
        }
        c->sockfd = fd;
        c->connected = 1;
        pthread_mutex_unlock(&c->lock);
        printf("[tcp] 已连接网关 %s:%d\n", c->ip, c->port);
        backoff = 1;

        /* 接收循环：按 \n 切帧，容忍半包/粘包/超长帧 */
        {
            char recvbuf[RECV_CHUNK];
            char line[PROTO_FRAME_MAX + 2];
            size_t line_len = 0;
            int overflow = 0;
            ssize_t n;

            while (1) {
                n = recv(fd, recvbuf, sizeof(recvbuf), 0);
                if (n <= 0) {
                    break;   /* 对端关闭或出错 */
                }
                for (ssize_t i = 0; i < n; i++) {
                    char ch = recvbuf[i];
                    if (ch == '\n') {
                        line[line_len] = '\0';
                        if (overflow) {
                            printf("[tcp] 丢弃超长帧（>%d 字节）\n", PROTO_FRAME_MAX);
                        } else if (line_len > 0) {
                            process_frame(c, line, line_len);
                        }
                        line_len = 0;
                        overflow = 0;
                    } else if (!overflow) {
                        if (line_len < PROTO_FRAME_MAX) {
                            line[line_len++] = ch;
                        } else {
                            overflow = 1;
                            printf("[tcp] 帧超长，丢弃\n");
                        }
                    }
                    /* overflow 状态下继续丢弃，直到遇到 \n */
                }
            }
        }

        /* 连接断开：先取发送锁，避免与进行中的 send 竞争 fd 的关闭 */
        pthread_mutex_lock(&c->send_lock);
        pthread_mutex_lock(&c->lock);
        c->connected = 0;
        if (c->sockfd == fd) {
            c->sockfd = -1;
        }
        pthread_mutex_unlock(&c->lock);
        close(fd);
        pthread_mutex_unlock(&c->send_lock);
        printf("[tcp] 连接断开，准备重连\n");
    }
    return NULL;
}

tcp_client_t *tcp_client_create(const char *ip, int port)
{
    tcp_client_t *c = calloc(1, sizeof(*c));
    if (!c) {
        return NULL;
    }
    snprintf(c->ip, sizeof(c->ip), "%s", ip ? ip : "127.0.0.1");
    c->port = port;
    c->sockfd = -1;
    pthread_mutex_init(&c->lock, NULL);
    pthread_mutex_init(&c->send_lock, NULL);
    return c;
}

/* 注册帧回调（每收到一帧 JSON 行调用） */
void tcp_client_set_frame_cb(tcp_client_t *c, tcp_frame_cb_t cb, void *user)
{
    c->on_frame = cb;
    c->user = user;
}

/* 启动：创建连接线程，断线自动重连（退避 1s..30s） */
int tcp_client_start(tcp_client_t *c)
{
    pthread_mutex_lock(&c->lock);
    c->running = 1;
    pthread_mutex_unlock(&c->lock);
    if (pthread_create(&c->thread, NULL, network_thread, c) != 0) {
        pthread_mutex_lock(&c->lock);
        c->running = 0;
        pthread_mutex_unlock(&c->lock);
        return -1;
    }
    pthread_mutex_lock(&c->lock);
    c->started = 1;
    pthread_mutex_unlock(&c->lock);
    return 0;
}

/* 发送整帧（自动追加 \n），发送失败返回 -1 */
int tcp_client_send(tcp_client_t *c, const char *buf, size_t len)
{
    int fd, connected, rc = 0;
    size_t sent = 0;

    if (!c || !buf || len == 0) {
        return -1;
    }

    pthread_mutex_lock(&c->send_lock);
    pthread_mutex_lock(&c->lock);
    fd = c->sockfd;
    connected = c->connected;
    pthread_mutex_unlock(&c->lock);

    if (fd < 0 || !connected) {
        pthread_mutex_unlock(&c->send_lock);
        return -1;
    }

    while (sent < len) {
        ssize_t n = send(fd, buf + sent, len - sent, 0);
        if (n <= 0) {
            rc = -1;
            break;
        }
        sent += (size_t)n;
    }
    pthread_mutex_unlock(&c->send_lock);
    return rc;
}

/* 查询是否已连接（供主循环判断在线状态） */
int tcp_client_is_connected(tcp_client_t *c)
{
    int r;
    pthread_mutex_lock(&c->lock);
    r = c->connected;
    pthread_mutex_unlock(&c->lock);
    return r;
}

/* 停止连接线程（置停止标志并等待线程退出） */
void tcp_client_stop(tcp_client_t *c)
{
    int fd = -1;
    int should_join = 0;

    if (!c) {
        return;
    }
    pthread_mutex_lock(&c->lock);//互斥锁保护
    if (c->started) {
        c->running = 0;
        fd = c->sockfd;
        c->sockfd = -1;
        c->connected = 0;
        c->started = 0;
        should_join = 1;
    }
    pthread_mutex_unlock(&c->lock);//中间临界区

    if (fd >= 0) {
        shutdown(fd, SHUT_RDWR);   /* 解除 recv 阻塞 */
    }
    if (should_join) {
        pthread_join(c->thread, NULL);
    }
}

/* 销毁客户端：停止、关闭 fd、释放内存 */
void tcp_client_destroy(tcp_client_t *c)
{
    if (!c) {
        return;
    }
    tcp_client_stop(c);
    pthread_mutex_destroy(&c->send_lock);
    pthread_mutex_destroy(&c->lock);
    free(c);
}
