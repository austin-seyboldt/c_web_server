#include <sys/socket.h>
#include <sys/stat.h>
#include <stdio.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <err.h>
#include <errno.h>
#include <sys/types.h>				// fstat()
#include <sys/time.h>
#include <pthread.h>
#include "httpRequest.h"	// Defines httpRequest struct and FILENAME_SIZE and REQUEST_BUFFER_SIZE constants
#include "queue.h"			// Defines LogQueue, JobQueue
#include "thread_args.h"	// Defines structs for passing data to threads
#include "file_list.h"		// Defines FileList linked list


// Used to print to program flow to console
#define DEBUG
#ifdef DEBUG
#  define D(x) x
#else 
#  define D(x) 
#endif


#define RESPONSE_BUFFER_SIZE 256
#define FILE_BUFFER_SIZE 8192
#define SMALL_BUFFER_SIZE 64
#define PORT_MIN 1024
#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1
#define BACK_LOG 20				// Max backlog size for a socket
#define METHOD_LENGTH 4			// Maximum length of a method
#define MAX_RETRIES 5			// maximum amount of times a failed interruptible function should be retried
#define HTTP_VERSION "HTTP/1.1"
#define RECV_TIMEOUT_SECONDS 20
#define DEFAULT_THREAD_COUNT 4
#define LOG_FILE_BUFFER_SIZE 8192
#define SMALL_FILE_BUFFER_SIZE 2048
#define BYTES_PER_LOG_LINE 20
#define LOG_FILENAME_SIZE 255
#define FILENAME_BUFFER_SIZE 128

struct args 
{
	uint16_t port;
	char log_filename[LOG_FILENAME_SIZE + 1];
	size_t num_threads;
};





int server_init(struct sockaddr_in* server_addr, uint16_t port);
uint16_t get_port(char* port);
bool parse_request(int client_sock, httpRequest* request);
bool get(httpRequest* request, JobThreadArgs* args);
bool head(httpRequest* request, JobThreadArgs* args);
bool put(httpRequest* request, JobThreadArgs* args);
bool send_response_header(int client_sock, httpRequest* request);
void process_request(httpRequest* request, JobThreadArgs* thread_args);
int head_helper(httpRequest* request, JobThreadArgs* args);
bool upload_file(int client_sock, int filefd, httpRequest* request);
bool get_request(int client_sock, httpRequest* request);
bool transfer_data(int out_fd, int in_fd, size_t file_size, int temp_log_file);


void* handle_connection(void* thread_args);
void* handle_log_requests(void* log_args);
void cleanup(httpRequest* request, JobThreadArgs* args);
void write_to_log(httpRequest* request, LogThreadArgs* args);
off_t write_log_header(httpRequest* request, LogThreadArgs* args);
int open_file_read(httpRequest* request);
off_t write_file_to_log(int filefd, httpRequest* request, LogThreadArgs* args);
bool process_health_request(httpRequest* request, JobThreadArgs* args);
off_t health_to_log(httpRequest* request, LogThreadArgs* args);
int put_open(httpRequest* request, JobThreadArgs* args);
bool parse_args(int argc, char* argv[], struct args*);


int main(int argc, char* argv[])
{
	struct args prog_args;
	memset(&prog_args, 0, sizeof(struct args));

	bool success;
	if (!(success = parse_args(argc, argv, &prog_args)))
	{
		return EXIT_FAILURE;
	}

	bool logging_on = false;
	int log_file_fd = -1;
	if (strlen(prog_args.log_filename) != 0)
	{
		logging_on = true;
		while ((log_file_fd = open(prog_args.log_filename, O_WRONLY | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR)) == -1)
		{
			if (errno == EINTR)
			{
				continue;
			}
		}

	}
	size_t num_threads = DEFAULT_THREAD_COUNT;
	if (prog_args.num_threads != 0)
	{
		num_threads = prog_args.num_threads;
	}

	// Perform server initialization
	struct sockaddr_in server_addr;
	int server_sock = server_init(&server_addr, prog_args.port);
	if (server_sock == -1)
	{
		return EXIT_FAILURE;
	}
	D(printf("Initialized server on port %i\n", prog_args.port));

	// Create shared data types
	JobQueue* job_pool = JobQueue_create();
	LogQueue* log_pool = LogQueue_create();
	FileList* file_list = FileList_create();

	off_t* log_file_offset = (off_t*) malloc(sizeof(off_t));
	*(log_file_offset) = 0;

	pthread_mutex_t dispatch_lock = PTHREAD_MUTEX_INITIALIZER;
	pthread_mutex_t log_pool_lock = PTHREAD_MUTEX_INITIALIZER;
	pthread_mutex_t read_write_lock = PTHREAD_MUTEX_INITIALIZER;
	pthread_mutex_t health_lock = PTHREAD_MUTEX_INITIALIZER;
	pthread_cond_t job_pool_empty = PTHREAD_COND_INITIALIZER;
	pthread_cond_t log_pool_empty = PTHREAD_COND_INITIALIZER;
	pthread_cond_t file_list_update = PTHREAD_COND_INITIALIZER;
	size_t log_count = 0;
	size_t log_fail_count = 0;

	// Prepare Job Threads
	JobThreadArgs* job_thread_args = (JobThreadArgs*) malloc(num_threads * sizeof(JobThreadArgs));
	for (size_t i = 0; i < num_threads; i++)
	{
		job_thread_args[i].dispatch_lock = &dispatch_lock;
		job_thread_args[i].job_pool_empty = &job_pool_empty;
		job_thread_args[i].log_pool_lock = &log_pool_lock;
		job_thread_args[i].log_pool_empty = &log_pool_empty;
		job_thread_args[i].read_write_lock = &read_write_lock;
		job_thread_args[i].file_list_update = &file_list_update;
		job_thread_args[i].health_lock = &health_lock;
		job_thread_args[i].job_pool = job_pool;
		job_thread_args[i].log_pool = log_pool;
		job_thread_args[i].file_list = file_list;
		job_thread_args[i].is_logging = logging_on;
		job_thread_args[i].thread_id = i;
		job_thread_args[i].log_count = &log_count;
		job_thread_args[i].log_fail_count = &log_fail_count;
	}
	pthread_t* job_threads = (pthread_t*) malloc(num_threads * sizeof(pthread_t));
	for (size_t i = 0; i < num_threads; i++)
	{
		pthread_create(&job_threads[i], NULL, handle_connection, (void*)&job_thread_args[i]);
	}

	pthread_t log_thread;
	LogThreadArgs log_thread_args;
	if (logging_on)
	{
		log_thread_args.log_file_offset = log_file_offset;
		log_thread_args.log_file_fd = log_file_fd;
		log_thread_args.log_pool = log_pool;
		log_thread_args.log_pool_lock = &log_pool_lock;
		log_thread_args.log_pool_empty = &log_pool_empty;
		log_thread_args.health_lock = &health_lock;
		log_thread_args.log_count = &log_count;
		log_thread_args.log_fail_count = &log_fail_count;
		pthread_create(&log_thread, NULL, handle_log_requests, (void*)&log_thread_args);
	}


	while (true)
	{
		// Prepare client socket and accept connection
		struct sockaddr client_addr;
		socklen_t client_len = sizeof(client_addr);
		int client_sock  = accept(server_sock, &client_addr, &client_len);

		if (client_sock == -1)
		{
			perror("accept()");
			continue;
		}
		D(printf("Connection accepted.\n"));

		// Add the connection to the job pool
		pthread_mutex_lock(&dispatch_lock);
		job_pool->enqueue(job_pool, client_sock);
		pthread_cond_signal(&job_pool_empty);
		pthread_mutex_unlock(&dispatch_lock);

	}


	close(server_sock);
	return EXIT_SUCCESS;
}

