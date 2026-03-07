#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/epoll.h>

#define BUFFER_LENGTH 1024

typedef int(*CALLBACK)(int fd);


struct conn_item{
    int fd;
    char rbuffer[BUFFER_LENGTH];
    int rlen;
    char wbuffer[BUFFER_LENGTH];
    int wlen;
    union {
        CALLBACK accept_callback;
        CALLBACK recv_callback;
    } recv_t;

    CALLBACK send_callback;
};

struct conn_item connlist[1024] = {0};
struct reactor
{
    int epfd;
    struct conn_item *conn_list;
};

int epfd;

int set_event(int fd, int event, int flag)
{
    struct epoll_event ev;
    ev.events = event;
    ev.data.fd = fd;
    if (flag)
    {
        // add
        epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev);
    }
    else
    {
        // mod
        epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &ev);
    }
}
int recv_cb(int fd);
int send_cb(int fd);
// listenfd触发EPOLLIN事件时执行accept_cb
int accept_cb(int fd) 
{
    struct sockaddr_in client_addr;
    socklen_t len = sizeof(client_addr);
    int clientfd = accept(fd, (struct sockaddr*)&client_addr, &len);
    if (clientfd<0)
    {
        return -1;
    }

    set_event(clientfd, EPOLLIN, 1);

    connlist[clientfd].fd = clientfd;
    memset(connlist[clientfd].rbuffer, 0, BUFFER_LENGTH);
    memset(connlist[clientfd].wbuffer, 0, BUFFER_LENGTH);
    connlist[clientfd].rlen = 0;
    connlist[clientfd].wlen = 0;
    connlist[clientfd].recv_t.recv_callback = recv_cb;
    connlist[clientfd].send_callback = send_cb;

    return clientfd;
}

// clientfd触发EPOLLIN事件时执行recv_cb
int recv_cb(int fd) 
{
    char* buffer = connlist[fd].rbuffer;
    int idx = connlist[fd].rlen;

    int count = recv(fd, buffer+idx, BUFFER_LENGTH-idx, 0);
    if (count == 0)
    {
        epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
        close(fd);
        printf("client %d disconnected.\n", fd);
        return -1;
    }
    connlist[fd].rlen += count;

    // memcpy(connlist[fd].wbuffer, connlist[fd].rbuffer, connlist[fd].rlen);
    // connlist[fd].wlen = connlist[fd].rlen;

    http_request(&connlist[fd]);
    http_response(&connlist[fd]);

    // 修改事件
    set_event(fd, EPOLLOUT, 0);

    return count;
}

// clientfd触发EPOLLOUT事件时执行send_cb
int send_cb(int fd) 
{
    char* buffer = connlist[fd].wbuffer;
    int idx = connlist[fd].wlen;
    int count = send(fd, buffer, idx, 0);

    // 修改事件
    set_event(fd, EPOLLIN, 0);

    return count;
}




int main()
{
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(struct sockaddr_in));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_port = htons(2048);

    int opt = 1;
    if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt))) {
        perror("setsockopt failed");
        close(sockfd);
        return 1;
    }

    if (-1 == bind(sockfd, (struct sockaddr*)&server_addr, sizeof(struct sockaddr)))
    {
        perror("bind");
        return -1;
    }

    listen(sockfd, 10);

    connlist[sockfd].fd = sockfd;
    connlist[sockfd].recv_t.accept_callback = accept_cb;

    epfd = epoll_create1(EPOLL_CLOEXEC);
    if (epfd == 1)
    {
        perror("epoll_create1");
        return -1;
    }
    
    set_event(sockfd, EPOLLIN, 1);

    struct epoll_event events[1024] = {0};
    while(1)
    {
        int nready = epoll_wait(epfd, events, 1024, -1);
        for (int i = 0; i < nready; i++)
        {
            int connfd = events[i].data.fd; 
            if (events[i].events & EPOLLIN) 
            {
                printf("recv <----: %s\n", connlist[connfd].rbuffer);
                int count = connlist[connfd].recv_t.recv_callback(connfd);
            } 
            else if (events[i].events & EPOLLOUT)
            {
                printf("send ---->: %s\n", connlist[connfd].wbuffer);
                int count = connlist[connfd].send_callback(connfd);
            }
        }
    }

    return 0;
}
