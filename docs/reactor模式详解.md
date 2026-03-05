# Reactor 模式详解

## 目录

1. [什么是Reactor模式](#1-什么是-reactor-模式)
2. [为什么需要Reactor模式](#2-为什么需要-reactor-模式)
3. [核心组件](#3-核心组件)
4. [工作原理](#4-工作原理)
5. [reactor.c代码逐行解析](#5-reactorc-代码逐行解析)
6. [与epoll_server.c的对比](#6-与-epoll_serverc-的对比)
7. [事件流转图](#7-事件流转图)
8. [理解Reactor的关键思想](#8-理解-reactor-的关键思想)
9. [Reactor的变种](#9-reactor-的变种)
10. [常见框架中的Reactor](#10-常见框架中的-reactor)

---

## 1. 什么是 Reactor 模式

Reactor（反应堆）是一种**事件驱动**的设计模式，用于处理并发I/O事件。

核心思想：**不主动等待，而是被动响应**。

- 传统阻塞模型：程序主动调用`read()`，一直等到数据到来，期间什么都不能做。
- Reactor模型：程序把fd注册到事件分发器，然后去做其他事。当fd就绪时，事件分发器通知程序，程序再去处理。

```
传统阻塞:
  程序 ──► read() ──► [阻塞等待] ──► 数据到达 ──► 处理

Reactor:
  程序 ──► 注册回调 ──► 去干别的
              ↑
  fd就绪 ──► 事件分发器 ──► 调用回调 ──► 处理
```

---

## 2. 为什么需要Reactor模式

### 问题起点：高并发连接

假设服务器要同时处理10000个客户端连接：

**方案一：每个连接一个线程**

- 缺点：线程开销大（每个线程8MB栈），10000个线程 = 80GB 内存
- 上下文切换开销极高

**方案二：单线程轮询**

- 缺点：逐个检查每个fd是否就绪，效率 O(n)，大量无效检查

**方案三：Reactor（事件驱动 + I/O多路复用）**

- 用epoll/select等系统调用，内核负责监听所有fd
- 只有真正就绪的fd才会通知到用户空间
- 单线程可以处理大量并发连接，效率O(1)（epoll红黑树实现）

---

## 3. 核心组件

Reactor模式由以下角色组成：

```
+------------------+       注册/删除        +---------------------+
|   Handle (fd)    | ───────────────────►  |  Event Demultiplexer|
|  (文件描述符)     |                       |  (事件分发器: epoll)  |
+------------------+                       +---------------------+
                                                    │ 事件就绪
                                                    ▼
+------------------+       dispatch        +---------------------+
|  Event Handler   | ◄───────────────────  |    Dispatcher       |
|  (事件处理器/回调) |                       |    (事件循环)        |
+------------------+                       +---------------------+
```

| 角色                         | 说明                    | 在 reactor.c 中对应                   |
|----------------------------|-----------------------|-----------------------------------|
| **Handle**                 | 操作系统资源句柄，即fd          | `int fd`                          |
| **Event Demultiplexer**    | I/O多路复用器，等待事件发生       | `epoll_wait()`                    |
| **Event Handler**          | 事件处理回调函数              | `accept_cb`, `recv_cb`, `send_cb` |
| **Dispatcher**             | 事件循环，将事件分发给对应 Handler | `while(1)` 主循环                    |
| **Concrete Event Handler** | 具体的处理逻辑               | `conn_item.recv_t.recv_callback`  |

---

## 4. 工作原理

Reactor的工作流程分为两个阶段：**注册阶段**和**运行阶段**。

### 注册阶段

```
1. 创建listenfd，绑定端口
2. 创建epoll实例 (epfd)
3. 将listenfd注册到epoll，监听EPOLLIN事件
4. 为listenfd绑定回调函数accept_cb
```

### 运行阶段（事件循环）

```
while(1) {
    nready = epoll_wait(epfd, events, ...)   // 阻塞等待事件
    for each event:
        取出 fd
        if EPOLLIN:
            调用该fd的recv_callback(fd)
        if EPOLLOUT:
            调用该fd的 end_callback(fd)
}
```

### 回调的级联触发

```
listenfd EPOLLIN
    └─► accept_cb(listenfd)
            ├── accept()得到 clientfd
            ├── 将clientfd注册到epoll，监听 EPOLLIN
            └── 绑定clientfd的回调为recv_cb

clientfd EPOLLIN
    └─► recv_cb(clientfd)
            ├── recv()读取数据
            └── 修改clientfd监听EPOLLOUT（切换为写事件）

clientfd EPOLLOUT
    └─► send_cb(clientfd)
            ├── send()发送数据
            └── 修改clientfd监听EPOLLIN（切换回读事件）
```

**这就是"反应"的含义**：每个事件触发一个回调，回调可以改变监听的事件类型，从而驱动下一步逻辑。

---

## 5. reactor.c 代码逐行解析

### 5.1 数据结构

```c
typedef int(*CALLBACK)(int fd);   // 回调函数类型：接收fd，返回int

struct conn_item {
    int fd;                        // 连接的文件描述符
    char buffer[BUFFER_LENGTH];    // 读写缓冲区
    int idx;                       // 缓冲区当前使用长度
    union {
        CALLBACK accept_callback;  // listenfd使用：accept回调
        CALLBACK recv_callback;    // clientfd使用：读数据回调
    } recv_t;                      // union节省内存：同一个fd只用其中一个
    CALLBACK send_callback;        // 写数据回调
};
```

`union`的用法很精妙：`listenfd`和`clientfd`都存在`connlist`中，但listenfd需要`accept_callback`，clientfd需要`recv_callback`，
两者不会同时使用，用union复用同一块内存。

```c
struct conn_item connlist[1024] = {0};  // 全局连接表，下标就是fd值
```

这是一个以 **fd 为下标**的数组。Linux 内核分配 fd 时从小到大递增，所以 `connlist[fd]` 直接就能找到对应连接，O(1) 查找，非常高效。

```c
struct reactor {
    int epfd;                    // epoll 实例 fd
    struct conn_item *conn_list; // 连接列表指针
};
```

`struct reactor` 是对 epfd 和连接表的封装（本文件中未完全使用，是设计雏形）。

### 5.2 set_event：注册/修改事件

```c
int set_event(int fd, int event, int flag)
{
    struct epoll_event ev;
    ev.events = event;    // 监听的事件类型，如 EPOLLIN、EPOLLOUT
    ev.data.fd = fd;      // 事件携带的数据，这里存 fd
    if (flag) 
    {
        epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev);  // flag=1: 新增
    } 
    else 
    {
        epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &ev);  // flag=0: 修改
    }
}
```

`epoll_ctl` 的三种操作：

- `EPOLL_CTL_ADD`：把fd加入epoll监听
- `EPOLL_CTL_MOD`：修改fd监听的事件类型
- `EPOLL_CTL_DEL`：从epoll中删除fd

### 5.3 accept_cb：处理新连接

```c
int accept_cb(int fd)
{
    // 1. 接受新连接
    int clientfd = accept(fd, (struct sockaddr*)&client_addr, &len);

    // 2. 将 clientfd 注册到 epoll，监听读事件
    set_event(clientfd, EPOLLIN, 1);

    // 3. 初始化连接项，绑定回调
    connlist[clientfd].fd = clientfd;
    connlist[clientfd].recv_t.recv_callback = recv_cb;
    connlist[clientfd].send_callback = send_cb;

    return clientfd;
}
```

每来一个新连接，就把它"入册"：注册到 epoll，并绑定好后续的读写回调。

### 5.4 recv_cb：处理读事件

```c
int recv_cb(int fd)
{
    int count = recv(fd, buffer+idx, BUFFER_LENGTH-idx, 0);
    if (count == 0) 
    {
        // 对端关闭连接
        epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
        close(fd);
        return -1;
    }
    connlist[fd].idx += count;

    // 关键：读完之后改为监听写事件
    set_event(fd, EPOLLOUT, 0);  // flag=0 表示 MOD，不是 ADD
    return count;
}
```

读完数据后，将事件从 `EPOLLIN` 切换为 `EPOLLOUT`，这样下一次循环就会触发 `send_cb`，实现"读完就写"的 Echo 逻辑。

### 5.5 send_cb：处理写事件

```c
int send_cb(int fd)
{
    int count = send(fd, buffer, idx, 0);

    // 发送完后改回监听读事件，等待下一条消息
    set_event(fd, EPOLLIN, 0);
    return count;
}
```

发完之后切换回 `EPOLLIN`，等待客户端的下一条数据。这样形成一个完整的读写循环。

### 5.6 主循环（Dispatcher）

```c
// 初始化
connlist[sockfd].recv_t.accept_callback = accept_cb;
epfd = epoll_create1(EPOLL_CLOEXEC);
set_event(sockfd, EPOLLIN, 1);

// 事件循环
while(1) 
{
    int nready = epoll_wait(epfd, events, 1024, -1);
    for (int i = 0; i < nready; i++) 
    {
        int connfd = events[i].data.fd;
        if (events[i].events & EPOLLIN) 
        {
            // 调用该fd对应的读回调（可能是accept_cb或recv_cb）
            connlist[connfd].recv_t.recv_callback(connfd);
        } 
        else if (events[i].events & EPOLLOUT) 
        {
            // 调用该fd对应的写回调
            connlist[connfd].send_callback(connfd);
        }
    }
}
```

主循环不关心"这个 fd 是 listenfd 还是 clientfd"，也不关心"要做什么"，只负责：

1. 等事件
2. 查表找回调
3. 调用回调

这就是 **Dispatcher 的职责分离**。

---

## 6. 与 epoll_server.c 的对比

`epoll_server.c` 是没有使用 Reactor 模式的普通 epoll 服务器：

```c
// epoll_server.c 的事件循环
if (sockfd == connfd) 
{
    // 直接硬编码 accept 逻辑
    clientfd = accept(sockfd, ...);
    epoll_ctl(epfd, EPOLL_CTL_ADD, clientfd, &ev);
} 
else if (events[i].events & EPOLLIN) 
{
    // 直接硬编码 recv/send 逻辑
    count = recv(connfd, buff, ...);
    send(connfd, buff, count, 0);
}
```

对比分析：

| 特性     | epoll_server.c          | reactor.c               |
|--------|-------------------------|-------------------------|
| 事件处理方式 | 硬编码 if/else             | 回调函数表                   |
| 扩展性    | 差，新事件类型需改主循环            | 好，只需绑定新回调               |
| 关注点分离  | 混杂在主循环中                 | Dispatcher 与 Handler 分离 |
| 代码结构   | 简单但不易维护                 | 结构清晰，可扩展                |
| fd区分方式 | `if (sockfd == connfd)` | 通过回调函数指针                |

**本质区别**：`epoll_server.c` 是面向过程的，主循环知道所有业务逻辑；`reactor.c` 是事件驱动的，主循环只做分发，业务逻辑封装在回调中。

---

## 7. 事件流转图

```
服务器启动
    │
    ▼
socket() + bind() + listen()
    │
    ▼
epoll_create1()
    │
    ▼
将 listenfd 注册到 epoll (EPOLLIN)
绑定 accept_cb
    │
    ▼
┌─────────────────────────────────────────┐
│              epoll_wait()               │  ◄── 阻塞在此
└─────────────────────────────────────────┘
    │
    ├── listenfd EPOLLIN ──► accept_cb(listenfd)
    │                              │
    │                              ├── accept() 得到 clientfd
    │                              ├── 注册 clientfd 到 epoll (EPOLLIN)
    │                              └── 绑定 recv_cb / send_cb
    │
    ├── clientfd EPOLLIN ──► recv_cb(clientfd)
    │                              │
    │                              ├── recv() 读数据
    │                              └── 修改 clientfd 为 EPOLLOUT
    │
    └── clientfd EPOLLOUT ──► send_cb(clientfd)
                                   │
                                   ├── send() 发数据
                                   └── 修改 clientfd 为 EPOLLIN
                                              │
                                              └──► 回到 epoll_wait()
```

---

## 8. 理解 Reactor 的关键思想

### 8.1 控制反转（IoC）

普通程序：**我去调用 I/O**（主动）

```c
while(1) {
    read(fd, buf, len);   // 我主动读
    process(buf);
}
```

Reactor：**I/O 来通知我**（被动响应）

```c
register_handler(fd, EPOLLIN, my_read_cb);  // 注册回调
// 之后什么都不用管，fd 就绪时系统会调用 my_read_cb
```

这是一种控制反转：不是你调用框架，而是框架调用你。

### 8.2 事件驱动本质

Reactor 的核心是**将时间维度转化为事件维度**：

- 不是"等 1 秒再检查"，而是"有事件发生就处理"
- 不是"轮询所有 fd"，而是"谁就绪处理谁"

内核的 epoll 红黑树维护所有监听 fd，就绪队列存储已就绪的 fd，`epoll_wait` 只返回就绪的，所以效率极高。

### 8.3 回调表 = 路由表

`connlist[fd]` 以 fd 为键，存储对应的回调函数指针，本质上是一张**路由表**：

```
fd=3 (listenfd)  ──► accept_cb
fd=5 (clientfd)  ──► recv_cb / send_cb
fd=6 (clientfd)  ──► recv_cb / send_cb
...
```

主循环拿到就绪 fd，查路由表，调用对应处理器，完全解耦。

### 8.4 状态机视角

每个连接本质上是一个**状态机**：

```
[等待连接] ──accept──► [等待读] ──recv──► [等待写] ──send──► [等待读]
                                                                  │
                                                  客户端断开 ──► [关闭]
```

epoll 事件（EPOLLIN/EPOLLOUT）驱动状态转换，`set_event()` 就是在切换状态监听。

### 8.5 单线程为何高效

Reactor 通常运行在单线程中，但能处理大量并发，原因是：

- 所有 I/O 操作都是**非阻塞**的（或者已知就绪才调用）
- 回调函数执行时间极短，不做耗时计算
- 内核 epoll 负责监控所有 fd，用户态只需处理就绪的

**适合**：大量短连接、I/O 密集型场景（如 HTTP 服务器、代理）
**不适合**：回调中有耗时 CPU 计算的场景（需要引入线程池，即 Multi-Reactor）

---

## 9. Reactor 的变种

### 9.1 单 Reactor 单线程（reactor.c 的实现）

```
Client ──► Reactor(单线程) ──► accept/recv/send
```

- 优点：简单，无锁
- 缺点：单核，无法利用多核 CPU；回调中的耗时操作会阻塞所有连接

### 9.2 单 Reactor 多线程

```
Client ──► Reactor(单线程) ──► 线程池处理业务
```

- Reactor负责I/O，工作线程负责业务逻辑
- 代表：早期Nginx

### 9.3 多Reactor多线程（主从Reactor）

```
Client ──► Main Reactor(Accept) ──► Sub Reactor 1 ──► 线程 1
                                ──► Sub Reactor 2 ──► 线程 2
                                ──► Sub Reactor N ──► 线程 N
```

- Main Reactor只做accept，将clientfd分发给Sub Reactor
- 每个Sub Reactor在独立线程中跑自己的事件循环
- 代表：Netty、现代Nginx

---

## 10. 常见框架中的Reactor

| 框架/库         | 语言   | Reactor 实现                      |
|--------------|------|---------------------------------|
| **libevent** | C    | 封装epoll/kqueue/select，典型Reactor |
| **libuv**    | C    | Node.js底层，事件循环即Reactor          |
| **Netty**    | Java | 主从Multi-Reactor                 |
| **Nginx**    | C    | 每个worker进程独立的Reactor            |
| **Redis**    | C    | 单线程Reactor（ae事件库）               |
| **Tokio**    | Rust | 异步运行时，基于Reactor思想               |

---

## 总结

Reactor模式的精髓可以用一句话概括：

> **用事件驱动代替轮询等待，用回调分发代替条件判断，用非阻塞I/O实现高并发。**

理解Reactor的学习路径：

1. 先理解阻塞I/O的局限性（`tcp_server.c`）
2. 理解I/O多路复用（`select_server.c` → `poll_server.c` → `epoll_server.c`）
3. 在epoll基础上引入回调机制（`reactor.c`）
4. 扩展为多线程版本（Multi-Reactor）

`reactor.c`是Reactor模式最小化的完整实现，虽然只有173行，却包含了这个模式的所有核心要素。