bool parse_args(int argc, char* argv[], struct args* prog_args)
{
	int opt;
	while ((opt = getopt(argc, argv, "N:l:")) != -1)
	{
		if ((optarg == 0) || (*optarg == '-'))
		{
			opt = ':';
			optind--;
		}
		if (opt == 'N')
		{
			long int num = strtol(optarg, NULL, 10);
			if (num <= 0)
			{
				fprintf(stderr, "Invalid argument\n");
				return false;
			}
			prog_args->num_threads = num;
		}
		else if (opt == 'l')
		{
			if (strlen(optarg) > LOG_FILENAME_SIZE)
			{
				fprintf(stderr, "Invalid log_file name\n");
				return false;
			}
			strncpy(prog_args->log_filename, optarg, LOG_FILENAME_SIZE + 1);
		}
		else if (opt == ':')
		{
			fprintf(stderr, "Option provided without corresponding argument\n");
			return false;
		}
		else if (opt == '?')
		{
			fprintf(stderr, "Option not recognized\n");
			return false;
		}
	}

	// get the port number
	if (optind < argc)
	{
		if ((prog_args->port = get_port(argv[optind])) == 0)
		{
			fprintf(stderr, "Invalid port\n");
			return false;
		}
	}
	else
	{
		fprintf(stderr, "Missing port number\n");
		return false;
	}
	if (++optind < argc)
	{
		fprintf(stderr, "Too many arguments\n");
		return false;
	}
	return true;
}

void* handle_connection(void* thread_args)
{
	JobThreadArgs* args = (JobThreadArgs*) thread_args;

	D(printf("Thread [%lu] initialized\n", args->thread_id));
	while (true)
	{
		// Get the client socket
		pthread_mutex_lock(args->dispatch_lock);
		while (args->job_pool->empty(args->job_pool))
		{
			pthread_cond_wait(args->job_pool_empty, args->dispatch_lock);
		}
		int client_sock = args->job_pool->dequeue(args->job_pool);
		pthread_mutex_unlock(args->dispatch_lock);
		D(printf("Thread [%lu] grabbed the job from socket [%i]\n", args->thread_id, client_sock));

		httpRequest* request = (httpRequest*) malloc(sizeof(httpRequest));
		if (!parse_request(client_sock, request))
		{
			D(printf("Thread [%lu] encountered an error parsing\n", args->thread_id));
			if (send_response_header(client_sock, request) && args->is_logging)
			{
				pthread_mutex_lock(args->log_pool_lock);
				args->log_pool->enqueue(args->log_pool, request);
				pthread_cond_signal(args->log_pool_empty);
				pthread_mutex_unlock(args->log_pool_lock);
			}
			else
			{
				free(request);
			}
		}
		else
		{
			request->client_sock = client_sock;
			request->file_open = false;
			request->temp_file_fd = -1;
			D(printf("Thread [%lu] working on file: %s\n", args->thread_id, request->filename));
			process_request(request, args);
		}
		close(client_sock);
		D(printf("Thread [%lu] returned\n", args->thread_id));

	}
}

void* handle_log_requests(void* thread_args)
{
	D(printf("Thread [log] initialized\n"));
	LogThreadArgs* args = (LogThreadArgs*) thread_args;

	while (true)
	{
		pthread_mutex_lock(args->log_pool_lock);
		while (args->log_pool->empty(args->log_pool))
		{
			pthread_cond_wait(args->log_pool_empty, args->log_pool_lock);
		}
		httpRequest* request = args->log_pool->dequeue(args->log_pool);
		pthread_mutex_unlock(args->log_pool_lock);

		D(printf("Thread [log] grabbed the job\n"));

		write_to_log(request, args);
		pthread_mutex_lock(args->health_lock);
		(*(args->log_count))++;
		if (!((request->status == OK) || (request->status == CREATED)))
		{
			(*(args->log_fail_count))++;
		}
		pthread_mutex_unlock(args->health_lock);
		
		free(request);
		
		D(printf("Thread [log] returned\n"));
	}
}



