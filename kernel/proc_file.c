/*
 * Interface functions between file system and kernel/processes. added @lab4_1
 */

#include "proc_file.h"

#include "hostfs.h"
#include "pipe.h"
#include "pmm.h"
#include "process.h"
#include "ramdev.h"
#include "rfs.h"
#include "riscv.h"
#include "spike_interface/spike_file.h"
#include "spike_interface/spike_utils.h"
#include "util/functions.h"
#include "util/string.h"

static int file_kind(struct file *file) {
  if (file->offset == FILE_KIND_PIPE_READ) return FILE_KIND_PIPE_READ;
  if (file->offset == FILE_KIND_PIPE_WRITE) return FILE_KIND_PIPE_WRITE;
  return FILE_KIND_VFS;
}

static pipe_t *file_pipe(struct file *file) {
  return (pipe_t *)file->f_dentry;
}

static int alloc_fd_slot(proc_file_management *pfiles) {
  if (pfiles->nfiles >= MAX_FILES) return -1;

  for (int fd = 0; fd < MAX_FILES; ++fd) {
    if (pfiles->opened_files[fd].status == FD_NONE) return fd;
  }
  return -1;
}

//
// initialize file system
//
void fs_init(void) {
  vfs_init();

  if (register_hostfs() < 0) panic("fs_init: cannot register hostfs.\n");
  struct device *hostdev = init_host_device("HOSTDEV");
  vfs_mount("HOSTDEV", MOUNT_AS_ROOT);

  if (register_rfs() < 0) panic("fs_init: cannot register rfs.\n");
  struct device *ramdisk0 = init_rfs_device("RAMDISK0");
  rfs_format_dev(ramdisk0);
  vfs_mount("RAMDISK0", MOUNT_DEFAULT);
}

//
// initialize a proc_file_management data structure for a process.
// return the pointer to the page containing the data structure.
//
proc_file_management *init_proc_file_management(void) {
  proc_file_management *pfiles = (proc_file_management *)alloc_page();
  memset(pfiles, 0, sizeof(*pfiles));
  pfiles->cwd = vfs_root_dentry;

  for (int fd = 0; fd < MAX_FILES; ++fd) pfiles->opened_files[fd].status = FD_NONE;

  if (current != NULL) sprint("FS: created a file management struct for a process.\n");
  return pfiles;
}

void copy_proc_file_management(proc_file_management *dst, proc_file_management *src) {
  memset(dst, 0, sizeof(*dst));
  dst->cwd = src->cwd;

  for (int fd = 0; fd < MAX_FILES; ++fd) {
    dst->opened_files[fd].status = FD_NONE;
    if (src->opened_files[fd].status == FD_NONE) continue;

    if (file_kind(&src->opened_files[fd]) == FILE_KIND_PIPE_READ ||
        file_kind(&src->opened_files[fd]) == FILE_KIND_PIPE_WRITE) {
      memcpy(&dst->opened_files[fd], &src->opened_files[fd], sizeof(struct file));
      pipe_incref(file_pipe(&dst->opened_files[fd]));
      dst->nfiles++;
    }
  }
}

//
// reclaim the open-file management data structure of a process.
// note: this function is not used as PKE does not actually reclaim a process.
//
void reclaim_proc_file_management(proc_file_management *pfiles) {
  free_page(pfiles);
}

//
// get an opened file from proc->opened_file array.
// return: the pointer to the opened file structure.
//
static struct file *get_opened_file(int fd) {
  if (fd < 0 || fd >= MAX_FILES) panic("get_opened_file: invalid fd!\n");

  struct file *pfile = &(current->pfiles->opened_files[fd]);
  if (pfile->status == FD_NONE) panic("get_opened_file: unopened fd!\n");
  return pfile;
}

//
// open a file named as "pathname" with the permission of "flags".
// return: -1 on failure; non-zero file-descriptor on success.
//
int do_open(char *pathname, int flags) {
  struct file *opened_file = vfs_open(pathname, flags);
  if (opened_file == NULL) return -1;

  int fd = alloc_fd_slot(current->pfiles);
  if (fd < 0) panic("do_open: no file entry for current process!\n");

  memcpy(&current->pfiles->opened_files[fd], opened_file, sizeof(struct file));
  current->pfiles->nfiles++;
  return fd;
}

//
// read content of a file ("fd") into "buf" for "count".
// return: actual length of data read from the file.
//
int do_read(int fd, char *buf, uint64 count) {
  struct file *pfile = get_opened_file(fd);
  if (pfile->readable == 0) panic("do_read: no readable file!\n");

  if (file_kind(pfile) == FILE_KIND_PIPE_READ) return pipe_read(file_pipe(pfile), buf, count);

  return vfs_read(pfile, buf, count);
}

