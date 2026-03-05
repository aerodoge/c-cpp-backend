#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <pthread.h>


void* client_thread(void* arg)
{
    int clientfd = *(int*)arg;
    while(1)
    {
        char buff[256] = {0};
        int count = recv(clientfd, buff, 256, 0);
        if (count == 0)
        {
            printf("client %d disconnected.\n", clientfd);
            break;
        }
        send(clientfd, buff, count, 0);
        printf("clientfd: %d, count: %d, buff: %s\n", clientfd, count, buff);
    }

    close(clientfd);
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

#if 0
    struct sockaddr_in client_addr;
    socklen_t len = sizeof(struct sockaddr);
    int clientfd = accept(sockfd, (struct sockaddr*)&client_addr, &len);
    printf("accepted\n");

#if 0
    char buff[256] = {0};
    int count = recv(clientfd, buff, 256, 0);
    send(clientfd, buff, count, 0);
    printf("socdfd: %d, clientfd: %d, count: %d, buff: %s\n", sockfd, clientfd, count, buff);
#else
    while(1)
    {
        char buff[256] = {0};
        int count = recv(clientfd, buff, 256, 0);
        if (count == 0)
        {
            break;
        }
        send(clientfd, buff, count, 0);
        printf("socdfd: %d, clientfd: %d, count: %d, buff: %s\n", sockfd, clientfd, count, buff);
    }
#endif
#else
    while(1)
    {
        struct sockaddr_in client_addr;
        socklen_t len = sizeof(struct sockaddr);
        int clientfd = accept(sockfd, (struct sockaddr*)&client_addr, &len);
        printf("accepted\n");
        pthread_t tid;
        pthread_create(&tid, NULL, client_thread, &clientfd);
    }
#endif


    getchar();
    //close(clientfd);
}




