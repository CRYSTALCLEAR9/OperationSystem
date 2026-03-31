#include "pipe.h"

#include "pmm.h"
#include "sync_utils.h"
#include "util/string.h"
#include "vfs.h"

static volatile int g_pipe_lock __attribute__((aligned(8))) = 0;

pipe_t *pipe_alloc(void) {
  pipe_t *pipe = (pipe_t *)alloc_page();
  if (pipe == 0) return 0;
  memset(pipe, 0, sizeof(*pipe));
  pipe->refcnt = 2;
  pipe->read_open = 1;
  pipe->write_open = 1;
  return pipe;
}

void pipe_incref(pipe_t *pipe) {
  if (pipe == 0) return;
  spin_lock(&g_pipe_lock);
  pipe->refcnt++;
  spin_unlock(&g_pipe_lock);
}

void pipe_decref(pipe_t *pipe) {
  int free_it = 0;
  if (pipe == 0) return;

  spin_lock(&g_pipe_lock);
  pipe->refcnt--;
  if (pipe->refcnt == 0) free_it = 1;
  spin_unlock(&g_pipe_lock);

  if (free_it) free_page(pipe);
}

int pipe_read(pipe_t *pipe, char *buf, int count) {
  if (pipe == 0 || buf == 0 || count <= 0) return 0;

  spin_lock(&g_pipe_lock);
  int readable = pipe->data_size < count ? pipe->data_size : count;
  for (int i = 0; i < readable; i++) {
    buf[i] = pipe->buffer[pipe->read_pos];
    pipe->read_pos = (pipe->read_pos + 1) % PIPE_BUFFER_SIZE;
  }
  pipe->data_size -= readable;
  spin_unlock(&g_pipe_lock);
  return readable;
}

int pipe_write(pipe_t *pipe, const char *buf, int count) {
  if (pipe == 0 || buf == 0 || count <= 0) return 0;

  spin_lock(&g_pipe_lock);
  int writable = PIPE_BUFFER_SIZE - pipe->data_size;
  if (writable > count) writable = count;
  for (int i = 0; i < writable; i++) {
    pipe->buffer[pipe->write_pos] = buf[i];
    pipe->write_pos = (pipe->write_pos + 1) % PIPE_BUFFER_SIZE;
  }
  pipe->data_size += writable;
  spin_unlock(&g_pipe_lock);
  return writable;
}

void pipe_close_end(pipe_t *pipe, int kind) {
  if (pipe == 0) return;

  spin_lock(&g_pipe_lock);
  if (kind == FILE_KIND_PIPE_READ)
    pipe->read_open = 0;
  else if (kind == FILE_KIND_PIPE_WRITE)
    pipe->write_open = 0;
  spin_unlock(&g_pipe_lock);
}