//
// write content ("buf") whose length is "count" to a file "fd".
// return: actual length of data written to the file.
//
int do_write(int fd, char *buf, uint64 count) {
  struct file *pfile = get_opened_file(fd);
  if (pfile->writable == 0) panic("do_write: cannot write file!\n");

  if (file_kind(pfile) == FILE_KIND_PIPE_WRITE) return pipe_write(file_pipe(pfile), buf, count);

  return vfs_write(pfile, buf, count);
}

int do_lseek(int fd, int offset, int whence) {
  struct file *pfile = get_opened_file(fd);
  return vfs_lseek(pfile, offset, whence);
}

int do_stat(int fd, struct istat *istat) {
  struct file *pfile = get_opened_file(fd);
  return vfs_stat(pfile, istat);
}

int do_disk_stat(int fd, struct istat *istat) {
  struct file *pfile = get_opened_file(fd);
  return vfs_disk_stat(pfile, istat);
}

int do_close(int fd) {
  struct file *pfile = get_opened_file(fd);
  int rc = 0;

  int kind = file_kind(pfile);
  if (kind == FILE_KIND_PIPE_READ || kind == FILE_KIND_PIPE_WRITE) {
    pipe_close_end(file_pipe(pfile), kind);
    pipe_decref(file_pipe(pfile));
  } else {
    rc = vfs_close(pfile);
  }

  pfile->status = FD_NONE;
  pfile->readable = 0;
  pfile->writable = 0;
  pfile->offset = 0;
  pfile->f_dentry = 0;

  if (current->stdin_fd == fd) current->stdin_fd = -1;
  if (current->stdout_fd == fd) current->stdout_fd = -1;
  if (current->pfiles->nfiles > 0) current->pfiles->nfiles--;

  return rc;
}

//
// open a directory
// return: the fd of the directory file
//
int do_opendir(char *pathname) {
  struct file *opened_file = vfs_opendir(pathname);
  if (opened_file == NULL) return -1;

  int fd = alloc_fd_slot(current->pfiles);
  if (fd < 0) panic("do_opendir: no file entry for current process!\n");

  memcpy(&current->pfiles->opened_files[fd], opened_file, sizeof(struct file));
  current->pfiles->nfiles++;
  return fd;
}

int do_readdir(int fd, struct dir *dir) {
  struct file *pfile = get_opened_file(fd);
  return vfs_readdir(pfile, dir);
}

int do_mkdir(char *pathname) {
  return vfs_mkdir(pathname);
}

int do_closedir(int fd) {
  struct file *pfile = get_opened_file(fd);
  int rc = vfs_closedir(pfile);
  pfile->status = FD_NONE;
  pfile->readable = 0;
  pfile->writable = 0;
  pfile->offset = 0;
  pfile->f_dentry = 0;
  if (current->pfiles->nfiles > 0) current->pfiles->nfiles--;
  return rc;
}

int do_link(char *oldpath, char *newpath) {
  return vfs_link(oldpath, newpath);
}

int do_unlink(char *path) {
  return vfs_unlink(path);
}

int do_pipe(int *fds) {
  if (fds == 0) return -1;

  pipe_t *pipe = pipe_alloc();
  if (pipe == 0) return -1;

  int read_fd = alloc_fd_slot(current->pfiles);
  if (read_fd < 0) {
    pipe_decref(pipe);
    return -1;
  }

  current->pfiles->opened_files[read_fd].status = FD_OPENED;
  current->pfiles->opened_files[read_fd].readable = 1;
  current->pfiles->opened_files[read_fd].writable = 0;
  current->pfiles->opened_files[read_fd].offset = FILE_KIND_PIPE_READ;
  current->pfiles->opened_files[read_fd].f_dentry = (struct dentry *)pipe;
  current->pfiles->nfiles++;

  int write_fd = alloc_fd_slot(current->pfiles);
  if (write_fd < 0) {
    do_close(read_fd);
    return -1;
  }

  current->pfiles->opened_files[write_fd].status = FD_OPENED;
  current->pfiles->opened_files[write_fd].readable = 0;
  current->pfiles->opened_files[write_fd].writable = 1;
  current->pfiles->opened_files[write_fd].offset = FILE_KIND_PIPE_WRITE;
  current->pfiles->opened_files[write_fd].f_dentry = (struct dentry *)pipe;
  current->pfiles->nfiles++;

  fds[0] = read_fd;
  fds[1] = write_fd;
  return 0;
}
