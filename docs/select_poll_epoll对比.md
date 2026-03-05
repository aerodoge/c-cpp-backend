# select / poll / epoll 深度对比

## 目录

1. [背景：为什么需要 I/O 多路复用](#1-背景为什么需要-io-多路复用)
2. [select](#2-select)
3. [poll](#3-poll)
4. [epoll](#4-epoll)
5. [内核实现原理对比](#5-内核实现原理对比)
6. [性能对比：用数字说话](#6-性能对比用数字说话)
7. [代码层面对比](#7-代码层面对比)
8. [横向对比总表](#8-横向对比总表)
9. [应用场景选择](#9-应用场景选择)

---

## 1. 背景：为什么需要 I/O 多路复用

服务器要同时监听多个 fd（文件描述符）是否就绪。朴素做法：

```c
// 方案一：逐个阻塞读（无法并发）
read(fd1, ...);  // 阻塞，fd2 完全被忽略
read(fd2, ...);

// 方案二：非阻塞轮询（CPU 空转）
while(1) {
    read(fd1, ...);  // 没数据立即返回 EAGAIN
    read(fd2, ...);  // 没数据立即返回 EAGAIN
    // CPU 100% 但大多数时候啥也没做
}
```

I/O 多路复用的目标：**把"等待哪个 fd 就绪"这件事交给内核，内核通知用户态，用户态只处理就绪的 fd**。

---

## 2. select

### 2.1 接口

```c
#include <sys/select.h>

int select(int nfds, fd_set *readfds, fd_set *writefds,
           fd_set *exceptfds, struct timeval *timeout);

// fd_set 操作宏
FD_ZERO(&set);       // 清空集合
FD_SET(fd, &set);    // 将 fd 加入集合
FD_CLR(fd, &set);    // 将 fd 从集合移除
FD_ISSET(fd, &set);  // 判断 fd 是否在集合中（是否就绪）
```

### 2.2 fd_set 的本质：位图

`fd_set` 是一个固定大小的位图（bitmap）：

```
fd_set（通常 128 字节 = 1024 位）

bit 0  bit 1  bit 2  ...  bit 1023
  0      0      1    ...    0
              ↑
           fd=2 被监听
```

每个 bit 代表一个 fd，1 表示监听/就绪，0 表示不监听/未就绪。

**这决定了 select 的 fd 上限：`FD_SETSIZE = 1024`**，这是编译期硬限制。

### 2.3 select_server.c 的关键代码解析

```c
fd_set rfds, rset;      // rfds: 主集合（持久保存监听的fd）
                         // rset: 每次调用前的副本（因为select会修改入参）
FD_ZERO(&rfds);
FD_SET(sockfd, &rfds);
int maxfd = sockfd;

while(1) {
    rset = rfds;         // ★ 关键：每次调用前必须复制！
                         //   因为 select 返回后会把未就绪的 bit 清零
                         //   如果不复制，下次调用就丢失了监听信息

    int nready = select(maxfd+1, &rset, NULL, NULL, NULL);
    //                  ↑
    //   必须传 maxfd+1，内核从 0 扫描到 maxfd

    if (FD_ISSET(sockfd, &rset)) {    // 新连接
        int clientfd = accept(...);
        FD_SET(clientfd, &rfds);       // 加入主集合
        maxfd = clientfd;              // 更新最大fd（注意：只增不减，有缺陷）
    }

    // ★ O(n) 遍历：必须从 0 扫描到 maxfd，逐个检查
    for(int i = sockfd+1; i <= maxfd; i++) {
        if (FD_ISSET(i, &rset)) {
            // 处理 i
        }
    }
}
```

### 2.4 select 的工作流程

```
用户态                              内核态
  │                                    │
  ├── 构造 fd_set 位图                  │
  │                                    │
  ├── select() ──── 系统调用 ──────────►│
  │                                    ├── 将 fd_set 从用户态复制到内核态
  │                                    ├── 遍历 0~maxfd 每个 fd
  │                 [阻塞等待]          ├── 为每个 fd 注册等待队列回调
  │                                    ├── 进程睡眠
  │                                    │
  │                                    ├── (某个 fd 就绪，唤醒进程)
  │                                    ├── 再次遍历 0~maxfd
  │                                    ├── 将未就绪的 bit 清零
  │                                    ├── 将修改后的 fd_set 复制回用户态
  │◄─── 返回就绪数量 ──────────────────│
  │                                    │
  ├── 用户态遍历 0~maxfd 检查 FD_ISSET  │
  │   找出哪些就绪
```

**两次 O(n) 遍历**：内核遍历一次 + 用户态遍历一次，且每次系统调用都要在用户态/内核态之间复制 fd_set。

### 2.5 优缺点

**优点：**
- POSIX 标准，跨平台（Linux/macOS/Windows/BSD）
- 实现简单，历史悠久，兼容性最好

**缺点：**
- **fd 上限 1024**，无法处理更多并发连接（可重编译改变，但不优雅）
- **每次调用需复制 fd_set**（用户态→内核态，内核态→用户态），O(n) 内存拷贝
- **内核遍历 0~maxfd 所有 fd**，O(n) 扫描，大量无效检查
- **用户态再遍历一次**，才能知道哪个 fd 就绪
- **fd_set 被修改**，每次调用前必须重新赋值（`rset = rfds`）

---

## 3. poll

### 3.1 接口

```c
#include <sys/poll.h>

int poll(struct pollfd *fds, nfds_t nfds, int timeout);

struct pollfd {
    int   fd;       // 监听的 fd
    short events;   // 关心的事件（输入参数）
    short revents;  // 实际发生的事件（输出参数，内核填写）
};
```

### 3.2 pollfd 数组：比位图更灵活

poll 用 `struct pollfd` 数组替换 `fd_set` 位图：

```
fds[0]: {fd=3, events=POLLIN,  revents=0}
fds[1]: {fd=5, events=POLLIN,  revents=POLLIN}  ← 就绪
fds[2]: {fd=7, events=POLLOUT, revents=0}
```

**events（输入）和 revents（输出）分离**，解决了 select 每次都要重新赋值的问题。

### 3.3 poll_server.c 的关键代码解析

```c
struct pollfd fds[1024] = {0};
fds[sockfd].fd = sockfd;
fds[sockfd].events = POLLIN;   // events 只设置一次，不用每次重置
int maxfd = sockfd;

while(1) {
    // ★ 注意：poll_server.c 用 fd 值作为 fds 数组下标
    //   fds[3] 存 fd=3 的信息，fds[5] 存 fd=5 的信息
    //   这样 O(1) 定位，但浪费了中间的空间（fds[4] 空着）
    int nready = poll(fds, maxfd+1, -1);
    //                      ↑
    //   传入监听数量，内核只检查 fds[0]~fds[maxfd]

    if (fds[sockfd].revents & POLLIN) {   // 新连接
        int clientfd = accept(...);
        fds[clientfd].fd = clientfd;
        fds[clientfd].events = POLLIN;    // 设置一次即可，不像 select 每次复制
        maxfd = clientfd;
    }

    // 仍然是 O(n) 遍历
    for (int i = sockfd+1; i <= maxfd; i++) {
        if (fds[i].revents & POLLIN) {
            int count = recv(i, buff, ...);
            if (count == 0) {
                fds[i].fd = -1;       // ★ fd=-1 表示该槽位失效
                fds[i].events = 0;    //   内核会跳过 fd<0 的项
                close(i);
            }
            send(i, buff, count, 0);
        }
    }
}
```

### 3.4 poll 的工作流程

```
用户态                              内核态
  │                                    │
  ├── 构造 pollfd 数组                  │
  │                                    │
  ├── poll() ────── 系统调用 ──────────►│
  │                                    ├── 将 pollfd 数组从用户态复制到内核态
  │                                    ├── 遍历数组每个 pollfd
  │                 [阻塞等待]          ├── 注册等待队列，进程睡眠
  │                                    │
  │                                    ├── (某个 fd 就绪，唤醒进程)
  │                                    ├── 再次遍历数组，填写 revents
  │                                    ├── 将数组复制回用户态
  │◄─── 返回就绪数量 ──────────────────│
  │                                    │
  ├── 用户态遍历数组检查 revents         │
```

流程与 select 几乎相同，核心问题没有解决：仍然是 O(n) 遍历 + 每次系统调用拷贝数组。

### 3.5 优缺点

**优点（相比 select）：**
- **无 fd 数量上限**（理论上受系统 open files limit 限制）
- **events/revents 分离**，不需要每次重置监听集合
- 支持更多事件类型（`POLLHUP`、`POLLERR` 等）

**缺点（与 select 相同的根本问题）：**
- **每次调用仍需复制 pollfd 数组**（用户态↔内核态）
- **内核仍需 O(n) 遍历**所有 pollfd
- **用户态仍需 O(n) 遍历**检查 revents
- **不跨平台**（Windows 不支持）

**poll 本质是 select 的改良版**，解决了 fd 上限和集合重置的问题，但没有解决 O(n) 性能问题。

---

## 4. epoll

### 4.1 接口

```c
#include <sys/epoll.h>

// 创建 epoll 实例，返回 epfd
int epoll_create1(int flags);

// 注册/修改/删除 fd
int epoll_ctl(int epfd, int op, int fd, struct epoll_event *event);
// op: EPOLL_CTL_ADD / EPOLL_CTL_MOD / EPOLL_CTL_DEL

// 等待事件，events 数组只存放就绪的 fd
int epoll_wait(int epfd, struct epoll_event *events, int maxevents, int timeout);

struct epoll_event {
    uint32_t events;   // 事件类型
    epoll_data_t data; // 用户数据（可存 fd、指针等）
};
```

### 4.2 epoll 的革命性设计

epoll 将"注册"和"等待"彻底分离：

| 操作 | select/poll | epoll |
|------|------------|-------|
| 注册监听 fd | 每次调用前重新构造集合 | `epoll_ctl()` 一次性注册，内核持久保存 |
| 等待就绪 | 传入所有 fd，内核全量扫描 | `epoll_wait()` 只返回就绪的 fd |
| 获取就绪列表 | 用户态遍历全部 fd | 直接从返回的数组读取 |

### 4.3 epoll 内核数据结构

```
epoll 实例（epfd）
├── 红黑树（监听集合）
│   ├── fd=3 (listenfd)  events=EPOLLIN
│   ├── fd=5 (clientfd)  events=EPOLLIN
│   ├── fd=6 (clientfd)  events=EPOLLOUT
│   └── ...               O(log n) 增删查
│
└── 就绪链表（ready list）
    ├── fd=5  events=EPOLLIN   ← 有数据可读
    └── fd=6  events=EPOLLOUT  ← 可写
```

- **红黑树**：存储所有被监听的 fd，O(log n) 增删改
- **就绪链表**：存储已就绪的 fd，内核在 fd 就绪时直接插入

### 4.4 epoll_server.c 关键代码解析

```c
int epfd = epoll_create1(EPOLL_CLOEXEC);  // 创建 epoll 实例

// 注册 listenfd，只需注册一次
struct epoll_event ev;
ev.events = EPOLLIN;
ev.data.fd = sockfd;
epoll_ctl(epfd, EPOLL_CTL_ADD, sockfd, &ev);

struct epoll_event events[1024] = {0};
while(1) {
    // ★ 核心：只返回就绪的事件，nready 就是真正有事情做的数量
    int nready = epoll_wait(epfd, events, 1024, -1);

    // ★ 只遍历就绪的 nready 个，不是所有 fd
    for (int i = 0; i < nready; i++) {
        int connfd = events[i].data.fd;
        if (sockfd == connfd) {            // 新连接
            int clientfd = accept(...);
            ev.events = EPOLLIN | EPOLLET; // 边缘触发
            ev.data.fd = clientfd;
            epoll_ctl(epfd, EPOLL_CTL_ADD, clientfd, &ev);
        } else if (events[i].events & EPOLLIN) {
            char buff[4] = {0};
            int count = recv(connfd, buff, sizeof(buff), 0);
            if (count == 0) {
                epoll_ctl(epfd, EPOLL_CTL_DEL, connfd, NULL); // 删除
                close(connfd);
            }
            send(connfd, buff, count, 0);
        }
    }
}
```

### 4.5 水平触发（LT）vs 边缘触发（ET）

epoll 独有的两种触发模式：

```
假设 fd 的读缓冲区有 100 字节数据，用户每次只读 10 字节：

水平触发（LT，默认）：
  第1次 epoll_wait → 返回该 fd（缓冲区有数据就触发）
  用户读了 10 字节，还剩 90 字节
  第2次 epoll_wait → 仍然返回该 fd（缓冲区还有数据）
  直到缓冲区清空才停止触发
  → 类似 select/poll 的行为，编程简单，不会漏数据

边缘触发（ET，需加 EPOLLET）：
  第1次 epoll_wait → 返回该 fd（状态从"无数据"变为"有数据"触发）
  用户读了 10 字节，还剩 90 字节
  第2次 epoll_wait → 不返回该 fd（状态没有再次变化）
  → 只在状态"边缘"（变化时）触发一次
  → 必须一次性读完所有数据（循环 read 直到 EAGAIN）
  → 必须配合非阻塞 fd 使用
  → 减少 epoll_wait 调用次数，效率更高
```

```
LT（水平触发）：缓冲区有数据 → 持续触发      [状态]
ET（边缘触发）：缓冲区数据量变化 → 触发一次   [变化]
```

### 4.6 reactor.c 中的 set_event 体现了 epoll 的精髓

```c
// recv_cb: 读完后切换为监听写事件
set_event(fd, EPOLLOUT, 0);  // MOD，不是 ADD

// send_cb: 写完后切换为监听读事件
set_event(fd, EPOLLIN, 0);   // MOD
```

`epoll_ctl(MOD)` 的成本是 O(log n)（红黑树查找），远比 select/poll 每次系统调用拷贝整个集合高效。

### 4.7 优缺点

**优点：**
- **O(1) 获取就绪 fd**：`epoll_wait` 直接返回就绪列表，不需要遍历全部
- **无需每次拷贝**：fd 注册一次，内核持久维护（红黑树）
- **无 fd 数量上限**：受系统文件描述符上限控制（可配置到百万级）
- **支持边缘触发（ET）**：更高效，减少系统调用次数
- **适合大规模并发**：10000+ 连接时性能优势极其显著

**缺点：**
- **仅 Linux 支持**（macOS 有 kqueue，Windows 有 IOCP，接口不同）
- **接口相对复杂**（3个函数：create/ctl/wait）
- **ET 模式编程复杂**，容易漏读数据

---

## 5. 内核实现原理对比

### 5.1 select/poll 的内核实现

每次调用 `select`/`poll` 时，内核执行：

```
1. 将 fd_set / pollfd 数组从用户态拷贝到内核态         ← 每次都拷贝
2. 遍历所有 fd（0~maxfd）                              ← O(n)
3. 对每个 fd 调用 poll() 方法（文件操作接口）
4. 如果该 fd 就绪，记录下来；否则在等待队列注册回调
5. 如果没有就绪 fd，进程睡眠
6. 某个 fd 就绪时，内核唤醒进程
7. 内核再次遍历所有 fd，找出就绪的（标记 bit / 填 revents）← O(n)
8. 将结果拷贝回用户态                                  ← 每次都拷贝
9. 用户态遍历结果找出就绪的 fd                         ← O(n)
```

**每次系统调用：3次 O(n) + 2次内存拷贝**

### 5.2 epoll 的内核实现

**注册阶段（epoll_ctl ADD）：**
```
1. 在红黑树中插入该 fd 的节点                          ← O(log n)，只做一次
2. 在该 fd 的等待队列注册回调（ep_poll_callback）       ← 只注册一次
```

**等待阶段（epoll_wait）：**
```
1. 检查就绪链表是否非空
2. 如果非空，直接将就绪链表中的事件复制到用户态 events 数组
3. 如果为空，进程睡眠
4. 某个 fd 就绪时，ep_poll_callback 将其加入就绪链表，唤醒进程
5. 将就绪链表中的事件拷贝到用户态
```

**每次系统调用：O(1) 获取结果，只拷贝就绪的事件（不是全部）**

### 5.3 "事件通知"机制的本质

```
select/poll：拉模型（Pull）
  → 每次都主动询问所有 fd："你好了吗？你好了吗？"
  → 内核被动响应，逐个检查

epoll：推模型（Push）
  → fd 就绪时，内核主动把它放入就绪链表
  → 进程只需从就绪链表取结果
  → 内核主动推送，用户被动接收
```

---

## 6. 性能对比：用数字说话

设并发连接数为 n，活跃连接（有数据的）为 k（通常 k << n）：

| 操作 | select | poll | epoll |
|------|--------|------|-------|
| 注册 fd | O(1) per fd | O(1) per fd | O(log n) per fd |
| 每次调用拷贝 | O(n)，拷贝整个 fd_set | O(n)，拷贝整个 pollfd 数组 | O(k)，只拷贝就绪事件 |
| 内核扫描 | O(n) | O(n) | O(1)，查就绪链表 |
| 用户态遍历 | O(n) | O(n) | O(k) |
| **总体复杂度** | **O(n)** | **O(n)** | **O(k)** |

当 n=10000，k=10（高并发但低活跃）时：
- select/poll：每次循环处理 10000 次
- epoll：每次循环处理 10 次

**差距 1000 倍。**

---

## 7. 代码层面对比

### 7.1 监听集合的管理

```c
// select：位图，需要备份
fd_set rfds, rset;
FD_ZERO(&rfds);
FD_SET(sockfd, &rfds);
// 每次调用前必须重置
rset = rfds;
select(maxfd+1, &rset, NULL, NULL, NULL);

// poll：数组，events/revents 分离，不需重置
struct pollfd fds[1024];
fds[sockfd].events = POLLIN;
poll(fds, maxfd+1, -1);

// epoll：持久注册，无需每次传入
epoll_create1(EPOLL_CLOEXEC);
epoll_ctl(epfd, EPOLL_CTL_ADD, sockfd, &ev);  // 只注册一次
epoll_wait(epfd, events, 1024, -1);            // 不需要传入监听集合
```

### 7.2 新连接处理

```c
// select
int clientfd = accept(sockfd, ...);
FD_SET(clientfd, &rfds);       // 加入位图
maxfd = clientfd;              // 更新最大值

// poll
int clientfd = accept(sockfd, ...);
fds[clientfd].fd = clientfd;   // 用 fd 值作下标
fds[clientfd].events = POLLIN;
maxfd = clientfd;

// epoll
int clientfd = accept(sockfd, ...);
ev.data.fd = clientfd;
epoll_ctl(epfd, EPOLL_CTL_ADD, clientfd, &ev);  // 注册到内核
// 不需要 maxfd！
```

### 7.3 断开连接处理

```c
// select：清除位图中对应 bit
FD_CLR(fd, &rfds);
close(fd);

// poll：将 fd 置为 -1（内核跳过 fd<0 的项）
fds[i].fd = -1;
fds[i].events = 0;
close(fd);

// epoll：从红黑树中删除
epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
close(fd);
```

### 7.4 遍历就绪事件

```c
// select：O(n) 遍历所有可能的 fd
for (int i = 0; i <= maxfd; i++) {
    if (FD_ISSET(i, &rset)) { ... }
}

// poll：O(n) 遍历所有 pollfd
for (int i = 0; i <= maxfd; i++) {
    if (fds[i].revents & POLLIN) { ... }
}

// epoll：O(k) 只遍历就绪的
for (int i = 0; i < nready; i++) {   // nready 是实际就绪数量
    int fd = events[i].data.fd;       // 直接取 fd，不需要扫描
    if (events[i].events & EPOLLIN) { ... }
}
```

---

## 8. 横向对比总表

| 特性 | select | poll | epoll |
|------|--------|------|-------|
| **诞生时间** | 1983（BSD） | 1986（SVR3） | 2002（Linux 2.5.44） |
| **标准** | POSIX | POSIX | Linux 专有 |
| **跨平台** | 最好（Win/Mac/Linux/BSD） | 较好（无 Windows） | 仅 Linux |
| **fd 上限** | 1024（FD_SETSIZE） | 无限制 | 无限制（受系统限制） |
| **数据结构** | 位图（fd_set） | 数组（pollfd[]） | 红黑树 + 就绪链表 |
| **每次调用拷贝** | O(n)，拷贝整个位图 | O(n)，拷贝整个数组 | O(k)，只拷贝就绪事件 |
| **内核扫描** | O(n)，全量遍历 | O(n)，全量遍历 | O(1)，查就绪链表 |
| **用户态遍历** | O(n) | O(n) | O(k) |
| **监听集合重置** | 每次调用前重置（rset=rfds） | 不需要（events不变） | 不需要（持久注册） |
| **触发模式** | 水平触发 | 水平触发 | 水平触发 + 边缘触发 |
| **内存** | 固定 128 字节 | 随 fd 数量线性增长 | 随 fd 数量线性增长（红黑树节点） |
| **适合并发量** | < 1000 | < 几千 | 10万+ |

---

## 9. 应用场景选择

### 选择 select 的场景

- 需要**跨平台**（Windows + Linux + macOS）
- 并发连接数极少（< 100），fd 值不超过 1024
- 对可移植性要求高于性能
- 嵌入式系统，内核版本较旧

代表：一些跨平台的基础网络库的兼容层

### 选择 poll 的场景

- 需要**跨 Linux/macOS/BSD**（不需要 Windows）
- 并发连接数在几百到几千，对性能没有极端要求
- fd 值可能超过 1024（select 无法处理）
- 逻辑简单，不想引入 epoll 的复杂性

代表：一些中小型服务的简单实现

### 选择 epoll 的场景

- **Linux 服务器，高并发**（万级以上连接）
- 连接数多但活跃连接少（典型的长连接场景：IM、推送）
- 需要边缘触发优化性能
- 构建高性能网络框架

代表：**Nginx、Redis、Node.js（libuv）、libevent、所有高性能 Linux 服务器**

### 决策流程图

```
需要跨平台（含 Windows）？
    │
   是 ──► select（或 libevent 抽象层）
    │
   否
    │
并发连接数 < 1000 且 fd < 1024？
    │
   是 ──► select（简单够用）
    │
   否
    │
Linux 专用？
    │
   是 ──► epoll（首选）
    │
   否（macOS/BSD）──► kqueue（epoll 的 BSD 等价物）
```

### 一句话总结

> **select** 解决了"如何同时监听多个 fd"的问题，代价是 O(n) 且有数量限制；
> **poll** 解决了"fd 数量上限"的问题，但 O(n) 没有改变；
> **epoll** 解决了"O(n) 扫描"的问题，用内核数据结构换来 O(1) 的就绪通知，是现代高性能服务器的基石。

三者是**递进式的演进**关系，理解了 select 的局限，就能理解为什么需要 poll；理解了 poll 的局限，就能理解为什么需要 epoll。