int server_init(struct sockaddr_in* server_addr, uint16_t port)
{
	// Initialize server address info
	server_addr->sin_family = AF_INET;
	server_addr->sin_port = htons(port);
	server_addr->sin_addr.s_addr = htonl(INADDR_ANY);

	// Create socket
	int server_sock = socket(AF_INET, SOCK_STREAM, 0);
	if (server_sock == -1)
	{
		perror("server_init()");
		return -1;
	}

	int8_t ret = 0;

	// Configure Socket, bind, set to listen
	socklen_t opt_val = 1;
	ret = setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, &opt_val, sizeof(opt_val));
	if (ret < 0)
	{
		perror("server_init():setsockopt()");
		return -1;
	}
	ret = bind(server_sock, (struct sockaddr*) server_addr, sizeof(struct sockaddr_in));
	if (ret < 0)
	{
		perror("server_init():bind()");
		return -1;
	}
	ret = listen(server_sock, BACK_LOG);

	if (ret < 0)
	{
		perror("server_init():listen()");
		return -1;
	}
	return server_sock;
}

uint16_t get_port(char* port)
{
	int64_t p = strtol(port, NULL, 10);
	if ((p <= PORT_MIN) || (p > UINT16_MAX))
	{
		return 0;
	}
	return (uint16_t)p;
}

bool get_request(int client_sock, httpRequest* request)
{
	memset(request, 0, sizeof(httpRequest));
	ssize_t bytes_read = 0;
	size_t total_bytes_read = 0;
	uint8_t* buf_start = request->buffer;
	uint16_t space_available = REQUEST_BUFFER_SIZE;
	bool good_request = true;

	// set a timeout in case the client doesn't send a complete request
	struct timeval time;
	time.tv_sec = RECV_TIMEOUT_SECONDS;
	time.tv_usec = 0;
	setsockopt(client_sock, SOL_SOCKET, SO_RCVTIMEO, &time, sizeof(time));

	// Try to receive request from the client
	do 
	{
		bytes_read = recv(client_sock, buf_start, space_available, 0);
		if (bytes_read == -1)
		{
			// recevied an interrupt signal, try again
			if (errno == EINTR)
			{
				continue;
			}
			// Recv timeout exceeded, request likely bad
			else if ((errno == EAGAIN) || (errno == EWOULDBLOCK))
			{
				good_request = false;
				break;
			}
			else
			{
				perror("get_request()");
				request->status = SERVER_ERROR;
				good_request = false;
				return false;
			}
		}
		// Client closed the connection
		else if ((bytes_read == 0) && (space_available != 0))
		{
			request->status = SERVER_ERROR;
			good_request = false;
		}
		total_bytes_read += (size_t)bytes_read;
		space_available -= (uint16_t)bytes_read;
		buf_start = (uint8_t*)(buf_start + bytes_read);
	} while (strstr((char*)request->buffer, "\r\n\r\n") == NULL);
	request->request_length = (uint16_t)total_bytes_read;

	// remove timeout
	time.tv_sec = 0;
	setsockopt(client_sock, SOL_SOCKET, SO_RCVTIMEO, &time, sizeof(time));

	return good_request;
}

