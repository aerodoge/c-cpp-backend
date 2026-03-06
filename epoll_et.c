/**
 * epoll 边缘触发（Edge Triggered）示例
 *
 * 演示 ET 模式的正确用法：
 *   1. fd 必须设置为非阻塞（O_NONBLOCK）
 *   2. EPOLLIN 触发后必须循环 recv 直到 EAGAIN，否则会漏读数据
 *
 * 对比 epoll_server.c（LT模式）：
 *   LT：缓冲区有数据就触发，读多少无所谓，下次还会触发
 *   ET：状态变化时触发一次，必须一次性读完，否则不再通知
 *
 * 编译：gcc -o epoll_et epoll_et.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/epoll.h>

#define BUFFER_LENGTH  1024
#define MAX_EVENTS     1024

// 将 fd 设置为非阻塞
static int set_nonblock(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        perror("fcntl F_GETFL");
        return -1;
    }
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        perror("fcntl F_SETFL");
        return -1;
    }
    return 0;
}

int main()
{
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) { perror("socket"); return -1; }

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family      = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_port        = htons(2048);

    int opt = 1;
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    if (bind(sockfd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("bind"); return -1;
    }
    listen(sockfd, 10);

    // ── listenfd 也设置非阻塞 ──────────────────────────────────────────
    // ET 模式下 listenfd 同样需要非阻塞，并在 accept_cb 中循环 accept
    // 直到 EAGAIN，避免遗漏并发到来的多个连接
    set_nonblock(sockfd);

    int epfd = epoll_create1(EPOLL_CLOEXEC);
    if (epfd < 0) { perror("epoll_create1"); return -1; }

    // listenfd 注册 ET 模式
    struct epoll_event ev;
    ev.events  = EPOLLIN | EPOLLET;   // ← EPOLLET 开启边缘触发
    ev.data.fd = sockfd;
    epoll_ctl(epfd, EPOLL_CTL_ADD, sockfd, &ev);

    struct epoll_event events[MAX_EVENTS];

    while (1) {
        int nready = epoll_wait(epfd, events, MAX_EVENTS, -1);
        if (nready < 0) {
            if (errno == EINTR) continue;   // 被信号打断，重试
            perror("epoll_wait");
            break;
        }

        for (int i = 0; i < nready; i++) {
            int fd = events[i].data.fd;

            // ── 新连接 ───────────────────────────────────────────────
            if (fd == sockfd) {
                /*
                 * ET 模式：listenfd EPOLLIN 只触发一次
                 * 如果同时来了多个连接，必须循环 accept 直到 EAGAIN
                 * 否则剩余的连接在下次数据到来之前都不会再触发
                 */
                while (1) {
                    struct sockaddr_in client_addr;
                    socklen_t len = sizeof(client_addr);
                    int clientfd = accept(sockfd,
                                         (struct sockaddr *)&client_addr,
                                         &len);
                    if (clientfd < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) {
                            // 所有等待的连接都 accept 完了
                            break;
                        }
                        perror("accept");
                        break;
                    }

                    // clientfd 必须设置非阻塞，ET 模式的强制要求
                    set_nonblock(clientfd);

                    // 注册 clientfd，ET + EPOLLIN
                    ev.events  = EPOLLIN | EPOLLET;
                    ev.data.fd = clientfd;
                    epoll_ctl(epfd, EPOLL_CTL_ADD, clientfd, &ev);

                    printf("new connection: fd=%d\n", clientfd);
                }

            // ── 可读事件 ─────────────────────────────────────────────
            } else if (events[i].events & EPOLLIN) {
                /*
                 * ET 模式：缓冲区从空变非空时触发一次
                 * 如果不一次读完，剩余数据不会再触发 EPOLLIN
                 * 必须循环 recv 直到返回 EAGAIN
                 *
                 * 对比 LT：LT 只要缓冲区有数据就持续触发，可以分多次读
                 */
                char buffer[BUFFER_LENGTH];
                int  total = 0;

                while (1) {
                    int count = recv(fd, buffer + total,
                                     BUFFER_LENGTH - total, 0);
                    if (count < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) {
                            // 缓冲区已读空，本次 ET 事件处理完毕
                            break;
                        }
                        if (errno == EINTR) continue;   // 信号打断，重试
                        perror("recv");
                        // 读出错，关闭连接
                        epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
                        close(fd);
                        total = -1;
                        break;
                    }
                    if (count == 0) {
                        // 对端关闭连接
                        epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
                        close(fd);
                        printf("client %d disconnected\n", fd);
                        total = -1;
                        break;
                    }
                    total += count;
                    if (total >= BUFFER_LENGTH) {
                        // 缓冲区满，先处理已有数据，下次继续读
                        // 实际项目应使用动态缓冲区或分帧协议
                        break;
                    }
                }

                if (total > 0) {
                    // echo 回去
                    send(fd, buffer, total, 0);
                    printf("recv <---- fd=%d (%d bytes): %.*s\n",
                           fd, total, total, buffer);
                }

            // ── 错误/断开 ─────────────────────────────────────────────
            } else if (events[i].events & (EPOLLERR | EPOLLHUP)) {
                epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
                close(fd);
                printf("client %d error/hangup\n", fd);
            }
        }
    }

    close(epfd);
    close(sockfd);
    return 0;
}
