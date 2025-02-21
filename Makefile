CC=gcc
CFLAGS=-Wall -Wextra -g
LDFLAGS=-lpthread

all: proxy

proxy: main.o proxy.o
	$(CC) $(CFLAGS) -o proxy main.o proxy.o $(LDFLAGS)

main.o: main.c proxy.h
	$(CC) $(CFLAGS) -c main.c

proxy.o: proxy.c proxy.h
	$(CC) $(CFLAGS) -c proxy.c

clean:
	rm -f proxy *.o