bool parse_request(int client_sock, httpRequest* request)
{
	// Get the request from the client
	if (!get_request(client_sock, request))
	{
		return false;
	}

	// Get request method, resource name, protocol version
	char* next_line_start = strstr((char*)request->buffer, "\r\n");
	char method_buf[SMALL_BUFFER_SIZE];
	char filename_buf[FILENAME_BUFFER_SIZE + 1];
	char version_buf[SMALL_BUFFER_SIZE];
	memset(method_buf, 0, SMALL_BUFFER_SIZE * sizeof(char));
	memset(filename_buf, 0, (FILENAME_BUFFER_SIZE + 1)*sizeof(char));
	memset(version_buf, 0, SMALL_BUFFER_SIZE * sizeof(char));
	if (sscanf((char*)request->buffer, "%s %s %s", method_buf, filename_buf, version_buf) < 3)
	{
		request->status = BAD_REQUEST;
		return false;
	}
	if (strcmp(method_buf, "GET") == 0)
	{
		request->type = GET;
	}
	else if (strcmp(method_buf, "HEAD") == 0)
	{
		request->type = HEAD;
	}
	else if (strcmp(method_buf, "PUT") == 0)
	{
		request->type = PUT;
	}
	else
	{
		request->status = NOT_IMPLEMENTED;
		return false;
	}
	if ((strlen(filename_buf) > FILENAME_SIZE + 1))
	{
		request->status = BAD_REQUEST;
		return false;
	}
	else
	{
		if (filename_buf[0] != '/')
		{
			request->status = BAD_REQUEST;
			return false;
			
		}
		for (size_t i = 1; i <= (FILENAME_SIZE + 1) && filename_buf[i] != '\0'; i++)
		{
			if (!((filename_buf[i] == '-') || (filename_buf[i] == '_') || 
				(filename_buf[i] >= '0' && filename_buf[i] <= '9') || 
				(filename_buf[i] >= 'A' && filename_buf[i] <= 'Z') || 
				(filename_buf[i] >= 'a' && filename_buf[i] <= 'z')))
			{
				request->status = BAD_REQUEST;
				return false;
				
			}
		}
	}
	strncpy(request->filename, (filename_buf + 1), FILENAME_SIZE);
	if ((strlen(request->filename) == 0))
	{
		request->status = BAD_REQUEST;
		return false;
	}

	// Check protocol version
	if ((strcmp(version_buf, HTTP_VERSION) != 0))
	{
		request->status = BAD_REQUEST;
		return false;
	}

	// Get start index of the message body
	char* msg_body_start;
	if ((msg_body_start = strstr((char*)request->buffer, "\r\n\r\n")) == NULL)
	{
		request->status = BAD_REQUEST;
		return false;
	}
	else
	{
		request->body_offset = (uint16_t)((msg_body_start + 4) - (char*)request->buffer);
	}

	// Check all other headers
	bool content_length_found = false;
	bool expect_100_found = false;

	if (!(request->type != PUT && (next_line_start == msg_body_start)))
	{
		do
		{
			char* this_line_start = next_line_start + 2;
			next_line_start = strstr(this_line_start, "\r\n");
			char header[SMALL_BUFFER_SIZE];
			char header_content[SMALL_BUFFER_SIZE];
			memset(header, 0, SMALL_BUFFER_SIZE * sizeof(char));
			memset(header_content, 0, SMALL_BUFFER_SIZE * sizeof(char));

			if (sscanf(this_line_start, "%s %s", header, header_content) < 2)
			{
				request->status = BAD_REQUEST;
				return false;
			}

			if (header[strlen(header) - 1] != ':')
			{
				request->status = BAD_REQUEST;
				return false;
			}

			if (strcmp(header, "Content-Length:") == 0)
			{
				if (content_length_found == true)
				{
					request->status = BAD_REQUEST;
					return false;
				}
				content_length_found = true;
				int64_t len = strtol(header_content, NULL, 10);

				// Have to check if header_content is actually 0 because strtol()
				// returns 0 upon conversion error
				if (len == 0)
				{
					if (strcmp(header_content, "0") == 0)
					{
						request->content_length = 0;
					}
					else
					{
						request->status = BAD_REQUEST;
						return false;
					}
				}
				else if (len < 0)
				{
					request->status = BAD_REQUEST;
					return false;
				}
				request->content_length = (uint64_t) len;
			}
			else if ((strcmp(header, "Expect:") == 0) && (strcmp(header_content, "100-continue") == 0))
			{
				expect_100_found = true;
			}

		} while (strstr(next_line_start, "\r\n\r\n") != next_line_start);
	}


	if (!content_length_found && (request->type == PUT))
	{
		request->status = BAD_REQUEST;
		return false;
	}
	if (expect_100_found)
	{
		char buf[SMALL_BUFFER_SIZE];
		memset(buf, 0, SMALL_BUFFER_SIZE * sizeof(char));
		strcat(buf, "HTTP/1.1 100 Continue\r\n\r\n");
		if (send(client_sock, buf, strlen(buf), 0) == -1)
		{
			perror("parse_request():send()");
		}
	}

	return true;
}

bool get(httpRequest* request, JobThreadArgs* args)
{
	D(printf("Thread [%lu] entered get()\n", args->thread_id));
	// Get a file descriptor to the requested file
	// and fill request w/ necessary data
	int client_sock = request->client_sock;
	int filefd;
	if (strcmp(request->filename, "healthcheck") == 0)
	{
		if (args->is_logging)
		{
			return process_health_request(request, args);

		}
		else
		{
			request->status = NOT_FOUND;
			filefd = -1;
		}
	}
	else
	{
		filefd = head_helper(request, args);
	}
	

	// Send response header
	bool good_response = send_response_header(client_sock, request);
	

	if (filefd == -1)
	{
		return good_response;
	}

	// Open a temp file for logging
	if (args->is_logging)
	{
		while ((request->temp_file_fd = open(".", O_RDWR | __O_TMPFILE, S_IRUSR | S_IWUSR)) == -1)
		{
			if (errno == EINTR)
			{
				continue;
			}
			perror("get():open()");
			break;
		}
	}

	// Transfer the file to the client
	if (good_response)
	{
		(void)transfer_data(client_sock, filefd, (size_t)request->content_length, request->temp_file_fd);
	}

	close(filefd);

	D(printf("Thread [%lu] leaving get()\n", args->thread_id));
	return good_response;
}

bool send_response_header(int client_sock, httpRequest* request)
{
	char msg_buf[RESPONSE_BUFFER_SIZE];
	memset(msg_buf, 0, sizeof(char) * RESPONSE_BUFFER_SIZE);
	strcat(msg_buf, "HTTP/1.1 ");

	switch(request->status)
	{
		case OK:
			strcat(msg_buf, "200 OK\r\n");
			break;
		case CREATED:
			strcat(msg_buf, "201 Created\r\n");
			break;
		case BAD_REQUEST:
			strcat(msg_buf, "400 Bad Request\r\n");
			break;
		case FORBIDDEN:
			strcat(msg_buf, "403 Forbidden\r\n");
			break;
		case NOT_FOUND:
			strcat(msg_buf, "404 Not Found\r\n");
			break;
		case SERVER_ERROR:
			strcat(msg_buf, "500 Server Error\r\n");
			break;
		// Changed code here: class test cases assume 400 response!
		case NOT_IMPLEMENTED:
			strcat(msg_buf, "400 Bad Request\r\n");
			//strcat(msg_buf, "501 Not Implemented\r\n");
			break;
	}

	uint64_t content_length = 0;
	if (request->status == OK && ((request->type == GET) || (request->type == HEAD)))
	{
		content_length = request->content_length;
	}
	strcat(msg_buf, "Content-Length: ");
	char file_size[SMALL_BUFFER_SIZE];
	memset(file_size, 0, sizeof(char) * SMALL_BUFFER_SIZE);
	sprintf(file_size, "%li", content_length);
	strcat(msg_buf, file_size);
	strcat(msg_buf, "\r\n");
	strcat(msg_buf, "\r\n");

	// Send the response
	size_t msg_size = strlen(msg_buf);
	size_t bytes_left = msg_size;
	size_t total_bytes_transferred = 0;
	while (total_bytes_transferred < msg_size)
	{
		ssize_t bytes_transferred = send(client_sock, msg_buf, bytes_left, 0);
		if (bytes_transferred == -1)
		{
			if (errno == EINTR)
			{
				continue;
			}
			if ((errno == ENOTCONN) || (errno == ECONNABORTED))
			{
				fprintf(stderr, "Client not connected\n");
			}
			perror("send_response_header():send()");
			return false;
		}
		total_bytes_transferred += (size_t) bytes_transferred;
		bytes_left -= (size_t) bytes_transferred;
	}

	return true;
}

