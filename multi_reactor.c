/**
 * Multi-Reactor（主从Reactor）模式
 *
 * 架构：
 *   Main Reactor（主线程）
 *     - 只负责 accept 新连接
 *     - 通过 pipe 将 clientfd 轮询分发给各 Sub Reactor
 *
 *   Sub Reactor（工作线程，SUB_REACTOR_NUM 个）
 *     - 每个独立的 epfd + connlist + 线程
 *     - 监听自己 pipe 的读端，接收 Main Reactor 派来的新 fd
 *     - 负责处理 clientfd 的 EPOLLIN/EPOLLOUT（recv/send）
 *
 *  Main Thread          Sub Thread 0          Sub Thread 1
 *  ─────────────        ─────────────         ─────────────
 *  epoll_wait           epoll_wait            epoll_wait
 *  │ listenfd EPOLLIN   │ pipe[0] EPOLLIN     │ pipe[0] EPOLLIN
 *  └► accept_cb         │ └► 注册 clientfd    │ └► 注册 clientfd
 *     └► write(pipe[1]) │ clientfd EPOLLIN    │ clientfd EPOLLIN
 *        round-robin ───┘ └► recv_cb          └► recv_cb
 *                          clientfd EPOLLOUT
 *                          └► send_cb
 *
 * 编译：gcc -o multi_reactor multi_reactor.c -lpthread
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/epoll.h>

#define BUFFER_LENGTH    1024
#define MAX_CONN         1024
#define SUB_REACTOR_NUM  4

// 回调函数签名：fd + 所属 reactor 指针（与 reactor.c 的区别：多了 void *arg）
typedef int (*CALLBACK)(int fd, void *arg);

// ──────────────────────────────────────────
// 数据结构
// ──────────────────────────────────────────

struct conn_item {
    int fd;
    char buffer[BUFFER_LENGTH];
    int  idx;
    union {
        CALLBACK accept_callback;
        CALLBACK recv_callback;
    } recv_t;
    CALLBACK send_callback;
    void *arg;  // 回调执行时透传给回调的上下文（指向所属的 sub_reactor）
};

// Sub Reactor：工作线程，每个独立运行一个事件循环
struct sub_reactor {
    int epfd;
    int pipefd[2];                  // pipefd[0] 读端（sub监听），pipefd[1] 写端（main写入）
    struct conn_item connlist[MAX_CONN];
    pthread_t tid;
};

// Main Reactor：主线程，只处理 accept
struct main_reactor {
    int epfd;
    struct conn_item connlist[MAX_CONN];  // 仅用于 listenfd
    struct sub_reactor subs[SUB_REACTOR_NUM];
    int robin;  // 轮询计数器
};

static struct main_reactor g_main;

// ──────────────────────────────────────────
// 工具函数
// ──────────────────────────────────────────

static void set_event(int epfd, int fd, int event, int flag)
{
    struct epoll_event ev;
    ev.events  = event;
    ev.data.fd = fd;
    if (flag)
        epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev);
    else
        epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &ev);
}

// ──────────────────────────────────────────
// Sub Reactor 的回调
// ──────────────────────────────────────────

int recv_cb(int fd, void *arg);
int send_cb(int fd, void *arg);

/*
 * pipe_cb：Sub Reactor 从 pipe 读端收到新 clientfd
 *
 * Main Reactor 在 accept 后将 clientfd 写入 pipe[1]，
 * Sub Reactor 的 epoll 监听到 pipe[0] EPOLLIN 后调用此函数，
 * 从管道读出 fd，初始化 conn_item，注册到自己的 epoll。
 */
static int pipe_cb(int pipefd, void *arg)
{
    struct sub_reactor *sub = (struct sub_reactor *)arg;
    int clientfd = 0;

    if (read(pipefd, &clientfd, sizeof(int)) <= 0)
        return -1;

    // 将 clientfd 注册到当前 sub reactor 的 epoll，监听读事件
    set_event(sub->epfd, clientfd, EPOLLIN, 1);

    // 初始化连接项
    sub->connlist[clientfd].fd  = clientfd;
    sub->connlist[clientfd].idx = 0;
    memset(sub->connlist[clientfd].buffer, 0, BUFFER_LENGTH);
    sub->connlist[clientfd].recv_t.recv_callback = recv_cb;
    sub->connlist[clientfd].send_callback        = send_cb;
    sub->connlist[clientfd].arg                  = sub;  // 绑定所属 sub reactor

    return clientfd;
}

/*
 * recv_cb：clientfd EPOLLIN 触发
 *
 * 读取数据后，将事件切换为 EPOLLOUT，等待发送。
 */
int recv_cb(int fd, void *arg)
{
    struct sub_reactor *sub = (struct sub_reactor *)arg;
    char *buffer = sub->connlist[fd].buffer;
    int   idx    = sub->connlist[fd].idx;

    int count = recv(fd, buffer + idx, BUFFER_LENGTH - idx, 0);
    if (count == 0) {
        // 对端关闭连接
        epoll_ctl(sub->epfd, EPOLL_CTL_DEL, fd, NULL);
        close(fd);
        sub->connlist[fd].fd = -1;
        printf("[sub%td] client %d disconnected\n", sub - g_main.subs, fd);
        return -1;
    }

    sub->connlist[fd].idx += count;

    // 切换为监听写事件
    set_event(sub->epfd, fd, EPOLLOUT, 0);

    return count;
}

