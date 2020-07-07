# HTTP/1.1 Multithreaded Server Design Document
**Author:** Austin Seyboldt  
  
  
### Design Overview:
To turn my program from homework 1 into a multithreaded server I will need to make changes to ensure that it's thread safe. To do this, I will add a dispatcher module, which will listen for connections and add them to a request queue as they come in. From there, any one of the worker threads can grab a request when it becomes available and/or they finish a previous job. Each of these threads will execute within handle_connection(), which will contain an infinite loop that waits for new connections to be added to the request queue. Then the thread can mostly handle the request the same as before with some exceptions (namely, writing the data to a temp file as it's received if logging is on). After the request has been completed, the thread will hand off the request data to another thread, which is responsible for logging the requests to a file. As for handling the health checks, this is a trivial task: simply have the logger thread increment two variables when it receives a log request (a varible counting total log entries, and one counting failed requests).
  
The design will be greatly simplified by having only one thread writing to the logfile at a time, but it will require data to be read/written multiple times, which isn't ideal but it will simplify my code.  
  
Three main areas that will cause synchronization problems are: dispatching connections to threads, handing off log requests to a logging thread, and ensuring that two threads do not access the same file concurrently (unless both threads are only reading). To solve this, I will use 4 mutexes and 3 condition variables. Pseudocode for those is provided below.  

#### Additional Data Structures:
  
A structure will be needed to pass arguments into the thread when its created. This will include the file descriptor of the client socket, 4 shared mutexes for ensuring synchronization at different points, a pointer to a shared job pool (to grab clients from), a pointer to a shared log pool (to add finished job data to), 2 counter variables to track the number of completed and failed requests in the log, and a boolean to indicate whether the thread needs to pass data to a logging thread.  
  
There are also 3 shared condition variables: job_pool_empty to make threads wait until there's work to do (the dispatcher will signal), log_pool_empty to signal to the logging thread to wake up and do some work. file_list_update will be used to make threads wait before opening files that are already opened in an incompatible mode. This variable will be signaled whenever the list of open files is updated by a thread (when a file is opened or closed).  
  
```
struct ThreadArgs {
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
};
```  
  
This data structure is used to pass data to the logging thread when it is created. It will need a file descriptor to the log file, a pointer to the log pool queue, a mutex to protect access to the log pool, a mutex to protect access to the logfile, and 2 counter variables to track the number of total and failed log entries.

```
struct LogThreadArgs {
	int log_file_fd;
	off_t* log_file_offset;
	LogQueue* log_pool;
	pthread_mutex_t* log_pool_lock;
	pthread_cond_t* log_pool_empty;
	pthread_mutex_t* health_lock;
	size_t* log_count;
	size_t* log_fail_count;
}
```  
  
Additionally, 2 queues and a linked list will be needed. There needs to be a queue for the connections, and a queue for the requests to be logged. We specifically want a queue so that those connections / finished jobs that come in first will be handled first.  
  
The linked list will be used to store the file names and open mode of files currently being worked on. This will prevent concurrent reads/writes or writes/writes on the same file.  
  
#### Syncronization Mechanics:
Assume the variable file_list is a shared linked list that keeps track of open files.  
  
``` 
dispatcher:
	while true:
		client_sock = accept()
		pthread_mutex_lock(dispatch_lock)
		request_pool.enqueue(client_sock)
		pthread_cond_signal(job_pool_empty)
		pthread_mutex_unlock(dispatch lock)
```  
  
```
handle_connection:
	while true:
		pthread_mutex_lock(dispatch_lock)
		while (request_pool->is_empty())
			pthread_cond_wait(job_pool_empty)

		client_sock = request_pool->dequeue()
		pthread_mutex_unlock(dispatch_lock)
```  
  
```
put/get/head:
	pthread_mutex_lock(read_write_lock)
	while (the file is already open in another thread in incompatible mode)
		pthread_cond_wait(file_list_update)
	// open the file
	// add the file to file_list
	pthread_mutex_unlock(read_write_lock)

	// Do processing

	if (logging is on)
		pthread_mutex_lock(log_file)
		log_pool->enqueue(request)
		pthread_cond_signal(log_pool_empty)
		pthread_mutex_unlock(log_lock)
		
	pthread_mutex_lock(read_write_lock)
	// perform cleanup, close the file
	// remove the file from file_list
	pthread_cond_signal(file_list_update)
	pthread_mutex_unlock(read_write_lock)

```  
  
```
logger:
	while true:
		pthread_mutex_lock(log_lock)
		while (log_pool->is_empty())
			pthread_cond_wait(log_pool_empty)
		request = log_pool->dequeue()
		pthread_mutex_unlock(log_lock)

		write_to_log()
		pthread_mutex_lock(health_lock)
		log_count++
		if (log entry was a failed request)
			log_fail_count++
		pthread_mutex_unlock(health_lock)
		// Perform cleanup
		

		pthread_mutex_lock(read_write_lock)
		// remove the file from the file list
		pthread_cond_signal(file_list_update)
		pthread_mutex_unlock(read_write_lock)
```  
  
```
process_health_request:
	pthread_mutex_lock(args->health_lock)
	total_log_count = args->log_count
	fail_count = args->log_fail_count
	pthread_mutex_unlock(args->health_lock)

	// Continue processing
```
    
#### New Functionality:
  
```
write_to_log:
	file offset = write_log_headers()
	if (the request failed or it was a head request)
		return
	open the specified file
	file offset = write_file_to_log()
	close the file
	return
```  
  
```
off_t write_log_headers:
	char buf[RESPONDE_BUFFER_SIZE]
	if (request failed)
		concatenate "FAIL" to buf
	concatenate request type to buf
	concatenate the file name to buf
	if (request failed)
		concatenate "HTTP/1.1 --- response" + error code
	else
		concatenate the content length
	if (request failed or type was HEAD)
		concatenate "========\n"
	write the buf to the log file
	return the new file offset
```  
  
```
off_t write_file_to_log():

	total_bytes_written = 0

	while (not EOF):
		char read_buf
		read from file into read_buf

		char write_buf
		bytes_this_line = 0
		for (every byte in read_buf):
			if (bytes_this_line == 0)
				add total_bytes_written to write_buf
			put hex version of read_buf[i] into write_buf
			total_bytes_written++
			bytes_this_line++
			if (bytes_this_line)
				bytes_this_line = 0
				add "\n" to write_buf

		if (end of file)
			add "========\n" to write_buf

		write write_buf to the log file

	return the new log file offset

```  
  
```
process_health_request:
	pthread_mutex_lock(args->health_lock)
	total_log_count = args->log_count
	fail_count = args->log_fail_count
	pthread_mutex_unlock(args->health_lock)

	char buf[RESPONSE_BUFFER_SIZE]
	buf <-- "HTTP/1.1 200 OK Content-Length: "
	char temp_buf[] <-- fail_count + "\n" + total_count
	buf <-- strlen(temp_buf) + "\r\n\r\n"
	buf <-- temp_buf

	send buf to client
```
  
#### Additional Changes:
  
Another additional member was added to struct httpRequest: bool file_open. This is used during the cleanup process after the request has been handled to check if the specified file has been opened and whether it should then be removed the file_list.

The httpRequest struct now has an int file descriptor to a temp file used for logging.