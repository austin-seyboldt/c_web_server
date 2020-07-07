#include "queue.h"
#include <stdlib.h>


LogQueueNode* LogQueueNode_create()
{
	LogQueueNode* this = (LogQueueNode*)malloc(sizeof(LogQueueNode));
	this->request = NULL;
	this->next = NULL;
	return this;
}



LogQueue* LogQueue_create()
{
	LogQueue* this = (LogQueue*)malloc(sizeof(LogQueue));
	this->enqueue = LogQueue_enqueue;
	this->dequeue = LogQueue_dequeue;
	this->empty = LogQueue_empty;
	this->root = NULL;
	this->tail = NULL;
	return this;
}

void LogQueue_enqueue(LogQueue* this, httpRequest* request)
{
	if (this == NULL)
	{
		return;
	}
	if (this->root == NULL)
	{
		this->root = LogQueueNode_create();
		this->tail = this->root;
		this->tail->request = request;
	}
	else
	{
		this->tail->next = LogQueueNode_create();
		this->tail = this->tail->next;
		this->tail->request = request;
	}
}

httpRequest* LogQueue_dequeue(LogQueue* this)
{
	if ((this == NULL) || (this->root == NULL))
	{
		return NULL;
	}
	// If the there's only 1 item, tail must be updated
	if (this->root == this->tail)
	{
		this->tail = NULL;
	}
	LogQueueNode* temp = this->root->next;
	httpRequest* request = this->root->request;
	free(this->root);
	this->root = temp;
	return request;
}

void LogQueue_destroy(LogQueue* this)
{
	if (this == NULL)
	{
		return;
	}
	this->root = LogQueue_destroy_helper(this->root);
	this->tail = NULL;
	free(this);
}

LogQueueNode* LogQueue_destroy_helper(LogQueueNode* this)
{
	if (this == NULL)
	{
		return NULL;
	}
	this->next = LogQueue_destroy_helper(this->next);
	free(this->request);
	free(this);
	return NULL;
}

bool LogQueue_empty(LogQueue* this)
{
	if (this->root == NULL)
	{
		return true;
	}
	return false;
}