/*
 * This app checks wait() and process-private globals.
 */

#include "user_lib.h"
#include "util/types.h"

int flag;

int main(void) {
  flag = 0;
  int pid = fork();
  if (pid == 0) {
    flag = 1;
    pid = fork();
    if (pid == 0) {
      flag = 2;
      printu("Grandchild process end, flag = %d.\n", flag);
    } else {
      wait(pid);
      printu("Child process end, flag = %d.\n", flag);
    }
  } else {
    wait(-1);
    printu("Parent process end, flag = %d.\n", flag);
  }
  exit(0);
  return 0;
}
