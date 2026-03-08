#include "user_lib.h"
#include "util/types.h"

int main(int argc, char *argv[]) {
  char *tag = argc > 0 ? argv[0] : "worker";
  for (int i = 0; i < 5; i++) {
    printu("[bg %s] round %d\n", tag, i);
    yield();
  }
  exit(0);
  return 0;
}
