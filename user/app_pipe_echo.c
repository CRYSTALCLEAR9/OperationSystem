#include "user_lib.h"
#include "util/types.h"

int main(int argc, char *argv[]) {
  char *msg = argc > 0 ? argv[0] : "pipe demo";
  printu("%s\n", msg);
  exit(0);
  return 0;
}
