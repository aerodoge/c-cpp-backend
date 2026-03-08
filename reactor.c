#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/sendfile.h>

#define ENABLE_HTTP_RESPONSE 1
#define BUFFER_LENGTH 1024

/*
 * 回调函数类型定义
 *
 * Reactor的核心机制：用函数指针将"事件"与"处理逻辑"解耦。
 * 主循环不需要知道"该做什么"，只负责查表调用对应的回调函数。
 *
 * 参数fd：触发事件的文件描述符
 * 返回值：操作结果（-1表示失败/连接关闭）
 */
typedef int(*CALLBACK)(int fd);


/*
 * 连接项：描述一个fd的完整上下文
 *
 * Reactor的精髓在于：每个fd不仅仅是一个整数，它携带了自己的缓冲区和回调函数，是一个完整的"状态机节点"。
 * 
 */
struct conn_item {
    int fd;                      // 文件描述符

    char rbuffer[BUFFER_LENGTH]; // 读缓冲区：存储从fd收到的数据
    int  rlen;                   // 读缓冲区当前已使用的字节数

    char wbuffer[BUFFER_LENGTH]; // 写缓冲区：存储待发送给客户端的数据
    int  wlen;                   // 写缓冲区中待发送的字节数

    char resource[BUFFER_LENGTH]; // 请求的资源路径（HTTP场景下使用）

    /*
     * 使用union节省内存：
     *
     * listenfd只需要accept_callback（接受新连接）
     * clientfd只需要recv_callback（读取数据）
     *
     * 两种fd不会同时使用同一个字段，union让它们共享同一块内存。
     * 通过绑定不同的函数指针，主循环无需区分fd的类型，统一调用recv_t.recv_callback(fd) 即可。
     * 
     */
    union {
        CALLBACK accept_callback; // listenfd使用：新连接到来时调用
        CALLBACK recv_callback;   // clientfd使用：数据可读时调用
    } recv_t;

    CALLBACK send_callback;       // clientfd使用：数据可写时调用
};

#if ENABLE_HTTP_RESPONSE
#define ROOT_DIR "/home/blockinjector/c-cpp"
typedef struct conn_item connection_t;

int http_request(connection_t* conn)
{
    // 解析HTTP请求（暂未实现，预留接口）
    return 0;
}

int http_response(connection_t* conn)
{
#if 0
    // 方式一：硬编码HTTP响应报文（调试用）
    conn->wlen = sprintf(conn->wbuffer, "HTTP/1.1 200 OK\r\nAccept-Ranges: bytes\r\nContent-Length: 77\r\nContent-Type: text/html\r\nDate: Sat 06 Aug 2026 12:16:46 GMT\r\n\r\n<html><head><title>cdd</title></head><body><h1>cdd</h1></body></html>\r\n\r\n");
    conn->wbuffer;
    return conn->wlen;
#else
    // 方式二：从文件读取HTTP响应体
    // 先用fstat获取文件大小，填入Content-Length头
    int filefd = open("index.html", O_RDONLY);
    struct stat stat_buf;
    fstat(filefd, &stat_buf);

    // 先将HTTP响应头写入 wbuffer，记录头部占用的字节数
    conn->wlen = sprintf(conn->wbuffer,
        "HTTP/1.1 200 OK\r\n"
        "Accept-Ranges: bytes\r\n"
        "Content-Length: %ld\r\n"
        "Content-Type: text/html\r\n"
        "Date: Sat 06 Aug 2026 12:16:46 GMT\r\n\r\n",
        stat_buf.st_size);

    // 紧接着头部，将文件内容读入wbuffer的剩余空间
    int count = read(filefd, conn->wbuffer + conn->wlen, BUFFER_LENGTH - conn->wlen);
    conn->wlen += count;
#endif
}

#endif


/*
 * 全局连接表：以fd值为下标的数组
 *
 * Linux内核分配fd时从小整数递增，因此可以直接用fd做数组下标，实现O(1)的连接查找，无需哈希表或链表。
 * 
 * connlist[fd]就是该fd对应的全部上下文（缓冲区 + 回调）。
 */
struct conn_item connlist[1024] = {0};

/*
 * Reactor结构体：对事件循环核心资源的封装
 *
 * epfd      ：epoll实例，内核在此维护监听集合（红黑树）和就绪队列
 * conn_list ：指向连接表的指针，与connlist全局数组配合使用
 *
 * 将epfd和conn_list 放在一起，便于将来封装成多Reactor实例
 * （参见 multi_reactor.c 的实现）。
 */
