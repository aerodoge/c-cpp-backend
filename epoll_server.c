#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/epoll.h>

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


    int epfd = epoll_create1(EPOLL_CLOEXEC);
    if (epfd == 1)
    {
        perror("epoll_create1");
        return -1;
    }
    
    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.fd = sockfd;
    
    if (-1 == epoll_ctl(epfd, EPOLL_CTL_ADD, sockfd, &ev))
    {
        perror("epoll_ctl");
        return -1;
    }

    struct epoll_event events[1024] = {0};
    while(1)
    {
        int nready = epoll_wait(epfd, events, 1024, -1);
        for (int i = 0; i < nready; i++)
        {
            int connfd = events[i].data.fd; 
            if (sockfd == connfd)
            {
                struct sockaddr_in client_addr;
                socklen_t len = sizeof(client_addr);
                int clientfd = accept(sockfd, (struct sockaddr*)&client_addr, &len);

                ev.events = EPOLLIN | EPOLLET;
                ev.data.fd = clientfd;
                epoll_ctl(epfd, EPOLL_CTL_ADD, clientfd, &ev);
                printf("client fd:%d\n", clientfd);
            }
            else if (events[i].events & EPOLLIN) 
            {
                char buff[4] = {0};
                int count = recv(connfd, buff, sizeof(buff), 0);
                if (count == 0)
                {
                    epoll_ctl(epfd, EPOLL_CTL_DEL, connfd, NULL);
                    close(i);
                    printf("client %d disconnected.\n", connfd);
                    continue;
                }
                if (-1 == send(connfd, buff, count, 0))
                {
                    perror("send");
                    continue;
                }
                printf("clientfd: %d, count: %d, buff: %s\n", connfd, count, buff);
            }
        }
    }

    return 0;
}
