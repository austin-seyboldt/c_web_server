#ifndef QUEUE_H
#define QUEUE_H
#include <stdbool.h>
#include "httpRequest.h"
#include <stdlib.h>

// Job Pool Header
typedef struct JobQueueNode {
	int client_sock;
	struct JobQueueNode* next;
} JobQueueNode;

JobQueueNode* JobQueueNode_create();

typedef struct JobQueue {
	JobQueueNode* root;
	JobQueueNode* tail;
	void (*enqueue)(struct JobQueue* this, int client_sock);
	int (*dequeue)(struct JobQueue* this);
	bool (*empty)(struct JobQueue* this);
} JobQueue;

JobQueue* JobQueue_create();
void JobQueue_enqueue(JobQueue* this, int client_sock);
int JobQueue_dequeue(JobQueue* this);
void JobQueue_destroy(JobQueue* this);
JobQueueNode* JobQueue_destroy_helper(JobQueueNode* this);
bool JobQueue_empty(JobQueue* this);




// Log Pool Header
typedef struct LogQueueNode {
	httpRequest* request;
	struct LogQueueNode* next;
} LogQueueNode;

LogQueueNode* LogQueueNode_create();

typedef struct LogQueue {
	LogQueueNode* root;
	LogQueueNode* tail;
	void (*enqueue)(struct LogQueue* this, httpRequest* request);
	httpRequest* (*dequeue)(struct LogQueue* this);
	bool (*empty)(struct LogQueue* this);
} LogQueue;

LogQueue* LogQueue_create();
void LogQueue_enqueue(LogQueue* this, httpRequest* request);
httpRequest* LogQueue_dequeue(LogQueue* this);
void LogQueue_destroy(LogQueue* this);
LogQueueNode* LogQueue_destroy_helper(LogQueueNode* this);
bool LogQueue_empty(LogQueue* this);

#endif