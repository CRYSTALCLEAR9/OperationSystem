#include "user_lib.h"
#include "util/string.h"
#include "util/types.h"

static int parse_next_int(const char **cursor, int *out) {
  const char *p = *cursor;
  if (p == 0 || *p == '\0') return 0;
  if (*p < '0' || *p > '9') return 0;

  int value = 0;
  while (*p >= '0' && *p <= '9') {
    value = value * 10 + (*p - '0');
    p++;
  }

  if (*p == ',' || *p == ':' || *p == '/' || *p == 'x') p++;
  *cursor = p;
  *out = value;
  return 1;
}

static void parse_profile(const char *arg, int *files, int *rounds) {
  *files = 4;
  *rounds = 30;
  if (arg == 0 || arg[0] == '\0') return;

  const char *cursor = arg;
  int value = 0;
  if (parse_next_int(&cursor, &value)) *files = value;
  if (parse_next_int(&cursor, &value)) *rounds = value;

  if (*files < 1) *files = 1;
  if (*files > 12) *files = 12;
  if (*rounds < 1) *rounds = 1;
  if (*rounds > 200) *rounds = 200;
}

static void int_to_str(int value, char *buf) {
  if (value == 0) {
    buf[0] = '0';
    buf[1] = '\0';
    return;
  }

  char tmp[16];
  int n = 0;
  while (value > 0 && n < (int)sizeof(tmp)) {
    tmp[n++] = '0' + (value % 10);
    value /= 10;
  }

  for (int i = 0; i < n; i++) buf[i] = tmp[n - 1 - i];
  buf[n] = '\0';
}

static void build_path(char *path, int path_size, int file_id) {
  char id[16];
  int_to_str(file_id, id);
  safestrcpy(path, "/RAMDISK0/sio", path_size);
  if (strlen(path) + strlen(id) < path_size) strcat(path, id);
}

static void build_payload(char *payload, int payload_size, int round_id, int file_id) {
  char round[16], file[16];
  int_to_str(round_id, round);
  int_to_str(file_id, file);
  safestrcpy(payload, "R", payload_size);
  if (strlen(payload) + strlen(round) < payload_size) strcat(payload, round);
  if (strlen(payload) + 2 < payload_size) strcat(payload, "_F");
  if (strlen(payload) + strlen(file) < payload_size) strcat(payload, file);
}

int main(int argc, char *argv[]) {
  int files, rounds;
  parse_profile(argc > 0 ? argv[0] : 0, &files, &rounds);

  printu("io-stress profile files=%d rounds=%d\n", files, rounds);
  int errors = 0;

  for (int round = 0; round < rounds; round++) {
    for (int file_id = 0; file_id < files; file_id++) {
      char path[30];
      char expected[32];
      char actual[64];

      build_path(path, sizeof(path), file_id);
      build_payload(expected, sizeof(expected), round, file_id);

      int fd = open(path, O_RDWR | O_CREAT);
      if (fd < 0) {
        printu("io-stress open failed: %s\n", path);
        errors++;
        continue;
      }

      lseek_u(fd, 0, SEEK_SET);
      int wn = write_u(fd, expected, strlen(expected));
      if (wn < 0) {
        printu("io-stress write failed: %s\n", path);
        errors++;
        close(fd);
        continue;
      }

      lseek_u(fd, 0, SEEK_SET);
      int rn = read_u(fd, actual, sizeof(actual) - 1);
      if (rn < 0) {
        printu("io-stress read failed: %s\n", path);
        errors++;
        close(fd);
        continue;
      }
      actual[rn] = '\0';

      if (strncmp(actual, expected, strlen(expected)) != 0) {
        printu("io-stress mismatch %s expect=%s got=%s\n", path, expected, actual);
        errors++;
      }

      close(fd);
    }
  }

  int total_ops = files * rounds;
  if (errors == 0) {
    printu("io-stress PASS ops=%d\n", total_ops);
    exit(0);
  } else {
    printu("io-stress FAIL ops=%d errors=%d\n", total_ops, errors);
    exit(-1);
  }
  return 0;
}
