#include "user_lib.h"
#include "util/string.h"
#include "util/types.h"

int main(int argc, char *argv[]) {
  char *path = argc > 0 ? argv[0] : "/RAMDISK0/stress_io.txt";
  char write_buf[64];
  char read_buf[128];

  int fd = open(path, O_RDWR | O_CREAT);
  if (fd < 0) {
    printu("stress-io: open failed %s\n", path);
    exit(-1);
  }

  for (int i = 0; i < 16; i++) {
    safestrcpy(write_buf, "round ", sizeof(write_buf));
    char digit[8];
    digit[0] = '0' + (i / 10);
    digit[1] = '0' + (i % 10);
    digit[2] = '\n';
    digit[3] = '\0';
    strcat(write_buf, digit);
    lseek_u(fd, 0, SEEK_SET);
    write_u(fd, write_buf, strlen(write_buf));
  }

  lseek_u(fd, 0, SEEK_SET);
  int n = read_u(fd, read_buf, sizeof(read_buf) - 1);
  if (n < 0) n = 0;
  read_buf[n] = '\0';
  close(fd);

  printu("stress-io: %s => %s", path, read_buf);
  exit(0);
  return 0;
}
