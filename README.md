## HTTP/1.1 Multithreaded Server
 
### Instructions
  
The program can be run in the terminal with the command "./httpserver". It requires at least one argument, the port number, which is a port number between 1024 - (2^16-1). In addition, two options may be provided: "-N" followed by the number of worker threads to use; "-l" followed by the name of the log file to log requests to. If the thread number is ommitted, the program uses the default value of 4. If the log file is ommitted then no logging is performed.  
  
### Known Issues
  
It is strongly recommended to disable logging if the workload is expected to consist of many large file requests because the logging can not only take quite long, but each log entry will be over 3 times larger than the file itself. Eventually these log requests will pile up and if the client requests a file that the logger is not yet finished with, the client will have to wait.  
  
Requests for the log file are unable to be processed properly if the log file is in use.  