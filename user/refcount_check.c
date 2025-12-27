#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

int
main(int argc, char *argv[])
{
  // Just print a message and exit
  printf("Checking refcounts...\n");
  
  // Fork a child
  int pid = fork();
  if(pid == 0) {
    // Child
    printf("Child exiting\n");
    exit(0);
  } else {
    // Parent
    wait(0);
    printf("Parent done\n");
  }
  
  exit(0);
}
