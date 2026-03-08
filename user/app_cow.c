/*
 * This app checks copy-on-write on heap pages after fork.
 */

#include "user_lib.h"
#include "util/types.h"

int main(void) {
  int *heap_data = naive_malloc();
  printu("the physical address of parent process heap is: ");
  printpa(heap_data);
  int pid = fork();
  if (pid == 0) {
    printu("the physical address of child process heap before copy on write is: ");
    printpa(heap_data);
    heap_data[0] = 0;
    printu("the physical address of child process heap after copy on write is: ");
    printpa(heap_data);
  }
  exit(0);
  return 0;
}