int head_helper(httpRequest* request, JobThreadArgs* args)
{
	// First try to open the requested file
	pthread_mutex_lock(args->read_write_lock);
	while (!args->file_list->safe_to_read(args->file_list, request->filename))
	{
		D(printf("Thread [%lu] is waiting to open the file...\n", args->thread_id));
		pthread_cond_wait(args->file_list_update, args->read_write_lock);
	}

	int filefd = open_file_read(request);
	if (filefd == -1)
	{
		pthread_mutex_unlock(args->read_write_lock);
		return -1;
	}
	args->file_list->insert(args->file_list, request->filename, R);
	pthread_mutex_unlock(args->read_write_lock);
	request->file_open = true;

	// Get the size of the file
	struct stat file_stats;
	if (fstat(filefd, &file_stats) == -1)
	{
		request->status = SERVER_ERROR;
		perror("head_helper():fstat()");
		if (close(filefd) == -1)
		{
			perror("head_helper():close()");
		}
		filefd = -1;
	}
	size_t file_size = (size_t) file_stats.st_size;
	request->content_length = (uint64_t) file_size;
	if (!S_ISREG(file_stats.st_mode) || (!((file_stats.st_mode & S_IRUSR) == S_IRUSR)))
	{
		request->status = FORBIDDEN;
		if (close(filefd) == -1)
		{
			perror("head_helper():close()");
		}
		filefd = -1;
	}
	if (filefd == -1)
	{
		pthread_mutex_lock(args->read_write_lock);
		args->file_list->remove(args->file_list, request->filename, R);
		pthread_mutex_unlock(args->read_write_lock);
		request->file_open = false;
	}

	return filefd;
}

bool head(httpRequest* request, JobThreadArgs* args)
{
	D(printf("Thread [%lu] entered head()\n", args->thread_id));

	// Get a file descriptor to the requested file
	// and fill request w/ necessary data
	int filefd;
	if (strcmp(request->filename, "healthcheck") == 0)
	{
		request->status = FORBIDDEN;
		filefd = -1;
	}
	else
	{
		filefd = head_helper(request, args);
	}

	// Send the header
	bool good_response = send_response_header(request->client_sock, request);

	if (filefd != -1)
	{
		close(filefd);
	}
	
	D(printf("Thread [%lu] leaving head()\n", args->thread_id));

	return good_response;
}

bool put(httpRequest* request, JobThreadArgs* args)
{
	D(printf("Thread [%lu] entered put()\n", args->thread_id));
	int client_sock = request->client_sock;
	bool good_response = true;

	
	int filefd = put_open(request, args);

	if (filefd != -1)
	{
		D(printf("Thread [%lu] about to upload file...\n", args->thread_id));

		// Open a temp file for logging
		if (args->is_logging)
		{
			while ((request->temp_file_fd = open(".", O_RDWR | __O_TMPFILE, S_IRUSR | S_IWUSR)) == -1)
			{
				if (errno == EINTR)
				{
					continue;
				}
				perror("put():open()");
				break;
			}
		}

		(void)upload_file(client_sock, filefd, request);
		request->status = CREATED;
	}

	D(printf("Thread [%lu] about to send response...\n", args->thread_id));

	// Send response
	good_response = send_response_header(client_sock, request);

	if (filefd != -1)
	{
		close(filefd);
	}
	
	D(printf("Thread [%lu] leaving put()\n", args->thread_id));

	return good_response;
}

bool transfer_data(int out_fd, int in_fd, size_t file_size, int temp_log_file)
{
	size_t total_bytes_written = 0;
	while (total_bytes_written < file_size)
	{
		// Read the data from source into a buffer
		uint8_t file_buf[FILE_BUFFER_SIZE];
		memset(file_buf, 0, sizeof(uint8_t) * FILE_BUFFER_SIZE);
		size_t total_bytes_left = file_size - total_bytes_written;
		ssize_t bytes_read = 0;

		if (total_bytes_left >= FILE_BUFFER_SIZE)
		{
			bytes_read = read(in_fd, file_buf, FILE_BUFFER_SIZE);
		}
		else
		{
			bytes_read = read(in_fd, file_buf, total_bytes_left);
		}
		
		

		if (bytes_read == -1)
		{
			if (errno == EINTR)
			{
				continue;
			}
			else
			{
				perror("transfer_data():read()");
				return false;
			}
		}

		// Write to temp_log_file
		if (temp_log_file != -1)
		{
			uint8_t* file_buf_offset = file_buf;
			ssize_t bytes_written_temp = 0;
			ssize_t bytes = 0;
			while (bytes_written_temp < bytes_read)
			{
				bytes = write(temp_log_file, file_buf_offset, bytes_read);
				if (bytes == -1)
				{
					if (errno == EINTR)
					{
						continue;
					}
					perror("transfer_data():write(temp_log_file)");
					break;
				}
				bytes_written_temp += bytes;
				file_buf_offset = (uint8_t*)(file_buf_offset + bytes);
			}
		}

		// Write all the data from the buffer into the destination
		size_t bytes_written = 0;
		size_t bytes_to_write = (size_t)bytes_read;
		uint8_t* file_buf_start = file_buf;
		while (bytes_written < (size_t)bytes_read)
		{
			ssize_t bytes_written_this_loop = write(out_fd, file_buf_start, bytes_to_write);
			if (bytes_written_this_loop == -1)
			{
				if (errno == EINTR)
				{
					continue;
				}
				perror("transfer_data():write(out_fd)");
				return false;
			}
			file_buf_start = (uint8_t*)(file_buf_start + bytes_written_this_loop);
			bytes_written += (size_t)bytes_written_this_loop;
			bytes_to_write -= (size_t)(bytes_written_this_loop);
		}
		total_bytes_written += bytes_written;
	}

	return true;
}

