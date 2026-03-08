#include "user_lib.h"
#include "util/string.h"
#include "util/types.h"

int main(int argc, char *argv[]) {
  int workers = argc > 0 ? atol(argv[0]) : 6;
  if (workers < 1) workers = 1;
  if (workers > 12) workers = 12;

  printu("stress-proc: start %d workers\n", workers);
  for (int i = 0; i < workers; i++) {
    int pid = fork();
    if (pid == 0) {
      for (int round = 0; round < 3; round++) {
        printu("worker %d round %d\n", i, round);
        yield();
      }
      exit(0);
    }
  }

  for (int i = 0; i < workers; i++) wait(-1);
  printu("stress-proc: done\n");
  exit(0);
  return 0;
}
