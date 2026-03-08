#include "user_lib.h"
#include "util/types.h"

int main(void) {
  char buf[128];
  int n;

  while ((n = read_stdin(buf, sizeof(buf) - 1)) > 0) {
    for (int i = 0; i < n; i++) {
      if (buf[i] >= 'a' && buf[i] <= 'z') buf[i] = buf[i] - 'a' + 'A';
    }
    buf[n] = '\0';
    printu("%s", buf);
  }

  exit(0);
  return 0;
}