bool upload_file(int client_sock, int filefd, httpRequest* request)
{
	// Write remaining data from request buffer to the file
	size_t total_file_size = (size_t)request->content_length;
	size_t total_bytes_written = 0;
	size_t bytes_in_buf;
	if ((bytes_in_buf = (size_t)(request->request_length - request->body_offset)) != 0)
	{
		// Need to loop through a write() to ensure all bytes are written
		uint8_t* buf_start = (uint8_t*)(request->buffer + request->body_offset);
		size_t bytes_left = bytes_in_buf;

		// Write to temp log file
		if (request->temp_file_fd != -1)
		{
			ssize_t bytes = write(request->temp_file_fd, buf_start, bytes_in_buf);
			if (bytes < 0)
			{
				perror("upload_file():write()");
			}
		}

		do
		{
			ssize_t bytes_written = write(filefd, buf_start, bytes_left);
			if (bytes_written == -1)
			{
				if (errno == EINTR)
				{
					continue;
				}
				perror("upload_file():write(filefd)");
				request->status = SERVER_ERROR;
				return false;
			}
			total_bytes_written += (size_t)bytes_written;
			bytes_left -= (size_t)bytes_written;
			buf_start = (uint8_t*)(buf_start + bytes_written);
		} while (total_bytes_written < bytes_in_buf);
	}

	// Write remaining data from socket into the file
	if (!transfer_data(filefd, client_sock, (total_file_size - total_bytes_written), request->temp_file_fd))
	{
		return false;
	}

	return true;
}

void process_request(httpRequest* request, JobThreadArgs* args)
{
	bool good_response = true;
	switch(request->type)
	{
		case GET:
			good_response = get(request, args);
			break;
		case PUT:
			good_response = put(request, args);
			break;
		case HEAD:
			good_response = head(request, args);
			break;
		default:
			fprintf(stderr, "process_request(): Received a bad request\n");
			break;
	}

	cleanup(request, args);

	// Pass data to logger if it's being used
	if (args->is_logging && good_response)
	{
		pthread_mutex_lock(args->log_pool_lock);
		args->log_pool->enqueue(args->log_pool, request);
		pthread_cond_signal(args->log_pool_empty);
		pthread_mutex_unlock(args->log_pool_lock);
	}
	else if (args->is_logging)
	{
		if (request->temp_file_fd != -1)
		{
			if (close(request->temp_file_fd) == -1)
			{
				perror("cleanup():close()");
			}
		}
		free(request);
	}
	else
	{
		free(request);
	}

}

// Caution: This function should only be used if the file specified by filename has been opened!
// Otherwise the specified file could be removed from file_list even if another thread was using it
void cleanup(httpRequest* request, JobThreadArgs* args)
{
	// stop tracking the files
	if (request->file_open)
	{
		if (request->type == PUT)
		{
			pthread_mutex_lock(args->read_write_lock);
			args->file_list->remove(args->file_list, request->filename, W);
			pthread_cond_signal(args->file_list_update);
			pthread_mutex_unlock(args->read_write_lock);
		}


		else if ((request->type == GET) || (request->type == HEAD))
		{
			pthread_mutex_lock(args->read_write_lock);
			args->file_list->remove(args->file_list, request->filename, R);
			pthread_cond_signal(args->file_list_update);
			pthread_mutex_unlock(args->read_write_lock);
		}
	}
}

void write_to_log(httpRequest* request, LogThreadArgs* args)
{
	D(printf("Entered write_to_log()\n"));

	// write the headers to logfile
	*(args->log_file_offset) = write_log_header(request, args);

	if (!((request->status == OK) || (request->status == CREATED)) || (request->type == HEAD))
	{
		// you are done --> exit
		return;
	}

	// open the file
	int filefd = -1;
	filefd = request->temp_file_fd;

	// write the file contents to the log
	
	*(args->log_file_offset) = write_file_to_log(filefd, request, args);

	if (filefd != -1)
	{
		close(filefd);
	}
}