struct reactor {
    int epfd;
    struct conn_item *conn_list;
};

// 全局epfd，供回调函数直接使用（单Reactor场景下的简化做法）
int epfd;

/*
 * set_event：向epoll注册或修改fd的监听事件
 *
 * fd    ：目标文件描述符
 * event ：监听的事件类型，如EPOLLIN（可读）、EPOLLOUT（可写）
 * flag  ：1 = ADD（新增），0 = MOD（修改已存在的 fd）
 *
 * ADD和MOD的区别：
 *   EPOLL_CTL_ADD：fd首次加入epoll，内核在红黑树中插入新节点
 *   EPOLL_CTL_MOD：fd已在epoll中，只修改监听的事件类型
 *   对未注册的fd执行MOD，或对已注册的fd执行ADD，都会报错。
 *
 * 在recv_cb和send_cb中交替调用set_event(EPOLLOUT) 和set_event(EPOLLIN)，驱动"读 → 写 → 读"的状态机流转。
 * 
 */
int set_event(int fd, int event, int flag)
{
    struct epoll_event ev;
    ev.events  = event;  // 要监听的事件
    ev.data.fd = fd;     // 事件触发时，通过ev.data.fd取回是哪个fd
    if (flag)
        epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev); // 新fd，加入监听
    else
        epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &ev); // 已有fd，修改事件类型
}

// 前向声明，accept_cb内部需要引用
int recv_cb(int fd);
int send_cb(int fd);

/*
 * accept_cb：处理新连接（listenfd触发EPOLLIN时调用）
 *
 * 职责：
 *   1. 从listenfd的accept队列取出一个新连接，得到clientfd
 *   2. 将clientfd注册到epoll，监听EPOLLIN（等待客户端发数据）
 *   3. 初始化connlist[clientfd]，绑定读写回调
 *
 * 完成后，主循环就能通过connlist[clientfd]路由到正确的回调，不再需要任何if/else来区分fd类型。
 * 
 */
int accept_cb(int fd)
{
    struct sockaddr_in client_addr;
    socklen_t len = sizeof(client_addr);

    // accept从完成队列取出一个已完成三次握手的连接
    int clientfd = accept(fd, (struct sockaddr*)&client_addr, &len);
    if (clientfd < 0)
        return -1;

    // 新fd加入epoll，监听读事件，等待客户端发来请求
    set_event(clientfd, EPOLLIN, 1); // flag=1: EPOLL_CTL_ADD

    // 初始化连接项，清空缓冲区，绑定回调函数
    connlist[clientfd].fd   = clientfd;
    connlist[clientfd].rlen = 0;
    connlist[clientfd].wlen = 0;
    memset(connlist[clientfd].rbuffer, 0, BUFFER_LENGTH);
    memset(connlist[clientfd].wbuffer, 0, BUFFER_LENGTH);

    // 绑定回调：后续 clientfd的读写事件由recv_cb/send_cb处理
    connlist[clientfd].recv_t.recv_callback = recv_cb;
    connlist[clientfd].send_callback        = send_cb;

    return clientfd;
}

/*
 * recv_cb：读取客户端数据（clientfd触发EPOLLIN时调用）
 *
 * 职责：
 *   1. 从fd读数据到rbuffer
 *   2. 调用http_request/http_response构造响应，填入wbuffer
 *   3. 将监听事件从EPOLLIN切换为EPOLLOUT，触发send_cb
 *
 * 关键：recv返回0表示对端关闭了连接（发送了TCP FIN），此时必须从epoll删除fd并close，否则会持续触发EPOLLIN。
 * 
 */
int recv_cb(int fd)
{
    char *buffer = connlist[fd].rbuffer;
    int   idx    = connlist[fd].rlen;  // 从上次读到的位置继续追加

    int count = recv(fd, buffer + idx, BUFFER_LENGTH - idx, 0);
    if (count == 0) {
        // 对端关闭连接：从epoll删除，关闭fd
        epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
        close(fd);
        printf("client %d disconnected.\n", fd);
        return -1;
    }
    connlist[fd].rlen += count;

    /*
     * 注释掉的echo逻辑（直接回显）：
     *   memcpy(connlist[fd].wbuffer, connlist[fd].rbuffer, connlist[fd].rlen);
     *   connlist[fd].wlen = connlist[fd].rlen;
     * 现在改为构造HTTP响应：
     */
    http_request(&connlist[fd]);   // 解析请求（当前为空实现）
    http_response(&connlist[fd]);  // 构造HTTP响应，填入wbuffer

    /*
     * 读完后切换为监听写事件：
     * 下次epoll_wait返回EPOLLOUT时，调用send_cb将wbuffer发出去。
     * flag=0: EPOLL_CTL_MOD（clientfd已在epoll中，只改事件类型）
     */
    set_event(fd, EPOLLOUT, 0);

    return count;
}

