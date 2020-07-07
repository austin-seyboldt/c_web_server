#ifndef THREAD_ARGS_H
#define THREAD_ARGS_H

#include "queue.h"
#include <pthread.h>
#include "file_list.h"
#include <sys/types.h>

typedef struct JobThreadArgs
{
	size_t thread_id;
	pthread_mutex_t* dispatch_lock;
	pthread_cond_t* job_pool_empty;
	pthread_mutex_t* log_pool_lock;
	pthread_cond_t* log_pool_empty;
	pthread_mutex_t* read_write_lock;
	pthread_cond_t* file_list_update;
	pthread_mutex_t* health_lock;
	JobQueue* job_pool;
	LogQueue* log_pool;
	LogQueue* health_pool;
	FileList* file_list;
	size_t* log_count;
	size_t* log_fail_count;
	bool is_logging;
} JobThreadArgs;

typedef struct LogThreadArgs
{
	int log_file_fd;
	off_t* log_file_offset;
	LogQueue* log_pool;
	pthread_mutex_t* log_pool_lock;
	pthread_mutex_t* log_file_lock;
	pthread_cond_t* log_pool_empty;
	pthread_mutex_t* health_lock;
	size_t* log_count;
	size_t* log_fail_count;
} LogThreadArgs;

#endif