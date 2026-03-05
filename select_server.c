#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/select.h>



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

    fd_set rfds, rset;
    FD_ZERO(&rfds);
    FD_SET(sockfd, &rfds);
    int maxfd = sockfd;
    printf("loop\n");
    while(1)
    {
        rset = rfds;
        int nready = select(maxfd+1, &rset, NULL, NULL, NULL);
        if (FD_ISSET(sockfd, &rset))
        {
            struct sockaddr_in client_addr;
            socklen_t len = sizeof(struct sockaddr);
            int clientfd = accept(sockfd, (struct sockaddr*)&client_addr, &len);
            printf("new connection: %d\n", clientfd);
            
            FD_SET(clientfd, &rfds);
            maxfd = clientfd;
        }

        for(int i = sockfd+1; i <= maxfd; i++)
        {
            if (FD_ISSET(i, &rset))
            {
                char buff[256] = {0};
                int count = recv(i, buff, 256, 0);
                if (count == 0)
                {
                    printf("client %d disconnected.\n", i);
                    
                    FD_CLR(i, &rfds);
                    close(i);

                    break;
                }
                send(i, buff, count, 0);
                printf("clientfd: %d, count: %d, buff: %s\n", i, count, buff);
            }
        }
    }


    getchar();

}