off_t write_log_header(httpRequest* request, LogThreadArgs* args)
{
	D(printf("Entered write_log_header()\n"));
	char head_buf[RESPONSE_BUFFER_SIZE];
	memset(head_buf, 0, RESPONSE_BUFFER_SIZE * sizeof(char));

	// if request was unsuccesful
	if (!((request->status == OK) || (request->status == CREATED)))
	{
		strcat(head_buf, "FAIL: ");
		char temp_buf[REQUEST_BUFFER_SIZE];

		if (sscanf((char*)request->buffer, "%[^\r\n]", temp_buf) == 1)
		{
			if (strlen(temp_buf) < (RESPONSE_BUFFER_SIZE - 6))
			{
				strcat(head_buf, temp_buf);
			}
		}
	}
	else if (request->type == GET)
	{
		strcat(head_buf, "GET ");
	}
	else if (request->type == HEAD)
	{
		strcat(head_buf, "HEAD ");
	}
	else
	{
		strcat(head_buf, "PUT ");
	}	

	if (!((request->status == OK) || (request->status == CREATED)))
	{
		
		strcat(head_buf, " --- response ");
		switch(request->status)
		{
			case OK:
				strcat(head_buf, "200\n");
				break;
			case CREATED:
				strcat(head_buf, "201\n");
				break;
			case BAD_REQUEST:
				strcat(head_buf, "400\n");
				break;
			case FORBIDDEN:
				strcat(head_buf, "403\n");
				break;
			case NOT_FOUND:
				strcat(head_buf, "404\n");
				break;
			case SERVER_ERROR:
				strcat(head_buf, "500\n");
				break;
			// Updated per class spec requirements
			case NOT_IMPLEMENTED:
				strcat(head_buf, "400\n");
				// strcat(head_buf, "501\n");
				break;
		}
	}
	else
	{
		strcat(head_buf, "/");
		strcat(head_buf, request->filename);

		strcat(head_buf, " length ");
		char temp_buf[SMALL_BUFFER_SIZE];
		sprintf(temp_buf, "%li", request->content_length);
		strcat(head_buf, temp_buf);
		strcat(head_buf, "\n");
	}
	if ((request->type == HEAD) || !((request->status == OK || (request->status == CREATED))))
	{
		strcat(head_buf, "========\n");
	}

	// write to the logfile
	off_t offset = *(args->log_file_offset);
	size_t bytes_to_write = strlen(head_buf);
	ssize_t bytes_written = 0;
	char* head_buf_offset = head_buf;
	while (bytes_written < (ssize_t)strlen(head_buf))
	{
		ssize_t bytes = pwrite(args->log_file_fd, head_buf_offset, bytes_to_write, offset);
		if (bytes == -1)
		{
			if ((errno == EAGAIN) || (errno == EINTR))
			{
				continue;
			}
			perror("write_log_header():pwrite()");
			return *(args->log_file_offset);
		}

		bytes_written += bytes;
		offset += (off_t)bytes;
		head_buf_offset = (char*)(head_buf_offset + bytes);
		bytes_to_write = bytes_to_write - (size_t)bytes;
	}
	return offset;
}

int open_file_read(httpRequest* request)
{
	int filefd;
	do
	{
		filefd = open(request->filename, O_RDONLY);
		if (filefd == -1)
		{
			// The call was interrupted, try again
			if (errno == EINTR)
			{
				continue;
			}
			else if (errno == ENOENT)
			{
				request->status = NOT_FOUND;
				return -1;
			}
			else if ((errno == EACCES) || (errno == EISDIR) || (errno == EPERM))
			{
				request->status = FORBIDDEN;
				return -1;
			}
			else
			{
				request->status = SERVER_ERROR;
				perror("open_file_read()");
				return -1;
			}
		}
	} while (filefd == -1);
	return filefd;
}

off_t write_file_to_log(int filefd, httpRequest* request, LogThreadArgs* args)
{
	D(printf("Entered write_file_to_log()\n"));
	off_t log_offset = *(args->log_file_offset);
	ssize_t total_bytes_read = 0;
	size_t bytes_left_to_read = (size_t)request->content_length;
	ssize_t total_bytes_written = 0;
	ssize_t bytes_this_line = 0;

	// this is to handles files with 0 content length
	bool entered_once = false;
	lseek(filefd, 0, SEEK_SET);

	struct stat mystat;
	fstat(filefd, &mystat);
	D(printf("temp file [%i] size: %lu\n", filefd, mystat.st_size));

	if (request->content_length < (size_t)mystat.st_size)
	{
		request->content_length = mystat.st_size;
	}

	// Read and write while theres more bytes to be read from the file
	while ((total_bytes_read < (ssize_t)request->content_length) || !entered_once)
	{
		entered_once = true;
		uint8_t read_buf [SMALL_FILE_BUFFER_SIZE];
		ssize_t bytes_read = 0;
		
		size_t bytes_to_read = SMALL_BUFFER_SIZE;
		if (((ssize_t)request->content_length - total_bytes_read) < SMALL_BUFFER_SIZE)
		{
			bytes_to_read = (size_t)(request->content_length - total_bytes_read);
		}
		bytes_read = read(filefd, read_buf, bytes_to_read);
		if (bytes_read == -1)
		{
			if ((errno == EINTR) || (errno == EAGAIN))
			{
				continue;
			}
			else
			{
				perror("write_file_to_log():read()");
				return *(args->log_file_offset);
			}
		}
		

		total_bytes_read += bytes_read;
		bytes_left_to_read -= (size_t) bytes_read;

		// convert the bytes into hex and format
		uint8_t log_buf[LOG_FILE_BUFFER_SIZE];
		memset(log_buf, 0, sizeof(uint8_t) * LOG_FILE_BUFFER_SIZE);
		size_t buf_index = 0;
		for (size_t i = 0; i < (size_t)bytes_read; i++)
		{
			if (bytes_this_line == 0)
			{
				int ret = snprintf((char*)&log_buf[buf_index], LOG_FILE_BUFFER_SIZE - buf_index, "%08lu", total_bytes_written);
				buf_index += ret;
			}
			int ret = snprintf((char*)&log_buf[buf_index], LOG_FILE_BUFFER_SIZE - buf_index, " %02x", read_buf[i]);
			buf_index += ret;
			bytes_this_line++;
			total_bytes_written++;

			if (bytes_this_line == BYTES_PER_LOG_LINE)
			{
				bytes_this_line = 0;
				snprintf((char*)&log_buf[buf_index], LOG_FILE_BUFFER_SIZE - buf_index, "%s", "\n");
				buf_index++;
			}
		}
		
		// Mark the end of the log entry
		if (total_bytes_written == (ssize_t)request->content_length)
		{
			if (bytes_this_line != 0)
			{
				strcat((char*)&log_buf, "\n========\n");
			}
			else
			{
				strcat((char*)&log_buf, "========\n");
			}
		}


		// Write the formatted entry to the log file
		ssize_t total_bytes_written_this_loop = 0;
		ssize_t bytes_left_to_write = (ssize_t)strlen((char*)log_buf);
		uint8_t* buf_ptr = log_buf;
		while (total_bytes_written_this_loop < (ssize_t)strlen((char*)log_buf))
		{
			ssize_t bytes_written = pwrite(args->log_file_fd, buf_ptr, bytes_left_to_write, log_offset);
			if (bytes_written == -1)
			{
				if ((errno == EAGAIN) || (errno == EINTR))
				{
					continue;
				}
				else
				{
					perror("write_file_to_log():pwrite()");
					return *(args->log_file_offset);
				}
			}
			else
			{

			}
			total_bytes_written_this_loop += bytes_written;
			buf_ptr = (uint8_t*)(buf_ptr + bytes_written);
			log_offset += (off_t)bytes_written;
			bytes_left_to_write -= bytes_written;
		}

	}

	return log_offset;
}

