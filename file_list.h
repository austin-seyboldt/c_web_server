#ifndef FILE_LIST_H
#define FILE_LIST_H

#include "httpRequest.h"
#include <stdbool.h>

enum FILE_MODE {R, W, RW};

typedef struct FileListNode
{
	char file_name[FILENAME_SIZE + 1];
	enum FILE_MODE file_mode;
	struct FileListNode* next;
} FileListNode;

FileListNode* FileListNode_create();

typedef struct FileList
{
	FileListNode* root;
	void (*insert) (struct FileList* this, const char* file_name, enum FILE_MODE mode);
	void (*remove)(struct FileList* this, const char* file_name, enum FILE_MODE mode);
	bool (*safe_to_read)(struct FileList* this, const char* file_name);
	bool (*safe_to_write)(struct FileList* this, const char* file_name);
} FileList;

FileList* FileList_create();
void FileList_destroy(FileList* this);
FileListNode* FileList_destroy_helper(FileListNode*);
void FileList_insert(FileList* this, const char* file_name, enum FILE_MODE mode);
void FileList_remove(FileList* this, const char* file_name, enum FILE_MODE mode);
FileListNode* FileList_remove_helper(FileListNode*, const char* file_name, enum FILE_MODE mode);
bool FileList_safe_to_read(FileList* this, const char* file_name);
bool FileList_safe_to_read_helper(FileListNode*, const char* file_name);
bool FileList_safe_to_write(FileList* this, const char* file_name);
bool FileList_safe_to_write_helper(FileListNode* this, const char* file_name);


#endif