/*
 * The supporting library for applications.
 */

#include "user_lib.h"
#include "util/types.h"
#include "util/snprintf.h"
#include "kernel/riscv.h"
#include "kernel/syscall.h"

uint64 do_user_call(uint64 sysnum, uint64 a1, uint64 a2, uint64 a3, uint64 a4, uint64 a5, uint64 a6,
                 uint64 a7) {
  int ret;

  asm volatile(
      "ecall\n"
      "sw a0, %0"
      : "=m"(ret)
      :
      : "memory");

  return ret;
}

int printu(const char* s, ...) {
  va_list vl;
  va_start(vl, s);

  char out[256];
  int res = vsnprintf(out, sizeof(out), s, vl);
  va_end(vl);
  const char* buf = out;
  size_t n = res < sizeof(out) ? res : sizeof(out);
  return do_user_call(SYS_user_print, (uint64)buf, n, 0, 0, 0, 0, 0);
}

int exit(int code) {
  return do_user_call(SYS_user_exit, code, 0, 0, 0, 0, 0, 0);
}

void* naive_malloc() {
  return (void*)do_user_call(SYS_user_allocate_page, PGSIZE, 0, 0, 0, 0, 0, 0);
}

void naive_free(void* va) {
  do_user_call(SYS_user_free_page, (uint64)va, 0, 0, 0, 0, 0, 0);
}

void* better_malloc(int n) {
  return (void*)do_user_call(SYS_user_allocate_page, n, 0, 0, 0, 0, 0, 0);
}

void better_free(void* va) {
  do_user_call(SYS_user_free_page, (uint64)va, 0, 0, 0, 0, 0, 0);
}

int fork() {
  return do_user_call(SYS_user_fork, 0, 0, 0, 0, 0, 0, 0);
}

void yield() {
  do_user_call(SYS_user_yield, 0, 0, 0, 0, 0, 0, 0);
}

int exec(const char *path, const char *arg) {
  return do_user_call(SYS_user_exec, (uint64)path, (uint64)arg, 0, 0, 0, 0, 0);
}

int wait(int pid) {
  return do_user_call(SYS_user_wait, pid, 0, 0, 0, 0, 0, 0);
}

void print_backtrace(int depth) {
  do_user_call(SYS_user_backtrace, depth, 0, 0, 0, 0, 0, 0);
}

void printpa(int* va) {
  do_user_call(SYS_user_printpa, (uint64)va, 0, 0, 0, 0, 0, 0);
}

int sem_new(int init_value) {
  return do_user_call(SYS_user_sem_new, init_value, 0, 0, 0, 0, 0, 0);
}

int sem_free(int sem_id) {
  return do_user_call(SYS_user_sem_free, sem_id, 0, 0, 0, 0, 0, 0);
}

void sem_P(int sem_id) {
  do_user_call(SYS_user_sem_P, sem_id, 0, 0, 0, 0, 0, 0);
}

void sem_V(int sem_id) {
  do_user_call(SYS_user_sem_V, sem_id, 0, 0, 0, 0, 0, 0);
}

int open(const char *pathname, int flags) {
  return do_user_call(SYS_user_open, (uint64)pathname, flags, 0, 0, 0, 0, 0);
}

int read_u(int fd, void * buf, uint64 count){
  return do_user_call(SYS_user_read, fd, (uint64)buf, count, 0, 0, 0, 0);
}

int write_u(int fd, void *buf, uint64 count) {
  return do_user_call(SYS_user_write, fd, (uint64)buf, count, 0, 0, 0, 0);
}

int lseek_u(int fd, int offset, int whence) {
  return do_user_call(SYS_user_lseek, fd, offset, whence, 0, 0, 0, 0);
}

int stat_u(int fd, struct istat *istat) {
  return do_user_call(SYS_user_stat, fd, (uint64)istat, 0, 0, 0, 0, 0);
}

int disk_stat_u(int fd, struct istat *istat) {
  return do_user_call(SYS_user_disk_stat, fd, (uint64)istat, 0, 0, 0, 0, 0);
}

int opendir_u(const char *dirname) {
  return do_user_call(SYS_user_opendir, (uint64)dirname, 0, 0, 0, 0, 0, 0);
}

int readdir_u(int fd, struct dir *dir) {
  return do_user_call(SYS_user_readdir, fd, (uint64)dir, 0, 0, 0, 0, 0);
}

int mkdir_u(const char *pathname) {
  return do_user_call(SYS_user_mkdir, (uint64)pathname, 0, 0, 0, 0, 0, 0);
}

int closedir_u(int fd) {
  return do_user_call(SYS_user_closedir, fd, 0, 0, 0, 0, 0, 0);
}

int link_u(const char *fn1, const char *fn2){
  return do_user_call(SYS_user_link, (uint64)fn1, (uint64)fn2, 0, 0, 0, 0, 0);
}

int unlink_u(const char *fn){
  return do_user_call(SYS_user_unlink, (uint64)fn, 0, 0, 0, 0, 0, 0);
}

int close(int fd) {
  return do_user_call(SYS_user_close, fd, 0, 0, 0, 0, 0, 0);
}

int read_cwd(char *path) {
  return do_user_call(SYS_user_rcwd, (uint64)path, 0, 0, 0, 0, 0, 0);
}

int change_cwd(const char *path) {
  return do_user_call(SYS_user_ccwd, (uint64)path, 0, 0, 0, 0, 0, 0);
}

int pipe_u(int fds[2]) {
  return do_user_call(SYS_user_pipe, (uint64)fds, 0, 0, 0, 0, 0, 0);
}

int set_stdin(int fd) {
  return do_user_call(SYS_user_set_stdin, fd, 0, 0, 0, 0, 0, 0);
}

int set_stdout(int fd) {
  return do_user_call(SYS_user_set_stdout, fd, 0, 0, 0, 0, 0, 0);
}

int read_stdin(void *buf, uint64 count) {
  return do_user_call(SYS_user_read_stdin, (uint64)buf, count, 0, 0, 0, 0, 0);
}

int set_affinity(int hartid) {
  return do_user_call(SYS_user_set_affinity, hartid, 0, 0, 0, 0, 0, 0);
}

int set_quiet(int on) {
  return do_user_call(SYS_user_set_quiet, on, 0, 0, 0, 0, 0, 0);
}
