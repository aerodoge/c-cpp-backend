# `sendfile`优化`http_response`方案

## 一、现状与问题

### 当前数据流

```
磁盘文件
  │
  │  read()
  ▼
用户空间 wbuffer[1024]   ← 头部也在这里，头 + 体共享 1024 字节
  │
  │  send()
  ▼
内核 Socket 发送缓冲区
  │
  ▼
网络
```

**代码路径（reactor.c:75-101）：**

```c
int http_response(connection_t* conn)
{
    int filefd = open("index.html", O_RDONLY);
    struct stat stat_buf;
    fstat(filefd, &stat_buf);

    // 写 HTTP 头到 wbuffer
    conn->wlen = sprintf(conn->wbuffer, "HTTP/1.1 200 OK\r\n...\r\n\r\n", stat_buf.st_size);

    // 紧接着头部，将文件体 read 进 wbuffer 剩余空间
    int count = read(filefd, conn->wbuffer + conn->wlen, BUFFER_LENGTH - conn->wlen);
    conn->wlen += count;
}
```

**问题：**

| 问题         | 说明                                                                 |
|------------|--------------------------------------------------------------------|
| **容量硬上限**  | `BUFFER_LENGTH = 1024`，头部占~150字节后，文件体最多~870字节，超出直接截断               |
| **两次拷贝**   | `read()`把数据从页缓存拷到用户态wbuffer，`send()`再从用户态拷回内核socket缓冲区，多一次无意义的内存拷贝 |
| **fd泄漏**   | `open()`的`filefd`从未`close()`                                       |
| **不可流式发送** | 大文件必须全部装入wbuffer才能发送，无法分块                                          |

---

## 二、`sendfile`系统调用

```c
#include <sys/sendfile.h>

ssize_t sendfile(int out_fd, int in_fd, off_t *offset, size_t count);
```

- `out_fd`：目标socket fd
- `in_fd`：源文件fd（必须是支持`mmap`的fd，即普通文件）
- `offset`：从文件哪个偏移量开始发，`NULL`表示从当前位置；调用后`*offset`被更新
- `count`：本次最多发多少字节

**数据流（零拷贝）：**

```
磁盘文件 → 页缓存
               │
               │  DMA（无 CPU 参与）
               ▼
         内核 Socket 发送缓冲区
               │
               ▼
             网络
```

用户态全程不触碰文件内容，节省一次 CPU 拷贝和一次上下文切换中的数据搬运。

---

## 三、改造方案

### 3.1 修改`conn_item`结构体

需要在连接上下文中增加"待发文件"相关字段，与wbuffer的HTTP头部分开管理。

```c
struct conn_item {
    int fd;

    char rbuffer[BUFFER_LENGTH];
    int  rlen;

    /* HTTP 响应头：仍用 wbuffer 存储 */
    char wbuffer[BUFFER_LENGTH];
    int  wlen;       // 头部总长度
    int  wsent;      // 头部已发送字节数（支持分批发送）

    char resource[BUFFER_LENGTH];

    /* 新增：sendfile 文件描述符与剩余字节 */
    int    filefd;        // 打开的文件 fd，-1 表示无文件
    off_t  file_offset;   // sendfile 当前偏移量
    size_t file_size;     // 文件总字节数（Content-Length）

    union {
        CALLBACK accept_callback;
        CALLBACK recv_callback;
    } recv_t;

    CALLBACK send_callback;
};
```

### 3.2 修改`http_response`

`http_response`只负责构造响应头、记录filefd，不再读取文件内容：

```c
int http_response(connection_t* conn)
{
    int filefd = open("index.html", O_RDONLY);
    if (filefd < 0) { /* 返回 404 */ return -1; }

    struct stat st;
    fstat(filefd, &st);

    // 只写头部到 wbuffer
    conn->wlen = snprintf(conn->wbuffer, BUFFER_LENGTH,
        "HTTP/1.1 200 OK\r\n"
        "Content-Length: %ld\r\n"
        "Content-Type: text/html\r\n"
        "\r\n",
        (long)st.st_size);

    conn->wsent      = 0;
    conn->filefd     = filefd;
    conn->file_offset = 0;
    conn->file_size  = (size_t)st.st_size;

    return 0;
}
```

### 3.3 修改`send_cb`

发送分为两阶段：

1. **阶段一：发送HTTP头部**（`wbuffer`，用普通`send()`）
2. **阶段二：发送文件体**（用`sendfile()`）

```c
int send_cb(int fd)
{
    connection_t *conn = &connlist[fd];

    /* 阶段一：头部未发完，继续 send */
    if (conn->wsent < conn->wlen) {
        int n = send(fd,
                     conn->wbuffer + conn->wsent,
                     conn->wlen - conn->wsent,
                     0);
        if (n < 0) goto cleanup;
        conn->wsent += n;

        if (conn->wsent < conn->wlen)
            return n;   // 头部还没发完，等下次 EPOLLOUT
    }

    /* 阶段二：用 sendfile 发送文件体 */
    if (conn->filefd >= 0 && conn->file_size > 0) {
        ssize_t n = sendfile(fd,
                             conn->filefd,
                             &conn->file_offset,
                             conn->file_size - (size_t)conn->file_offset);
        if (n < 0) goto cleanup;

        if ((size_t)conn->file_offset < conn->file_size)
            return n;   // 文件还没发完，等下次 EPOLLOUT
    }

cleanup:
    /* 全部发完（或出错），关闭 filefd，切回读事件 */
    if (conn->filefd >= 0) {
        close(conn->filefd);
        conn->filefd = -1;
    }
    set_event(fd, EPOLLIN, 0);
    return 0;
}
```

### 3.4 初始化时将`filefd`置为 -1

在`accept_cb`中初始化新连接时：

```c
connlist[clientfd].filefd = -1;
```

---

## 四、发送路径对比

| 维度        | 改造前                        | 改造后（sendfile）     |
|-----------|----------------------------|-------------------|
| 文件大小限制    | ~870字节（BUFFER_LENGTH - 头部） | 无限制（offset追踪进度）   |
| 拷贝次数（文件体） | 2次（页缓存→用户态→内核）             | 0次用户态拷贝           |
| 大文件支持     | 否                          | 是（分批sendfile）     |
| fd泄漏      | 有                          | 无（send_cb负责close） |
| 代码复杂度     | 低                          | 略高（两阶段状态机）        |

---

## 五、注意事项

1. **`sendfile`仅Linux支持**，macOS的`sendfile`签名不同（`off_t *len`既是输入也是输出），跨平台需要条件编译。
2. **非阻塞socket + ET模式**：`sendfile`可能返回`EAGAIN`（内核发送缓冲区满），需循环重试或在EPOLLOUT时继续调用，当前LT模式下问题不大但仍需处理返回值。
3. **头部发送也可能分批**：`send()`不保证一次发完，`wsent`字段用来跟踪已发字节数，保证正确性。
4. **Content-Type自动检测**：当前写死为`text/html`，实际场景应根据文件扩展名推断MIME类型。
5. **`snprintf`替代`sprintf`**：避免头部过长时溢出wbuffer。
