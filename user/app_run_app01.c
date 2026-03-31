/*
 * Run app0 on hart0 and app1 on hart1, then wait for both.
 */

#include "user_lib.h"

int main(void) {
  set_affinity(0);

  int pid0 = fork();
  if (pid0 == 0) {
    if (set_affinity(0) < 0) {
      printu("app_run_app01: need at least 1 hart\n");
      exit(-1);
    }
    yield();
    if (exec("/bin/app0", 0) == -1) {
      printu("exec failed: /bin/app0\n");
      exit(-1);
    }
    exit(0);
  }

  int pid1 = fork();
  if (pid1 == 0) {
    if (set_affinity(1) < 0) {
      printu("app_run_app01: start with `-p2` to run app1 on hart1\n");
      exit(-1);
    }
    yield();
    if (exec("/bin/app1", 0) == -1) {
      printu("exec failed: /bin/app1\n");
      exit(-1);
    }
    exit(0);
  }

  wait(pid0);
  wait(pid1);
  exit(0);
  return 0;
}
