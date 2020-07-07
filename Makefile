CC = gcc
CFLAGS = -Wall -Wextra -Wpedantic -Wshadow -pthread -O2

all: httpserver

httpserver: server.o job_queue.o log_queue.o file_list.o
	$(CC) $(CFLAGS) -o httpserver server.o job_queue.o log_queue.o file_list.o

server.o: server.c
	$(CC) $(CFLAGS) -c server.c

job_queue.o: queue.h job_queue.c 
	$(CC) $(CFLAGS) -c job_queue.c

log_queue.o: queue.h log_queue.c 
	$(CC) $(CFLAGS) -c log_queue.c

file_list.o: file_list.h file_list.c
	$(CC) $(CFLAGS) -c file_list.c

clean:
	$(RM) *.o *~

spotless:
	$(RM) httpserver *.o *~
