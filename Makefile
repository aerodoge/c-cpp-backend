CC=gcc


tcp-server: tcp-server.o
	$(CC) -o $@ $^

tcp-server.o: tcp-server.c
	$(CC) -c tcp-server.c











clean:
	rm -rf *.o






