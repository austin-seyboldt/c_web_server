#ifndef HTTPREQUEST_H
#define HTTPREQUEST_H
#include <stdint.h>
#include <stdbool.h>

#define REQUEST_BUFFER_SIZE 4096
#define FILENAME_SIZE 27

enum request_type {GET, HEAD, PUT};
enum status_code {OK, CREATED, BAD_REQUEST, FORBIDDEN, NOT_FOUND, SERVER_ERROR, NOT_IMPLEMENTED};

typedef struct httpRequest 
{
	enum request_type type;
	char filename[FILENAME_SIZE + 1];
	uint64_t content_length;
	uint16_t body_offset;
	uint8_t buffer[REQUEST_BUFFER_SIZE + 1];
	uint16_t request_length;
	enum status_code status;
	int client_sock;
	bool file_open;
	int temp_file_fd;
} httpRequest;

#endif