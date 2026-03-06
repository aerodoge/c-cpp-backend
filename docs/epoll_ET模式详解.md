# epoll ET 模式详解

## 目录

1. [两种触发模式](#1-两种触发模式)
2. [LTvsET：用一个例子彻底说清楚](#2-lt-vs-et用一个例子彻底说清楚)
3. [ET模式的两个强制要求](#3-et-模式的两个强制要求)
4. [为什么ET必须非阻塞](#4-为什么-et-必须非阻塞)
5. [为什么ET必须循环读到 EAGAIN](#5-为什么-et-必须循环读到-eagain)
6. [epoll_et.c代码解析](#6-epoll_etc-代码解析)
7. [listenfd的ET陷阱](#7-listenfd-的-et-陷阱)
8. [LTvsET性能对比](#8-lt-vs-et-性能对比)
9. [ET模式的适用场景](#9-et-模式的适用场景)

---

## 1. 两种触发模式

epoll 独有两种事件通知方式：

| 模式     | 全称                    | 触发时机                           |
|--------|-----------------------|--------------------------------|
| **LT** | Level Triggered（水平触发） | **只要**缓冲区有数据，每次`epoll_wait`都通知 |
| **ET** | Edge Triggered（边缘触发）  | 缓冲区状态**发生变化**时，只通知一次           |

```c
// 开启 LT（默认，不需要额外标志）
ev.events = EPOLLIN;

// 开启 ET（需要加 EPOLLET 标志）
ev.events = EPOLLIN | EPOLLET;
```

---

## 2. LT vs ET：用一个例子彻底说清楚

假设客户端发来100字节，每次`recv`只读10字节：

### LT（水平触发，默认）

```
时间线：

客户端发来100字节，内核读缓冲区：[100字节]

第1次 epoll_wait → 触发（缓冲区有数据）
  recv(fd, buf, 10) → 读了10字节，缓冲区剩90字节

第2次 epoll_wait → 触发（缓冲区还有数据）
  recv(fd, buf, 10) → 读了10字节，缓冲区剩80字节

第3次 epoll_wait → 触发 ...

...直到缓冲区清空，才不再触发
```

**LT 的行为**：只要缓冲区非空，就持续触发，读多读少都无所谓，下次还会通知。

### ET（边缘触发）

```
时间线：

客户端发来 100 字节，内核读缓冲区：[空 → 100字节] ← 状态变化！

第1次 epoll_wait → 触发（状态从"空"变为"有数据"）
  recv(fd, buf, 10) → 读了10字节，缓冲区剩90字节

第2次 epoll_wait → 不触发（状态没有再次从"空"变为"有数据"）

第3次 epoll_wait → 不触发 ...

剩余的90字节永远没有机会被读到！
```

**ET 的行为**：只在状态变化（从无数据变为有数据）的"边缘"触发一次，之后不再重复通知。

---

## 3. ET 模式的两个强制要求

ET模式对编程有两个**强制要求**，违反任何一个都会出bug：

```
强制要求 1：fd必须设置为非阻塞（O_NONBLOCK）
强制要求 2：EPOLLIN触发后，必须循环recv直到返回EAGAIN
```

这两个要求是相互依存的，缺一不可。

---

## 4. 为什么ET必须非阻塞

假设ET模式下fd是**阻塞**的：

```
ET触发，缓冲区有10字节
→ recv(fd, buf, 1024)  // 想读1024字节
→ 读完10字节后，缓冲区空了
→ recv阻塞！等待更多数据到来
→ 事件循环卡死，其他所有连接都无法响应！
```

设置O_NONBLOCK后：

```
ET触发，缓冲区有10字节
→ recv(fd, buf, 1024)  // 想读1024字节
→ 读完10字节，缓冲区空了
→ recv立即返回-1，errno = EAGAIN  ← 不阻塞，告诉你"没数据了"
→ 事件循环继续处理其他 fd
```

**O_NONBLOCK是ET模式的"安全阀"**：确保recv/send/accept在资源不可用时立即返回，不会阻塞事件循环。

---

## 5. 为什么ET必须循环读到EAGAIN

假设ET模式下只读一次：

```
客户端发来100字节，ET触发
→ recv一次，读了10字节，返回10
→ 不继续读了，等下次epoll_wait

下次epoll_wait：缓冲区还有90字节，但状态没有"从空变非空"
→ ET不触发！
→ 剩余90字节永远等不到通知
```

正确做法：

```c
// ET模式的标准读循环
while (1) 
{
    int count = recv(fd, buffer, size, 0);
    if (count < 0) 
    {
        if (errno == EAGAIN || errno == EWOULDBLOCK) 
        {
            break;  // 缓冲区真正空了，本次处理完毕
        }
        // 其他错误
        break;
    }
    if (count == 0) 
    {
        // 对端关闭
        break;
    }
    // 处理读到的数据...
}
```

**循环读到EAGAIN** 确保本次ET触发期间，把缓冲区里所有数据都取走，不留"尾巴"。

---

## 6. epoll_et.c 代码解析

### 6.1 设置非阻塞

```c
static int set_nonblock(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);      // 获取当前 flags
    fcntl(fd, F_SETFL, flags | O_NONBLOCK); // 追加 O_NONBLOCK
    return 0;
}
```

`fcntl`用"追加"而非"覆盖"，保留fd已有的其他标志位。

### 6.2 注册 ET 事件

```c
// listenfd：非阻塞 + ET
set_nonblock(sockfd);
ev.events = EPOLLIN | EPOLLET;
epoll_ctl(epfd, EPOLL_CTL_ADD, sockfd, &ev);

// clientfd：accept 后立即设置非阻塞 + ET
set_nonblock(clientfd);
ev.events = EPOLLIN | EPOLLET;
epoll_ctl(epfd, EPOLL_CTL_ADD, clientfd, &ev);
```

### 6.3 核心：循环读直到 EAGAIN

```c
} 
else if (events[i].events & EPOLLIN) 
{

    char buffer[BUFFER_LENGTH];
    int  total = 0;

    while (1) 
    {                                    // ← 循环读
        int count = recv(fd, buffer + total, BUFFER_LENGTH - total, 0);
        if (count < 0) 
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK) 
            {
                break;   // ← 读完了，退出循环
            }
            if (errno == EINTR) continue;  // 被信号打断，重试
            // 真正的错误
            close_and_del(fd);
            break;
        }
        if (count == 0) 
        {
            close_and_del(fd);  // 对端关闭
            break;
        }
        total += count;
        if (total >= BUFFER_LENGTH) break;  // 缓冲区满，先处理
    }

    if (total > 0) 
    {
        send(fd, buffer, total, 0);  // echo
    }
}
```

循环中的三种退出条件：

| 退出条件                  | 含义              |
|-----------------------|-----------------|
| `errno == EAGAIN`     | 缓冲区读空，正常结束      |
| `count == 0`          | 对端关闭连接（TCP FIN） |
| `count < 0` 且非 EAGAIN | 真正的读错误          |

### 6.4 EINTR 的处理

```c
if (errno == EINTR) continue;
```

`recv`返回-1且 `errno == EINTR`表示被信号中断（不是错误），应该重试。这在生产代码中是必须处理的细节。

---

## 7. listenfd的ET陷阱

ET对listenfd同样有影响，这是容易被遗漏的坑：

**场景**：多个客户端同时发起连接，在epoll_wait返回之前，listenfd的accept队列里排了3个连接。

### LT的行为（安全）

```
第1次 epoll_wait：触发，accept一次，取走连接A，队列剩B、C
第2次 epoll_wait：触发（队列非空），accept一次，取走连接B
第3次 epoll_wait：触发（队列非空），accept一次，取走连接C
→ 所有连接都被处理
```

### ET的行为（危险）

```
第1次 epoll_wait：触发，accept一次，取走连接A，队列剩B、C
第2次 epoll_wait：不触发（listenfd 状态没有再次从空变非空）
→ 连接B和C被永久搁置！
```

### 正确做法：listenfd也要循环accept

```c
if (fd == sockfd) 
{
    while (1) 
    {                         // ← 循环 accept
        int clientfd = accept(sockfd, ...);
        if (clientfd < 0) 
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK) 
            {
                break;   // 没有更多等待的连接了
            }
            break;
        }
        set_nonblock(clientfd);
        ev.events = EPOLLIN | EPOLLET;
        epoll_ctl(epfd, EPOLL_CTL_ADD, clientfd, &ev);
    }
}
```

**规律**：ET模式下，任何fd的任何事件，都必须在触发时把该事件对应的操作做到"不能再做"（EAGAIN）为止。

---

## 8. LT vs ET 性能对比

ET 的性能优势体现在减少 `epoll_wait` 的系统调用次数：

```
场景：客户端发来 10 次小消息，每次 100 字节

LT 模式：
  每次消息到来 → epoll_wait 返回 → recv → 处理 → 等下次
  10 次消息 = 10 次 epoll_wait 返回
  每次 epoll_wait 都是一次系统调用（用户态↔内核态切换）

ET 模式：
  多条消息可能在一次 epoll_wait 返回后全部读完（循环 recv 到 EAGAIN）
  理想情况：10 次消息 = 1 次 epoll_wait 返回
```

但ET的收益并非总是显著：

- 消息到达间隔很长时（一问一答），LT和ET触发次数相同
- 大量数据密集到达时，ET能显著减少系统调用次数
- ET模式代码更复杂，出错概率更高

**ET是以编程复杂度换取极端情况下的性能收益。**

---

## 9. ET 模式的适用场景

| 场景                  | 推荐模式 | 原因                 |
|---------------------|------|--------------------|
| 数据量小，一收一发           | LT   | 两者性能相当，LT更简单       |
| 大量数据密集传输（文件、视频流）    | ET   | 减少系统调用，提升吞吐        |
| 高并发短连接（HTTP）        | ET   | 每个连接生命周期短，ET减少无效唤醒 |
| 逻辑复杂，开发优先           | LT   | 出错风险低，调试方便         |
| 追求极致性能（Nginx/Redis） | ET   | 业界标配               |

### 实际项目中的选择

- **Redis**：LT模式。Redis是单线程，数据量通常较小，LT足够，追求代码简洁
- **Nginx**：ET模式。高并发HTTP，需要最大化吞吐量
- **libevent**：默认LT，可选ET

---

## 总结对比

```
                LT（水平触发）                  ET（边缘触发）
                ──────────────              ──────────────
触发时机         缓冲区非空就触发              缓冲区状态变化时触发一次
fd是否需要非阻塞  不强制（阻塞也能工作）         必须非阻塞
是否需要循环读    不需要（读多少都行）           必须循环读到 EAGAIN
漏数据风险       无                          有（未循环读时）
编程复杂度       低                          高
系统调用次数     相对多                       相对少
兼容性          select/poll 行为一致         epoll 独有
```

**一句话**：
> LT 是"有就通知"，ET 是"变了才通知"。ET 性能更好但门槛更高，必须配合非阻塞 + 循环读，任何一个缺失都会导致数据丢失或程序卡死。
