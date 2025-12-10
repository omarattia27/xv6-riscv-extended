#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

// Test 1: Basic COW - parent and child write to different pages
void
test_basic_cow(void)
{
  printf("test_basic_cow: ");
  
  char *buf = sbrk(4096 * 3);
  if(buf == (char*)-1) {
    printf("FAIL - sbrk failed\n");
    return;
  }
  
  for(int i = 0; i < 4096 * 3; i++)
    buf[i] = 'A';
  
  int pid = fork();
  if(pid < 0) {
    printf("FAIL - fork failed\n");
    return;
  }
  
  if(pid == 0) {
    // child: write to first page
    buf[0] = 'C';
    buf[1] = 'C';
    if(buf[0] != 'C' || buf[1] != 'C') {
      printf("FAIL - child write failed\n");
      exit(1);
    }
    exit(0);
  } else {
    // parent: write to last page
    buf[4096*2] = 'P';
    buf[4096*2 + 1] = 'P';
    
    wait(0);
    
    // parent should still see its own writes
    if(buf[4096*2] != 'P' || buf[4096*2 + 1] != 'P') {
      printf("FAIL - parent data corrupted\n");
      return;
    }
    
    // parent should not see child's writes (COW worked)
    if(buf[0] != 'A' || buf[1] != 'A') {
      printf("FAIL - COW didn't isolate child writes\n");
      return;
    }
  }
  
  printf("OK\n");
}

// Test 2: Multiple children writing to same page
void
test_multi_child_cow(void)
{
  printf("test_multi_child_cow: ");
  
  char *buf = sbrk(4096);
  if(buf == (char*)-1) {
    printf("FAIL - sbrk failed\n");
    return;
  }
  
  for(int i = 0; i < 4096; i++)
    buf[i] = 'X';
  
  int n_children = 5;
  for(int i = 0; i < n_children; i++) {
    int pid = fork();
    if(pid < 0) {
      printf("FAIL - fork %d failed\n", i);
      return;
    }
    if(pid == 0) {
      // each child writes its id
      buf[0] = '0' + i;
      if(buf[0] != '0' + i) {
        printf("FAIL - child %d write failed\n", i);
        exit(1);
      }
      exit(0);
    }
  }
  
  // parent waits for all children
  for(int i = 0; i < n_children; i++)
    wait(0);
  
  // parent should still see original data
  if(buf[0] != 'X') {
    printf("FAIL - parent data corrupted by children\n");
    return;
  }
  
  printf("OK\n");
}

// Test 3: Nested forks
void
test_nested_fork(void)
{
  printf("test_nested_fork: ");
  
  char *buf = sbrk(4096);
  if(buf == (char*)-1) {
    printf("FAIL - sbrk failed\n");
    return;
  }
  
  buf[0] = 'G';  // grandparent
  
  int pid1 = fork();
  if(pid1 < 0) {
    printf("FAIL - first fork failed\n");
    return;
  }
  
  if(pid1 == 0) {
    // child (parent of grandchild)
    buf[0] = 'P';
    
    int pid2 = fork();
    if(pid2 < 0) {
      printf("FAIL - second fork failed\n");
      exit(1);
    }
    
    if(pid2 == 0) {
      // grandchild
      buf[0] = 'C';
      if(buf[0] != 'C') {
        printf("FAIL - grandchild write failed\n");
        exit(1);
      }
      exit(0);
    } else {
      wait(0);
      // parent should still see 'P'
      if(buf[0] != 'P') {
        printf("FAIL - parent corrupted by grandchild\n");
        exit(1);
      }
      exit(0);
    }
  } else {
    wait(0);
    // grandparent should still see 'G'
    if(buf[0] != 'G') {
      printf("FAIL - grandparent corrupted\n");
      return;
    }
  }
  
  printf("OK\n");
}

// Test 4: Read-only pages don't get copied unnecessarily
void
test_readonly_no_copy(void)
{
  printf("test_readonly_no_copy: ");
  
  char *buf = sbrk(4096);
  if(buf == (char*)-1) {
    printf("FAIL - sbrk failed\n");
    return;
  }
  
  for(int i = 0; i < 4096; i++)
    buf[i] = 'R';
  
  int pid = fork();
  if(pid < 0) {
    printf("FAIL - fork failed\n");
    return;
  }
  
  if(pid == 0) {
    // child: only read, no write
    char c = buf[100];
    if(c != 'R') {
      printf("FAIL - child read wrong value\n");
      exit(1);
    }
    exit(0);
  } else {
    wait(0);
    // parent can still read
    if(buf[100] != 'R') {
      printf("FAIL - parent read failed\n");
      return;
    }
  }
  
  printf("OK\n");
}

