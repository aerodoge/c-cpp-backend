# Multi-Reactor（主从Reactor）详解

## 目录

1. [为什么需要 Multi-Reactor](#1-为什么需要-multi-reactor)
2. [架构设计](#2-架构设计)
3. [核心问题：如何跨线程传递 fd](#3-核心问题如何跨线程传递-fd)
4. [multi_reactor.c 代码解析](#4-multi_reactorc-代码解析)
5. [与 reactor.c 的对比](#5-与-reactorc-的对比)
6. [线程安全分析](#6-线程安全分析)
7. [局限性与进一步演进](#7-局限性与进一步演进)

---

## 1. 为什么需要 Multi-Reactor

单 Reactor（`reactor.c`）运行在单线程中，有一个根本限制：

```
单线程 Reactor：
  epoll_wait → accept_cb → recv_cb → send_cb → epoll_wait → ...
                                ↑
              如果 recv_cb 中有耗时操作（数据库查询、文件读写）
              整个事件循环被阻塞，所有其他连接都无法响应
```

另外，单线程无法利用多核 CPU：

```
4核服务器 + 单Reactor = 只用了 1/4 的算力
```

Multi-Reactor 的解法：**分工**

- **Main Reactor（主线程）**：只做 accept，轻量，不阻塞
- **Sub Reactor（工作线程×N）**：各自独立处理一批连接的读写

```
Main Thread          Sub Thread 0         Sub Thread 1         Sub Thread 2
─────────────        ────────────         ────────────         ────────────
accept only          处理连接 A、D、G     处理连接 B、E、H     处理连接 C、F、I
```

4 核机器开 4 个 Sub Reactor，4 个核都能跑满。

---

## 2. 架构设计

```
                        ┌─────────────────────────────────────────┐
                        │             Main Reactor（主线程）        │
                        │  epfd_main 只监听 listenfd               │
                        │  accept_cb → 轮询 → write(pipe[1], fd)   │
                        └──────┬──────────┬──────────┬────────────┘
                               │ pipe     │ pipe     │ pipe
                               ▼          ▼          ▼
              ┌────────────────┐ ┌────────────────┐ ┌────────────────┐
              │  Sub Reactor 0 │ │  Sub Reactor 1 │ │  Sub Reactor 2 │
              │  epfd_0        │ │  epfd_1        │ │  epfd_2        │
              │  connlist[fd]  │ │  connlist[fd]  │ │  connlist[fd]  │
              │  Thread T0     │ │  Thread T1     │ │  Thread T2     │
              │                │ │                │ │                │
              │  pipe[0] ──►   │ │  pipe[0] ──►   │ │  pipe[0] ──►   │
              │    pipe_cb     │ │    pipe_cb     │ │    pipe_cb     │
              │  clientfd ──►  │ │  clientfd ──►  │ │  clientfd ──►  │
              │    recv_cb     │ │    recv_cb     │ │    recv_cb     │
              │    send_cb     │ │    send_cb     │ │    send_cb     │
              └────────────────┘ └────────────────┘ └────────────────┘
```

关键设计原则：
- Main Reactor 和 Sub Reactor **各自拥有独立的 epfd**
- Sub Reactor 拥有**独立的 connlist**，无需跨线程访问
- 主从之间通过 **pipe** 传递新连接，解耦且无锁

---

## 3. 核心问题：如何跨线程传递 fd

这是 Multi-Reactor 设计的核心问题。有两种方案：

### 方案一：直接调用 epoll_ctl（可行但有隐患）

```c
// Main 线程中：
int idx = robin++ % SUB_REACTOR_NUM;
struct sub_reactor *sub = &g_main.subs[idx];

// 直接往 sub 的 epfd 上注册
epoll_ctl(sub->epfd, EPOLL_CTL_ADD, clientfd, &ev);  // epoll_ctl 是线程安全的

// 但是！connlist 的初始化呢？
sub->connlist[clientfd].recv_t.recv_callback = recv_cb;  // ← 主线程写
// 同时子线程也在读 connlist[clientfd]                   // ← 子线程读
// 竞态条件！
```

`epoll_ctl` 本身是线程安全的（内核加了锁），但初始化 `connlist` 不是原子操作，主线程写 `connlist` 和子线程读 `connlist` 之间存在竞态。解决方法是加锁，但引入了锁复杂度。

### 方案二：通过 pipe 传递（multi_reactor.c 的做法）

```c
// Main 线程：只写 fd 到管道，不碰 connlist
write(sub->pipefd[1], &clientfd, sizeof(int));

// Sub 线程：从管道读 fd，在自己线程内初始化 connlist
// pipe_cb 在 sub 线程的事件循环中执行
static int pipe_cb(int pipefd, void *arg) 
{
    struct sub_reactor *sub = (struct sub_reactor *)arg;
    int clientfd;
    read(pipefd, &clientfd, sizeof(int));

    // connlist 的初始化完全在 sub 线程内完成 → 无竞态
    sub->connlist[clientfd].recv_t.recv_callback = recv_cb;
    sub->connlist[clientfd].send_callback = send_cb;
    sub->connlist[clientfd].arg = sub;

    set_event(sub->epfd, clientfd, EPOLLIN, 1);
    return clientfd;
}
```

pipe 的作用：**将"初始化 connlist + 注册 epoll"这两步操作，从主线程转移到子线程执行**，天然避免竞态，无需加锁。

```
时间线：
Main:  accept → write(pipe) ─────────────────────────────────
Sub:   epoll_wait ─────────────── pipe EPOLLIN → pipe_cb
                                               ↑
                            在这里初始化 connlist 并注册 epoll
                            全程在 sub 线程内，安全
```

pipe 还有一个额外好处：pipe 本身是一个 fd，可以直接被 epoll 监听，不需要额外的同步机制，和事件循环完美融合。

---

## 4. multi_reactor.c 代码解析

### 4.1 数据结构

```c
// 回调函数签名：比 reactor.c 多了 void *arg（指向所属 sub_reactor）
typedef int (*CALLBACK)(int fd, void *arg);

struct conn_item 
{
    int fd;
    char buffer[BUFFER_LENGTH];
    int  idx;
    union {
        CALLBACK accept_callback;
        CALLBACK recv_callback;
    } recv_t;
    CALLBACK send_callback;
    void *arg;   // ← 新增：透传给回调的上下文（sub_reactor 指针）
};

struct sub_reactor 
{
    int epfd;
    int pipefd[2];              // [0] 读端（sub监听），[1] 写端（main写入）
    struct conn_item connlist[MAX_CONN];
    pthread_t tid;
};

struct main_reactor 
{
    int epfd;
    struct conn_item connlist[MAX_CONN];  // 只用于 listenfd
    struct sub_reactor subs[SUB_REACTOR_NUM];
    int robin;                  // 轮询计数器
};
```

与 reactor.c 的结构差异：
- `struct reactor` 现在被真正使用（而不是只声明）
- `conn_item` 增加了 `void *arg`，让回调知道自己属于哪个 sub reactor
- 每个 sub reactor 独立持有自己的 `connlist`，不再共享全局数组

### 4.2 初始化流程

```c
// 1. 主 epfd，监听 listenfd
g_main.epfd = epoll_create1(EPOLL_CLOEXEC);
set_event(g_main.epfd, sockfd, EPOLLIN, 1);

// 2. 初始化每个 sub reactor
for (int i = 0; i < SUB_REACTOR_NUM; i++) 
{
    sub->epfd = epoll_create1(EPOLL_CLOEXEC);  // 独立 epfd
    pipe(sub->pipefd);                          // 创建 pipe
    set_event(sub->epfd, sub->pipefd[0], EPOLLIN, 1);  // 监听 pipe 读端
    pthread_create(&sub->tid, NULL, sub_reactor_run, sub);  // 启动线程
}
```

整个系统有 `1 + SUB_REACTOR_NUM` 个 epfd，互相独立。

### 4.3 accept_cb：轮询分发

```c
static int accept_cb(int fd, void *arg) 
{
    int clientfd = accept(fd, ...);

    // round-robin：0, 1, 2, ..., N-1, 0, 1, 2, ...
    int idx = (g_main.robin++) % SUB_REACTOR_NUM;
    struct sub_reactor *sub = &g_main.subs[idx];

    // 只写 fd，不碰 connlist
    write(sub->pipefd[1], &clientfd, sizeof(int));
    return clientfd;
}
```

轮询保证各 Sub Reactor 的连接数大致均衡。实际项目中也可以选最少连接数的 Sub Reactor（Least Connections 策略）。

### 4.4 sub_reactor_run：工作线程事件循环

```c
static void *sub_reactor_run(void *arg) 
{
    struct sub_reactor *sub = (struct sub_reactor *)arg;
    struct epoll_event events[MAX_CONN];

    while (1) 
    {
        int nready = epoll_wait(sub->epfd, events, MAX_CONN, -1);
        for (int i = 0; i < nready; i++) 
        {
            int fd = events[i].data.fd;

            if (fd == sub->pipefd[0]) 
            {
                // Main 分配来了新连接
                pipe_cb(fd, sub);

            } 
            else if (events[i].events & EPOLLIN) 
            {
                sub->connlist[fd].recv_t.recv_callback(fd, sub->connlist[fd].arg);

            } 
            else if (events[i].events & EPOLLOUT) 
            {
                sub->connlist[fd].send_callback(fd, sub->connlist[fd].arg);
            }
        }
    }
}
```

Sub Reactor 的事件循环和单 Reactor 几乎相同，额外增加了对 `pipefd[0]` 的判断。

### 4.5 Main Reactor 事件循环：极度精简

```c
while (1) 
{
    int nready = epoll_wait(g_main.epfd, events, MAX_CONN, -1);
    for (int i = 0; i < nready; i++) 
    {
        int fd = events[i].data.fd;
        if (events[i].events & EPOLLIN) 
        {
            // 主循环只可能触发 accept_cb（只注册了 listenfd）
            g_main.connlist[fd].recv_t.accept_callback(fd, NULL);
        }
    }
}
```

主线程的事件循环极其轻量，绝不会阻塞，accept 延迟极低。

---

## 5. 与 reactor.c 的对比

|             | reactor.c          | multi_reactor.c               |
|-------------|--------------------|-------------------------------|
| 线程数         | 1                  | 1 + SUB_REACTOR_NUM           |
| epfd数量      | 1                  | 1 + SUB_REACTOR_NUM           |
| connlist    | 全局共享               | 每个sub独立                       |
| accept后如何处理 | 直接注册到当前epfd        | write pipe → sub线程注册          |
| 回调签名        | `CALLBACK(int fd)` | `CALLBACK(int fd, void *arg)` |
| CPU利用       | 单核                 | 多核                            |
| 线程安全        | 无需考虑               | connlist各自独立，天然安全             |
| 适合场景        | 中低并发，逻辑简单          | 高并发，充分利用多核                    |

---

## 6. 线程安全分析

Multi-Reactor 中，哪些数据在多线程间共享？

```
g_main.robin        ← 主线程读写，理论上有竞态
                      但 int 的自增在 x86 上通常不会出问题
                      严格要求可改为 atomic_fetch_add

g_main.subs[i]      ← 主线程只读（初始化后不修改结构体本身）
                         只写 pipefd[1]（管道写是原子的，<= PIPE_BUF 字节）

sub->connlist[fd]   ← 仅由 sub 自己的线程读写，无共享
                      因为 clientfd 通过 pipe 传入后，
                      只有对应 sub 线程会操作它
```

**结论：`multi_reactor.c` 的设计几乎不需要显式加锁。** pipe 的使用不只是传递数据，更是一种同步机制：通过 pipe 保证 `connlist` 的初始化发生在 sub 线程中，从根本上避免了竞态。

---

## 7. 局限性与进一步演进

### 当前版本的局限

回调函数（`recv_cb`、`send_cb`）仍然运行在 Sub Reactor 的事件循环线程中。如果业务逻辑耗时（如查询数据库），会阻塞整个 Sub Reactor，影响它管理的所有连接。

### 进一步演进：加入业务线程池

```
Main Reactor（accept）
    │ pipe
    ▼
Sub Reactor 0（I/O 读写）─────► 业务线程池（数据库、业务计算）
Sub Reactor 1（I/O 读写）─────►
Sub Reactor 2（I/O 读写）─────►
```

- Sub Reactor 只做 recv/send，读完数据后将任务投递到线程池
- 线程池处理完后，结果写回 Sub Reactor 的 pipe，触发 send
- Sub Reactor 永不阻塞，I/O 吞吐量最大化

这就是 **Netty** 和 **现代 Nginx** 的完整架构。