bool process_health_request(httpRequest* request, JobThreadArgs* args)
{
	D(printf("Thread [%lu] entered process_health_request()\n", args->thread_id));
	size_t log_count = 0;
	size_t log_fails = 0;
	
	pthread_mutex_lock(args->health_lock);
	log_count = *(args->log_count);
	log_fails = *(args->log_fail_count);
	pthread_mutex_unlock(args->health_lock);

	uint8_t buf[RESPONSE_BUFFER_SIZE];
	memset(buf, 0, RESPONSE_BUFFER_SIZE * sizeof(uint8_t));
	int off = sprintf((char*)buf, "HTTP/1.1 200 OK\r\nContent-Length: ");


	memset(request->buffer, 0, REQUEST_BUFFER_SIZE * sizeof(uint8_t));
	uint8_t count_buf[RESPONSE_BUFFER_SIZE];
	int ret = sprintf((char*)count_buf, "%lu\n%lu", log_fails, log_count);
	request->content_length = sprintf((char*)request->buffer, "%lu\n%lu", log_fails, log_count);
	snprintf((char*)&buf[off], RESPONSE_BUFFER_SIZE - off,"%i\r\n\r\n%s", ret, count_buf);

	// Open a temp file for logging
	if (args->is_logging)
	{
		while ((request->temp_file_fd = open(".", O_RDWR | __O_TMPFILE, S_IRUSR | S_IWUSR)) == -1)
		{
			if (errno == EINTR)
			{
				continue;
			}
			perror("get():open()");
			break;
		}
		if (write(request->temp_file_fd, request->buffer, request->content_length) == -1)
		{
			perror("process_request():write()");
		}
	}

	// send to the client
	size_t msg_size = strlen((char*)buf);
	size_t bytes_left = msg_size;
	size_t total_bytes_transferred = 0;
	while (total_bytes_transferred < msg_size)
	{
		ssize_t bytes_transferred = send(request->client_sock, buf, bytes_left, 0);
		if (bytes_transferred == -1)
		{
			if (errno == EINTR)
			{
				continue;
			}
			if ((errno == ENOTCONN) || (errno == ECONNABORTED))
			{
				fprintf(stderr, "Client not connected\n");
			}
			perror("process_health_request():send()");
			return false;
		}
		total_bytes_transferred += (size_t) bytes_transferred;
		bytes_left -= (size_t) bytes_transferred;
	}
	return true;
}

int put_open(httpRequest* request, JobThreadArgs* args)
{
	bool exists = true;
	if (strcmp(request->filename, "healthcheck") == 0)
	{
		request->status = FORBIDDEN;
		return -1;
	}

	struct stat file_stats;

	if (stat(request->filename, &file_stats) == -1)
	{
		if (errno == EACCES)
		{
			request->status = FORBIDDEN;
			return -1;
		}
		else if (errno == ENOENT)
		{
			exists = false;
		}
		else
		{
			request->status = SERVER_ERROR;
			return -1;
		}
	}

	if (exists)
	{
		if (!((file_stats.st_mode & S_IFMT) == S_IFREG))
		{
			request->status = FORBIDDEN;
			return -1;
		}
		if (!((file_stats.st_mode & S_IWUSR) == S_IWUSR))
		{
			request->status = FORBIDDEN;
			return -1;
		}
	}

	// Try to open the requested file

	pthread_mutex_lock(args->read_write_lock);
	while (!args->file_list->safe_to_write(args->file_list, request->filename))
	{
		D(printf("Thread [%lu] is waiting to open the file...\n", args->thread_id));
		pthread_cond_wait(args->file_list_update, args->read_write_lock);
	}

	int filefd;
	do
	{
		filefd = open(request->filename, O_CREAT | O_WRONLY | O_TRUNC, 0644);
		if (filefd == -1)
		{
			// The call was interrupted, try again
			if (errno == EINTR)
			{
				continue;
			}
			else if ((errno == EACCES) || (errno == EISDIR) || (errno == EPERM))
			{
				request->status = FORBIDDEN;
				break;
			}
			else
			{
				request->status = SERVER_ERROR;
				perror("put_open():open()");
				break;
			}
		}
	} while (filefd == -1);

	if (filefd != -1)
	{
		request->file_open = true;
		args->file_list->insert(args->file_list, request->filename, W);
	}
	pthread_mutex_unlock(args->read_write_lock);

	return filefd;
}






