#ifndef _PIPE_H_
#define _PIPE_H_

#include "util/types.h"

#define PIPE_BUFFER_SIZE 4096

typedef struct pipe_t {
  int refcnt;
  int read_open;
  int write_open;
  int read_pos;
  int write_pos;
  int data_size;
  char buffer[PIPE_BUFFER_SIZE];
} pipe_t;

pipe_t *pipe_alloc(void);
void pipe_incref(pipe_t *pipe);
void pipe_decref(pipe_t *pipe);
int pipe_read(pipe_t *pipe, char *buf, int count);
int pipe_write(pipe_t *pipe, const char *buf, int count);
void pipe_close_end(pipe_t *pipe, int kind);

#endif
