/*
 * header file to be used by applications.
 */

#ifndef _USER_LIB_H_
#define _USER_LIB_H_
#include "util/types.h"
#include "kernel/proc_file.h"

int printu(const char *s, ...);
int exit(int code);
void* naive_malloc();
void naive_free(void* va);
void* better_malloc(int n);
void better_free(void* va);
int fork();
void yield();
int exec(const char *path, const char *arg);
int wait(int pid);
void print_backtrace(int depth);
void printpa(int* va);
int sem_new(int init_value);
int sem_free(int sem_id);
void sem_P(int sem_id);
void sem_V(int sem_id);

// added @ lab4_1
int open(const char *pathname, int flags);
int read_u(int fd, void *buf, uint64 count);
int write_u(int fd, void *buf, uint64 count);
int lseek_u(int fd, int offset, int whence);
int stat_u(int fd, struct istat *istat);
int disk_stat_u(int fd, struct istat *istat);
int close(int fd);

// added @ lab4_2
int opendir_u(const char *pathname);
int readdir_u(int fd, struct dir *dir);
int mkdir_u(const char *pathname);
int closedir_u(int fd);

// added @ lab4_3
int link_u(const char *fn1, const char *fn2);
int unlink_u(const char *fn);
int read_cwd(char *path);
int change_cwd(const char *path);
int pipe_u(int fds[2]);
int set_stdin(int fd);
int set_stdout(int fd);
int read_stdin(void *buf, uint64 count);


#endif
