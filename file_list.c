#include "file_list.h"
#include <string.h>
#include <stdlib.h>

FileListNode* FileListNode_create()
{
	FileListNode* this = (FileListNode*) malloc(sizeof(FileListNode));
	memset(this, 0, sizeof(FileListNode));
	this->next = NULL;
	return this;
}



FileList* FileList_create()
{
	FileList* this = (FileList*) malloc(sizeof(FileList));
	this->root = NULL;
	this->insert = FileList_insert;
	this->remove = FileList_remove;
	this->safe_to_read = FileList_safe_to_read;
	this->safe_to_write = FileList_safe_to_write;
	return this;
}

void FileList_destroy(FileList* this)
{
	if (this->root != NULL)
	{
		this->root = FileList_destroy_helper(this->root);
	}
	free(this);
}

FileListNode* FileList_destroy_helper(FileListNode* this)
{
	if (this == NULL)
	{
		return NULL;
	}
	this->next = FileList_destroy_helper(this->next);
	free(this);
	return NULL;
}

void FileList_insert(FileList* this, const char* file_name, enum FILE_MODE mode)
{
	if (this->root == NULL)
	{
		this->root = FileListNode_create();
		this->root->file_mode = mode;
		strncpy(this->root->file_name, file_name, FILENAME_SIZE);
		return;
	}
	FileListNode* tail = this->root;
	while (tail->next != NULL)
	{
		tail = tail->next;
	}
	tail->next = FileListNode_create();
	tail = tail->next;
	tail->file_mode = mode;
	strncpy(tail->file_name, file_name, FILENAME_SIZE);
	return;
}

void FileList_remove(FileList* this, const char* file_name, enum FILE_MODE mode)
{
	this->root = FileList_remove_helper(this->root, file_name, mode);
}

FileListNode* FileList_remove_helper(FileListNode* this, const char* file_name, enum FILE_MODE mode)
{
	if (this == NULL)
	{
		return NULL;
	}
	if (strcmp(this->file_name, file_name) == 0)
	{
		if (this->file_mode == mode)
		{
			FileListNode* temp = this->next;
			free(this);
			return temp;
		}
	}
	this->next = FileList_remove_helper(this->next, file_name, mode);
	return this;
}

bool FileList_safe_to_read(FileList* this, const char* file_name)
{
	return FileList_safe_to_read_helper(this->root, file_name);
}

bool FileList_safe_to_read_helper(FileListNode* this, const char* file_name)
{
	if (this == NULL)
	{
		return true;
	}
	if (strcmp(this->file_name, file_name) == 0)
	{
		if (this->file_mode == R)
		{
			return true;
		}
		return false;
	}
	return FileList_safe_to_read_helper(this->next, file_name);
}

bool FileList_safe_to_write(FileList* this, const char* file_name)
{
	return FileList_safe_to_write_helper(this->root, file_name);
}

bool FileList_safe_to_write_helper(FileListNode* this, const char* file_name)
{
	if (this == NULL)
	{
		return true;
	}
	if (strcmp(this->file_name, file_name) == 0)
	{
		return false;
	}
	return FileList_safe_to_write_helper(this->next, file_name);
}