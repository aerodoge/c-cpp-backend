CC=gcc

all: tcp_server select_server poll_server epoll_server epoll_et reactor multi_reactor

tcp_server: tcp_server.c
	$(CC) -o $@ $^ -lpthread

select_server: select_server.c
	$(CC) -o $@ $^

poll_server: poll_server.c
	$(CC) -o $@ $^

epoll_server: epoll_server.c
	$(CC) -o $@ $^

epoll_et: epoll_et.c
	$(CC) -o $@ $^

reactor: reactor.c
	$(CC) -o $@ $^

multi_reactor: multi_reactor.c
	$(CC) -o $@ $^ -lpthread

tcp-server: tcp-server.o
	$(CC) -o $@ $^

tcp-server.o: tcp-server.c
	$(CC) -c tcp-server.c











clean:
	rm -rf *.o






