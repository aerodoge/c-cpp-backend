#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/poll.h>



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

    struct pollfd fds[1024] = {0};
    fds[sockfd].fd = sockfd;
    fds[sockfd].events = POLLIN;
    int maxfd = sockfd;
    while(1)
    {
        int nready = poll(fds, maxfd+1, -1);
        if (fds[sockfd].revents & POLLIN)
        {
            struct sockaddr_in client_addr;
            socklen_t len = sizeof(struct sockaddr);
            int clientfd = accept(sockfd, (struct sockaddr*)&client_addr, &len);
            printf("new connection: %d\n", clientfd);

            fds[clientfd].fd = clientfd;
            fds[clientfd].events = POLLIN;

            maxfd = clientfd;
        }
        /*
         * 监听套接字 vs 客户端套接字：
         *    监听套接字（sockfd）：只用于 accept() 接受新连接                                                                       
         *    客户端套接字（clientfd）：用于 recv()/send() 收发数据
         * */
        for (int i = sockfd + 1; i <= maxfd; i++)
        {
            if (fds[i].revents & POLLIN)
            {
                char buff[256] = {0};
                int count = recv(i, buff, 256, 0);
                if (count == 0)
                {
                    printf("client %d disconnected.\n", i);
                    fds[i].fd = -1;
                    fds[i].events = 0;
                    close(i);
                    continue;
                }
                if (-1 == send(i, buff, count, 0))
                {
                    perror("send");
                    continue;
                }
                printf("clientfd: %d, count: %d, buff: %s\n", i, count, buff);
            }
        }
    }


    getchar();

}




