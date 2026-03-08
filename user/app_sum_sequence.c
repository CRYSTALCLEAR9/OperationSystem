/*
 * Based on lab2_4: trigger stack growth and then an invalid heap access.
 */

#include "user_lib.h"
#include "util/types.h"

uint64 sum_sequence(uint64 n, int *p) {
  if (n == 0)
    return 0;
  else
    return *p = sum_sequence(n - 1, p + 1) + n;
}

int main(void) {
  uint64 n = 1024;
  int *ans = (int *)naive_malloc();

  printu("Summation of an arithmetic sequence from 0 to %ld is: %ld \n",
         n, sum_sequence(n + 1, ans));

  exit(0);
}
