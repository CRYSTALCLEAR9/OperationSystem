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

static void parse_profile(const char *arg, int *workers, int *rounds, int *work) {
  *workers = 8;
  *rounds = 10;
  *work = 4;
  if (arg == 0 || arg[0] == '\0') return;

  const char *cursor = arg;
  int value = 0;
  if (parse_next_int(&cursor, &value)) *workers = value;
  if (parse_next_int(&cursor, &value)) *rounds = value;
  if (parse_next_int(&cursor, &value)) *work = value;

  if (*workers < 1) *workers = 1;
  if (*workers > 12) *workers = 12;
  if (*rounds < 1) *rounds = 1;
  if (*rounds > 64) *rounds = 64;
  if (*work < 1) *work = 1;
  if (*work > 16) *work = 16;
}

int main(int argc, char *argv[]) {
  int workers, rounds, work;
  parse_profile(argc > 0 ? argv[0] : 0, &workers, &rounds, &work);

  printu("proc-stress profile workers=%d rounds=%d work=%d\n", workers, rounds, work);

  for (int round = 0; round < rounds; round++) {
    int launched = 0;
    printu("proc-stress round %d start\n", round);

    for (int index = 0; index < workers; index++) {
      int pid = fork();
      if (pid == 0) {
        for (int iter = 0; iter < work; iter++) {
          int *page = (int *)naive_malloc();
          if (page) {
            page[0] = (round << 8) | index;
            naive_free(page);
          }
          yield();
        }
        exit(0);
      }
      launched++;
    }

    int waited = 0;
    while (waited < launched) {
      int pid = wait(-1);
      if (pid < 0) {
        printu("proc-stress fail: wait returns -1 at round %d (waited=%d launched=%d)\n",
               round, waited, launched);
        exit(-1);
      }
      waited++;
    }

    printu("proc-stress round %d done (%d/%d)\n", round, waited, launched);
  }

  printu("proc-stress PASS rounds=%d workers=%d\n", rounds, workers);
  exit(0);
  return 0;
}