/*
 * send_cb：发送响应数据（clientfd触发EPOLLOUT时调用）
 *
 * 职责：
 *   1. 将wbuffer中的数据发送给客户端
 *   2. 将监听事件切回EPOLLIN，等待客户端的下一条请求
 *
 * EPOLLOUT的触发时机：fd的内核发送缓冲区有空间可写。
 * 注意：不能一直监听EPOLLOUT，否则只要缓冲区不满就会持续触发，浪费CPU。
 * 正确做法是：有数据要发时才切换到EPOLLOUT，发完立即切回EPOLLIN。
 */
int send_cb(int fd)
{
    char *buffer = connlist[fd].wbuffer;
    int   idx    = connlist[fd].wlen;

    int count = send(fd, buffer, idx, 0);

    // 发送完毕，切回监听读事件，等待客户端下一条请求
    // flag=0: EPOLL_CTL_MOD
    set_event(fd, EPOLLIN, 0);

    return count;
}


int main()
{
    // ── 创建监听 socket ──────────────────────────────────────────────
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(struct sockaddr_in));
    server_addr.sin_family      = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY); // 监听所有网卡
    server_addr.sin_port        = htons(2048);

    int opt = 1;
    // SO_REUSEADDR：允许服务器重启后立即复用处于TIME_WAIT状态的端口
    if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt))) {
        perror("setsockopt failed");
        close(sockfd);
        return 1;
    }

    if (-1 == bind(sockfd, (struct sockaddr*)&server_addr, sizeof(struct sockaddr))) {
        perror("bind");
        return -1;
    }

    // backlog=10：内核accept队列最多容纳10个已完成握手的连接
    listen(sockfd, 10);

    // ── 初始化 Reactor ───────────────────────────────────────────────

    // 将listenfd纳入连接表，绑定accept_cb
    // 这样主循环就能统一处理所有fd，无需特判"if fd == listenfd"
    connlist[sockfd].fd = sockfd;
    connlist[sockfd].recv_t.accept_callback = accept_cb;

    // EPOLL_CLOEXEC：子进程exec时自动关闭epfd，防止fd泄漏
    epfd = epoll_create1(EPOLL_CLOEXEC);
    if (epfd == 1) {
        perror("epoll_create1");
        return -1;
    }

    // 将listenfd注册到epoll，监听EPOLLIN（新连接到来）
    set_event(sockfd, EPOLLIN, 1); // flag=1: EPOLL_CTL_ADD

    // ── 事件循环（Dispatcher）────────────────────────────────────────
    //
    // 主循环的唯一职责：等待事件 → 查表 → 调用回调。
    // 不包含任何业务逻辑，所有处理都委托给绑定在conn_item上的回调函数。
    struct epoll_event events[1024] = {0};
    while (1) {
        // epoll_wait 阻塞直到有fd就绪，返回就绪数量nready
        // timeout=-1 表示永久阻塞，直到有事件发生
        int nready = epoll_wait(epfd, events, 1024, -1);

        for (int i = 0; i < nready; i++) {
            int connfd = events[i].data.fd; // 取出就绪的 fd

            if (events[i].events & EPOLLIN) {
                // 可读事件：通过连接表查找回调并调用
                // listenfd  → accept_cb（取新连接）
                // clientfd  → recv_cb（读数据）
                // 主循环无需知道是哪种，统一调用即可
                printf("recv <----: %s\n", connlist[connfd].rbuffer);
                int count = connlist[connfd].recv_t.recv_callback(connfd);
            }
            else if (events[i].events & EPOLLOUT) {
                // 可写事件：内核发送缓冲区有空间，可以发数据了
                printf("send ---->: %s\n", connlist[connfd].wbuffer);
                int count = connlist[connfd].send_callback(connfd);
            }
        }
    }

    return 0;
}
