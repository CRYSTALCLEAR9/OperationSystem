#include "user_lib.h"
#include "util/types.h"

int main(void) {
  printu("======== Shell Bonus Demo ========\n\n");
  if (exec("/bin/app_shell", "/shellrc_bonus") == -1) {
    printu("exec failed!\n");
    exit(-1);
  }
  exit(0);
  return 0;
}