// Test 5: Writing to multiple pages
void
test_multi_page_write(void)
{
  printf("test_multi_page_write: ");
  
  int npages = 10;
  char *buf = sbrk(4096 * npages);
  if(buf == (char*)-1) {
    printf("FAIL - sbrk failed\n");
    return;
  }
  
  // initialize all pages
  for(int i = 0; i < npages; i++)
    buf[i * 4096] = 'A' + i;
  
  int pid = fork();
  if(pid < 0) {
    printf("FAIL - fork failed\n");
    return;
  }
  
  if(pid == 0) {
    // child: write to every other page
    for(int i = 0; i < npages; i += 2) {
      buf[i * 4096] = 'a' + i;
    }
    
    // verify child writes
    for(int i = 0; i < npages; i += 2) {
      if(buf[i * 4096] != 'a' + i) {
        printf("FAIL - child write verification failed\n");
        exit(1);
      }
    }
    
    // verify unwritten pages
    for(int i = 1; i < npages; i += 2) {
      if(buf[i * 4096] != 'A' + i) {
        printf("FAIL - child unwritten page corrupted\n");
        exit(1);
      }
    }
    exit(0);
  } else {
    wait(0);
    
    // parent should see original data
    for(int i = 0; i < npages; i++) {
      if(buf[i * 4096] != 'A' + i) {
        printf("FAIL - parent page %d corrupted\n", i);
        return;
      }
    }
  }
  
  printf("OK\n");
}

// Test 6: Large memory allocation and fork
void
test_large_memory(void)
{
  printf("test_large_memory: ");
  
  // allocate many pages
  int npages = 50;
  char *buf = sbrk(4096 * npages);
  if(buf == (char*)-1) {
    printf("FAIL - sbrk failed\n");
    return;
  }
  
  // write to first and last byte of each page
  for(int i = 0; i < npages; i++) {
    buf[i * 4096] = i & 0xFF;
    buf[i * 4096 + 4095] = (i + 128) & 0xFF;
  }
  
  int pid = fork();
  if(pid < 0) {
    printf("FAIL - fork failed\n");
    return;
  }
  
  if(pid == 0) {
    // child: verify it can see all pages
    for(int i = 0; i < npages; i++) {
      if(buf[i * 4096] != (i & 0xFF)) {
        printf("FAIL - child verification failed at page %d\n", i);
        exit(1);
      }
    }
    
    // write to some pages
    for(int i = 0; i < npages; i += 5) {
      buf[i * 4096] = 0xCC;
    }
    exit(0);
  } else {
    wait(0);
    
    // parent should still see original data
    for(int i = 0; i < npages; i++) {
      if(buf[i * 4096] != (i & 0xFF)) {
        printf("FAIL - parent page %d corrupted\n", i);
        return;
      }
    }
  }
  
  printf("OK\n");
}

// Test 7: Child continues after parent frees memory
void
test_parent_exit_first(void)
{
  printf("test_parent_exit_first: ");
  
  char *buf = sbrk(4096);
  if(buf == (char*)-1) {
    printf("FAIL - sbrk failed\n");
    return;
  }
  buf[0] = 'X';
  
  int pid = fork();
  if(pid < 0) {
    printf("FAIL - fork failed\n");
    return;
  }
  
  if(pid == 0) {
    // child writes after fork
    buf[0] = 'C';
    if(buf[0] != 'C') {
      printf("FAIL - child write failed\n");
      exit(1);
    }
    exit(0);
  } else {
    // parent waits for child
    wait(0);
    // parent should still see 'X'
    if(buf[0] != 'X') {
      printf("FAIL - parent corrupted\n");
      return;
    }
  }
  
  printf("OK\n");
}

// Test 8: Rapid fork/exit cycles
void
test_rapid_fork(void)
{
  printf("test_rapid_fork: ");
  
  char *buf = sbrk(4096);
  if(buf == (char*)-1) {
    printf("FAIL - sbrk failed\n");
    return;
  }
  buf[0] = 'X';
  
  for(int i = 0; i < 20; i++) {
    int pid = fork();
    if(pid < 0) {
      printf("FAIL - fork %d failed\n", i);
      return;
    }
    if(pid == 0) {
      buf[0] = 'Y';
      exit(0);
    }
    wait(0);
  }
  
  if(buf[0] != 'X') {
    printf("FAIL - parent data corrupted\n");
    return;
  }
  
  printf("OK\n");
}

int
main(int argc, char *argv[])
{
  printf("COW fork tests starting\n");
  
  test_basic_cow();
  test_multi_child_cow();
  test_nested_fork();
  test_readonly_no_copy();
  test_multi_page_write();
  test_large_memory();
  test_parent_exit_first();
  test_rapid_fork();
  
  printf("ALL COW TESTS PASSED\n");
  exit(0);
}