/*
 * send_cb：clientfd EPOLLOUT 触发
 *
 * 将缓冲区数据发回客户端，清空缓冲区，切换回 EPOLLIN。
 */
int send_cb(int fd, void *arg)
{
    struct sub_reactor *sub = (struct sub_reactor *)arg;
    char *buffer = sub->connlist[fd].buffer;
    int   idx    = sub->connlist[fd].idx;

    int count = send(fd, buffer, idx, 0);

    // 清空缓冲区，准备下一轮
    sub->connlist[fd].idx = 0;
    memset(buffer, 0, BUFFER_LENGTH);

    // 切换回监听读事件
    set_event(sub->epfd, fd, EPOLLIN, 0);

    return count;
}

// ──────────────────────────────────────────
// Main Reactor 的回调
// ──────────────────────────────────────────

/*
 * accept_cb：listenfd EPOLLIN 触发
 *
 * accept 新连接后，通过轮询（round-robin）选一个 Sub Reactor，
 * 将 clientfd 写入其 pipe[1]，由 Sub Reactor 负责后续读写。
 */
static int accept_cb(int fd, void *arg)
{
    struct sockaddr_in client_addr;
    socklen_t len = sizeof(client_addr);
    int clientfd = accept(fd, (struct sockaddr *)&client_addr, &len);
    if (clientfd < 0) {
        perror("accept");
        return -1;
    }

    // 轮询选择 Sub Reactor
    int idx = (g_main.robin++) % SUB_REACTOR_NUM;
    struct sub_reactor *sub = &g_main.subs[idx];

    // 通过 pipe 通知 Sub Reactor 有新连接
    write(sub->pipefd[1], &clientfd, sizeof(int));

    printf("[main] accept clientfd=%d → sub_reactor[%d]\n", clientfd, idx);
    return clientfd;
}

// ──────────────────────────────────────────
// Sub Reactor 线程入口
// ──────────────────────────────────────────

static void *sub_reactor_run(void *arg)
{
    struct sub_reactor *sub = (struct sub_reactor *)arg;
    struct epoll_event events[MAX_CONN];

    printf("[sub%td] thread started, epfd=%d\n", sub - g_main.subs, sub->epfd);

    while (1) {
        int nready = epoll_wait(sub->epfd, events, MAX_CONN, -1);
        for (int i = 0; i < nready; i++) {
            int fd = events[i].data.fd;

            if (fd == sub->pipefd[0]) {
                // Main Reactor 分配来了新连接
                int clientfd = pipe_cb(fd, sub);
                if (clientfd > 0)
                    printf("[sub%td] registered clientfd=%d\n",
                           sub - g_main.subs, clientfd);

            } else if (events[i].events & EPOLLIN) {
                int count = sub->connlist[fd].recv_t.recv_callback(
                                fd, sub->connlist[fd].arg);
                if (count > 0)
                    printf("[sub%td] recv <---- fd=%d: %.*s\n",
                           sub - g_main.subs, fd,
                           sub->connlist[fd].idx, sub->connlist[fd].buffer);

            } else if (events[i].events & EPOLLOUT) {
                printf("[sub%td] send ----> fd=%d\n", sub - g_main.subs, fd);
                sub->connlist[fd].send_callback(fd, sub->connlist[fd].arg);
            }
        }
    }

    return NULL;
}

// ──────────────────────────────────────────
// main
// ──────────────────────────────────────────

int main()
{
    // ── 创建监听 socket ──────────────────────
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

    // ── 初始化 Main Reactor ──────────────────
    memset(&g_main, 0, sizeof(g_main));
    g_main.epfd = epoll_create1(EPOLL_CLOEXEC);
    if (g_main.epfd < 0) { perror("epoll_create1"); return -1; }

    // listenfd 绑定 accept_cb，注册到 main epoll
    g_main.connlist[sockfd].fd = sockfd;
    g_main.connlist[sockfd].recv_t.accept_callback = accept_cb;
    set_event(g_main.epfd, sockfd, EPOLLIN, 1);

    // ── 初始化并启动 Sub Reactors ────────────
    for (int i = 0; i < SUB_REACTOR_NUM; i++) {
        struct sub_reactor *sub = &g_main.subs[i];
        memset(sub, 0, sizeof(struct sub_reactor));

        sub->epfd = epoll_create1(EPOLL_CLOEXEC);
        if (sub->epfd < 0) { perror("epoll_create1"); return -1; }

        // 创建 pipe，用于 main → sub 传递新连接
        if (pipe(sub->pipefd) < 0) { perror("pipe"); return -1; }

        // 将 pipe 读端注册到 sub 的 epoll
        set_event(sub->epfd, sub->pipefd[0], EPOLLIN, 1);

        // 启动工作线程
        pthread_create(&sub->tid, NULL, sub_reactor_run, sub);
    }

    printf("[main] listening on :2048, %d sub reactors\n", SUB_REACTOR_NUM);

    // ── Main Reactor 事件循环（只处理 accept）──
    struct epoll_event events[MAX_CONN];
    while (1) {
        int nready = epoll_wait(g_main.epfd, events, MAX_CONN, -1);
        for (int i = 0; i < nready; i++) {
            int fd = events[i].data.fd;
            if (events[i].events & EPOLLIN) {
                g_main.connlist[fd].recv_t.accept_callback(fd, NULL);
            }
        }
    }

    return 0;
}
